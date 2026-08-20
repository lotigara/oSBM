#pragma once

#include "StarString.hpp"

// Crash-survivable run state, for unattended testing on devices where a crash
// leaves nothing behind.
//
// The game log is buffered and, more importantly, cannot distinguish "the
// process exited cleanly" from "the process died mid-frame" -- both simply
// stop. This writes a small fsync'd file after every update so a host can read
// the exact state the run was in when it stopped, and tell a crash from a
// clean exit by whether the file was ever marked finished.
//
// Inert until diagnosticsSetHeartbeatPath() is called, so desktop builds and
// anyone not running a test pay nothing but an atomic load.
namespace Star {

// Enables heartbeat writing to the given file. Called once at startup by the
// platform layer that knows where writable storage lives.
void diagnosticsSetHeartbeatPath(String const& path);

// What the process is doing right now, e.g. "world:CelestialWorld:123" or
// "loading:ObjectDatabase". Cheap; overwritten freely.
void diagnosticsSetActivity(String const& activity);

// Records a scenario step so a host can see how far an unattended run got.
void diagnosticsNoteStep(String const& step);

// Writes the current snapshot (uptime, memory, activity, step, counters).
// Called from the maintenance thread; safe to call from any thread.
void diagnosticsWriteHeartbeat();

// Marks the run as having ended on purpose. A heartbeat file without this is
// how a host recognises a crash.
void diagnosticsMarkCleanExit();

}
