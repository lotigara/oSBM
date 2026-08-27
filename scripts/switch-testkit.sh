#!/usr/bin/env bash
#
# Unattended-test helper for the Nintendo Switch build.
#
# Preferred transport is sys-ftpd (background sysmodule, port 5000) so logs and
# new NROs can move while the game is running. MTP (haze.nro) is the fallback
# and only works when that applet is in the foreground.
#
#   switch-testkit.sh push <nro> [scenario]  copy a build + autopilot config
#   switch-testkit.sh collect [label]        pull logs/crash/heartbeat
#   switch-testkit.sh reload                 ask a running build to chainload
#   switch-testkit.sh status                 latest collected run
#   switch-testkit.sh discover               find sys-ftpd on the LAN
#
set -uo pipefail

MTP_ROOT="mtp:/Nintendo Switch/SD Card/switch"
OSBM_DIR="$MTP_ROOT/oSBM"
RUNS_DIR="${SWITCH_TESTKIT_RUNS:-$HOME/.cache/osbm-switch-runs}"
FTP_PORT="${SWITCH_FTP_PORT:-5000}"
HOST_FILE="$RUNS_DIR/switch-ftp.host"

die() { echo "error: $*" >&2; exit 1; }

ftp_host() {
  if [[ -n "${SWITCH_FTP_HOST:-}" ]]; then
    printf '%s\n' "$SWITCH_FTP_HOST"
    return 0
  fi
  [[ -s "$HOST_FILE" ]] || return 1
  cat "$HOST_FILE"
}

ftp_url() {
  local host; host="$(ftp_host)" || return 1
  printf 'ftp://%s:%s' "$host" "$FTP_PORT"
}

ftp_ok() {
  local base; base="$(ftp_url)" || return 1
  curl -sS --connect-timeout 2 --max-time 4 --disable-epsv "$base/" >/dev/null 2>&1
}

cmd_discover() {
  echo "scanning 192.168.1.0/24 for tcp/$FTP_PORT ..."
  mkdir -p "$RUNS_DIR"
  local found=""
  local i
  for i in $(seq 1 254); do
    (timeout 0.25 bash -c "echo >/dev/tcp/192.168.1.$i/$FTP_PORT" 2>/dev/null && echo "192.168.1.$i") &
  done >"$RUNS_DIR/.ftp-scan.$$"
  wait
  found="$(head -1 "$RUNS_DIR/.ftp-scan.$$" 2>/dev/null || true)"
  rm -f "$RUNS_DIR/.ftp-scan.$$"
  [[ -n "$found" ]] || die "no host with port $FTP_PORT open. Is sys-ftpd running (reboot once after install) and is the Switch on Wi-Fi?"
  printf '%s\n' "$found" > "$HOST_FILE"
  echo "sys-ftpd at $found:$FTP_PORT (saved $HOST_FILE)"
  curl -sS --connect-timeout 3 --disable-epsv "$(ftp_url)/" | head
}

need_transport() {
  if ftp_ok; then
    echo "transport=ftp $(ftp_host):$FTP_PORT"
    return 0
  fi
  if kioclient ls "$MTP_ROOT/" >/dev/null 2>&1; then
    echo "transport=mtp"
    return 0
  fi
  die "neither sys-ftpd nor MTP is reachable. Reboot the Switch so sys-ftpd starts, or open haze.nro."
}

ftp_put() {
  local src="$1" dest="$2"
  curl -sS --connect-timeout 5 --max-time 120 --disable-epsv -T "$src" "$(ftp_url)$dest" >/dev/null
}

ftp_get() {
  local src="$1" dest="$2"
  curl -sS --connect-timeout 5 --max-time 60 --disable-epsv --ignore-content-length "$(ftp_url)$src" -o "$dest"
}

ftp_del() {
  local path="$1"
  curl -sS --connect-timeout 3 --quote "DELE $path" "$(ftp_url)/" >/dev/null 2>&1 || true
}

write_flags() {
  local scenario="${1:-}"
  local flag_body
  if [[ -n "$scenario" ]]; then
    flag_body="$scenario"
  else
    # One ~40 minute lap: planet, hub, every story mission, the Ruin boss,
    # an ancient vault, and a space encounter. Then repeat until stopped.
    flag_body=$'cycle=90\nscenario=orbitedworld,instanceworld:outpost,instanceworld:lunarbase,instanceworld:floranmission1,instanceworld:hylotlmission1,instanceworld:avianmission1,instanceworld:apexmission1,instanceworld:glitchmission1,instanceworld:penguinmission1,instanceworld:cultistmission1,instanceworld:tentaclemission=915.306,instanceworld:ancientvault_fire,instanceworld:spaceencounter\nsticky'
  fi
  local tmp; tmp="$(mktemp)"
  printf '%s\n' "$flag_body" > "$tmp"
  if ftp_ok; then
    ftp_put "$tmp" "/switch/oSBM/autopilot.flag"
    : > "$tmp"
    ftp_put "$tmp" "/switch/oSBM/autorestart.flag"
    ftp_del "/switch/oSBM/reload.flag"
    ftp_del "/switch/oSBM/heartbeat.txt"
  else
    kioclient copy "$tmp" "$OSBM_DIR/autopilot.flag" 2>/dev/null
    : > "$tmp"
    kioclient copy "$tmp" "$OSBM_DIR/autorestart.flag" 2>/dev/null
    kioclient remove "$OSBM_DIR/heartbeat.txt" 2>/dev/null || true
  fi
  rm -f "$tmp"
  echo "autopilot.flag:"; printf '  %s\n' "$flag_body"
}

