#include "StarDiagnostics.hpp"
#include "StarFormat.hpp"
#include "StarMemoryUsage.hpp"
#include "StarThread.hpp"
#include "StarTime.hpp"

#include <atomic>
#include <cstdio>

#if defined(STAR_SYSTEM_SWITCH) || defined(STAR_SYSTEM_FAMILY_UNIX) || defined(STAR_SYSTEM_ANDROID)
#include <fcntl.h>
#include <unistd.h>
#define STAR_DIAGNOSTICS_RAW_IO
#endif

namespace Star {

namespace {
  // Raw fd writes rather than File/ofstream: this has to work while the
  // process is in trouble, and the whole point is that the bytes are on disk
  // before the next thing happens.
  Mutex& diagnosticsMutex() {
    static Mutex mutex;
    return mutex;
  }

  String& heartbeatPath() {
    static String path;
    return path;
  }

  String& currentActivity() {
    static String activity = "starting";
    return activity;
  }

  String& currentStep() {
    static String step = "-";
    return step;
  }

  std::atomic<bool> s_enabled{false};
  std::atomic<uint64_t> s_heartbeatCount{0};
  double s_startTime = 0.0;

  void writeFileAtomically(String const& path, String const& contents) {
#ifdef STAR_DIAGNOSTICS_RAW_IO
    int fd = open(path.utf8Ptr(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
      return;
    auto const& utf8 = contents.utf8();
    ssize_t written = write(fd, utf8.data(), utf8.size());
    (void)written;
    fsync(fd);
    close(fd);
#else
    if (FILE* f = fopen(path.utf8Ptr(), "wb")) {
      fwrite(contents.utf8Ptr(), 1, contents.utf8Size(), f);
      fflush(f);
      fclose(f);
    }
#endif
  }
}

void diagnosticsSetHeartbeatPath(String const& path) {
  MutexLocker locker(diagnosticsMutex());
  heartbeatPath() = path;
  s_startTime = Time::monotonicTime();
  s_enabled.store(!path.empty(), std::memory_order_release);
}

void diagnosticsSetActivity(String const& activity) {
  if (!s_enabled.load(std::memory_order_acquire))
    return;
  MutexLocker locker(diagnosticsMutex());
  currentActivity() = activity;
}

void diagnosticsNoteStep(String const& step) {
  if (!s_enabled.load(std::memory_order_acquire))
    return;
  MutexLocker locker(diagnosticsMutex());
  currentStep() = step;
}

void diagnosticsWriteHeartbeat() {
  if (!s_enabled.load(std::memory_order_acquire))
    return;

  auto usage = memoryUsage();
  uint64_t beat = s_heartbeatCount.fetch_add(1, std::memory_order_relaxed) + 1;

  MutexLocker locker(diagnosticsMutex());
  // Deliberately flat "key=value" lines rather than JSON: a partially written
  // heartbeat still parses line by line, and there is no escaping to get wrong
  // while the process is unhealthy.
  String contents = strf(
      "state=running\n"
      "beat={}\n"
      "uptimeSeconds={:.1f}\n"
      "memoryUsedMB={}\n"
      "memoryBudgetMB={}\n"
      "memoryPercent={:.1f}\n"
      "assetCacheMB={}\n"
      "textureMB={}\n"
      "activity={}\n"
      "step={}\n",
      beat, Time::monotonicTime() - s_startTime,
      usage.used / (1024 * 1024), usage.budget / (1024 * 1024),
      usage.fraction() * 100.0f,
      memoryAccountBytes(MemoryCategory::AssetCache) / (1024 * 1024),
      memoryAccountBytes(MemoryCategory::TextureAtlas) / (1024 * 1024),
      currentActivity(), currentStep());

  writeFileAtomically(heartbeatPath(), contents);
}

void diagnosticsMarkCleanExit() {
  if (!s_enabled.load(std::memory_order_acquire))
    return;

  MutexLocker locker(diagnosticsMutex());
  String contents = strf(
      "state=clean-exit\n"
      "beat={}\n"
      "uptimeSeconds={:.1f}\n"
      "activity={}\n"
      "step={}\n",
      s_heartbeatCount.load(std::memory_order_relaxed),
      Time::monotonicTime() - s_startTime, currentActivity(), currentStep());
  writeFileAtomically(heartbeatPath(), contents);
}

}