cmd_push() {
  local nro="${1:-}" scenario="${2:-}"
  [[ -f "$nro" ]] || die "usage: push <path-to.nro> [scenario]"
  need_transport
  local dest_name="oSBM.1.nro"
  if ftp_ok; then
    local listing base max=0
    listing="$(curl -sS --connect-timeout 5 --max-time 15 --disable-epsv --list-only "$(ftp_url)/switch/" 2>/dev/null || true)"
    while IFS= read -r f; do
      [[ -z "$f" ]] && continue
      base="${f##*/}"
      if [[ "$base" =~ ^oSBM\.([0-9]+)\.nro$ ]]; then
        if (( 10#${BASH_REMATCH[1]} > max )); then
          max=$((10#${BASH_REMATCH[1]}))
        fi
      fi
    done <<< "$listing"
    dest_name="oSBM.$((max + 1)).nro"
    echo "pushing $(basename "$nro") -> /switch/$dest_name"
    ftp_put "$nro" "/switch/$dest_name"
  else
    echo "pushing $(basename "$nro") -> /switch/$dest_name"
    kioclient copy "$nro" "$MTP_ROOT/$dest_name" 2>/dev/null
  fi
  write_flags "$scenario"
  echo "ready -- launch /switch/$dest_name (hbmenu shows the NACP name; extra numbered copies are deleted on boot)"
}

cmd_reload() {
  ftp_ok || die "reload needs sys-ftpd (game must be running, or hbloader waiting)"
  local tmp; tmp="$(mktemp)"
  : > "$tmp"
  ftp_put "$tmp" "/switch/oSBM/reload.flag"
  rm -f "$tmp"
  echo "reload.flag written -- the running NRO will chainload within ~1s, or on next crash if it is wedged"
}

cmd_collect() {
  local label="${1:-run-$(date +%Y%m%d-%H%M%S)}"
  need_transport
  local dest="$RUNS_DIR/$label"
  mkdir -p "$dest"
  if ftp_ok; then
    for f in logs/starbound.log crash.txt heartbeat.txt relaunch-trace.txt stderr.txt stdout.txt; do
      ftp_get "/switch/oSBM/$f" "$dest/$(basename "$f")" 2>/dev/null || true
    done
  else
    for f in logs/starbound.log crash.txt heartbeat.txt relaunch-trace.txt stderr.txt stdout.txt; do
      kioclient copy "$OSBM_DIR/$f" "file://$dest/$(basename "$f")" 2>/dev/null || true
    done
  fi
  echo "collected into $dest"
  ls -la "$dest" | tail -n +2
  analyze "$dest"
}

analyze() {
  local dest="$1"
  local hb="$dest/heartbeat.txt"
  echo
  echo "---- verdict ----"
  if [[ ! -s "$hb" ]]; then
    echo "no heartbeat: build predates the heartbeat, or the run never started"
  elif grep -q '^state=clean-exit' "$hb"; then
    echo "CLEAN EXIT"
    sed -n 's/^/  /p' "$hb"
  else
    echo "CRASH (heartbeat never marked clean) -- state at death:"
    sed -n 's/^/  /p' "$hb"
  fi

  local log="$dest/starbound.log"
  if [[ -f "$log" ]]; then
    echo
    echo "peak memory seen in log:"
    grep -o 'newlibUsed=[0-9]*kB' "$log" | grep -o '[0-9]*' \
      | sort -rn | head -1 | awk '{printf "  %d MB\n", $1/1024}'
    local releases oom
    releases=$(grep -c 'released .*allocator cache' "$log" 2>/dev/null) || releases=0
    oom=$(grep -c 'bad_alloc\|span map FAILED' "$log" 2>/dev/null) || oom=0
    echo "allocator releases: $releases"
    echo "OOM / bad_alloc events: $oom"
    local worst
    worst=$(grep 'perf-maint] sweep' "$log" 2>/dev/null | grep -v 'mem unavailable' \
      | grep -o 'sweep [0-9.]*ms' | grep -o '[0-9.]*' | sort -rn | head -1)
    [[ -n "$worst" ]] && echo "worst in-game maintenance sweep: ${worst}ms"
    echo "last title/world lines:"
    grep -E 'Title:|Title world|fullyLoad|scenario warp|flushed world GPU|Unknown species' "$log" | tail -20 | sed 's/^/  /'
  fi

  if [[ -f "$dest/crash.txt" ]] && tail -8 "$dest/crash.txt" | grep -q 'span map FAILED\|ABORT\|FATALTHROW\|relaunch:'; then
    echo
    echo "crash.txt tail:"
    tail -8 "$dest/crash.txt" | sed 's/^/  /'
  fi
}

cmd_status() {
  local latest
  latest="$(ls -1dt "$RUNS_DIR"/*/ 2>/dev/null | head -1)"
  [[ -n "$latest" ]] || die "no collected runs in $RUNS_DIR"
  echo "latest run: $latest"
  analyze "${latest%/}"
}

case "${1:-}" in
  push)     shift; cmd_push "$@" ;;
  collect)  shift; cmd_collect "$@" ;;
  reload)   shift; cmd_reload "$@" ;;
  discover) shift; cmd_discover "$@" ;;
  status)   shift; cmd_status "$@" ;;
  *) sed -n '2,20p' "$0"; exit 1 ;;
esac
