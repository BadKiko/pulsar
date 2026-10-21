#!/bin/sh
# Keeps cucumber-satellite running while /data/cucumber-satellite/enabled exists.
# Log: /tmp/cucumber-satellite.log (tmpfs, rotated at 1 MB, one .1 kept).
#
# Exit status of the daemon: 0 = deliberate exit (SIGTERM from ctl.sh restart, the "Restart" button
# in Home Assistant): restart after 1 s, not counted as a crash unless it ran < MINUP seconds.
# Anything else (non-zero, killed by a signal, e.g. 139 = SIGSEGV) is a crash, restart after 3 s.
# CRASHES crashes within WINDOW seconds: swap in cucumber-satellite.prev (the bad binary is kept as
# .bad) and try again; if that crash-loops too, remove `enabled` and give the speaker back to the
# Yandex stack (same as `ctl.sh disable`).
# CS_DIR, CS_LOG, CS_Q override the device paths (local dry runs).
D=${CS_DIR:-/data/cucumber-satellite}
LOG=${CS_LOG:-/tmp/cucumber-satellite.log}
Q=${CS_Q:-/system/vendor/quasar/q}
BIN=$D/cucumber-satellite
MAXLOG=1000000
WINDOW=60
CRASHES=5
MINUP=5

# Copy-truncate: the daemon keeps its append-mode descriptor, so a rename would not stop growth.
rotate() {
  if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt $MAXLOG ]; then
    cp "$LOG" "$LOG.1" && : > "$LOG"
  fi
}

say() { echo "=== supervise: $* $(date)" >> "$LOG"; }

give_back() { # Yandex stack again, like ctl.sh disable
  rm -f "$D/enabled"
  say "$1; disabled, starting the Yandex stack"
  pidof quasar_launcher2 >/dev/null 2>&1 || "$Q" start >/dev/null 2>&1
  exit 1
}

( while sleep 60; do rotate; done ) &
ROT=$!
trap 'kill $ROT 2>/dev/null' EXIT

fails=0 first=0 swapped=0
while [ -f "$D/enabled" ]; do
  rotate
  if [ ! -x "$BIN" ]; then
    say "$BIN missing or not executable"
    if [ "$swapped" = 0 ] && [ -x "$BIN.prev" ]; then
      cp -p "$BIN.prev" "$BIN.tmp" && mv "$BIN.tmp" "$BIN" && swapped=1 && fails=0 && say "restored .prev" && continue
    fi
    give_back "no usable binary"
  fi
  echo "=== start $(date)" >> "$LOG"
  t0=$(date +%s)
  "$BIN" "$D/state.conf" >> "$LOG" 2>&1
  rc=$?
  now=$(date +%s)
  up=$((now - t0))
  echo "=== exit $rc after ${up}s $(date)" >> "$LOG"

  if [ $rc -eq 0 ] && [ $up -ge $MINUP ]; then
    [ $up -ge $WINDOW ] && swapped=0
    fails=0
    sleep 1
    continue
  fi
  [ $up -ge $WINDOW ] && { fails=0; swapped=0; }
  if [ $fails -eq 0 ] || [ $((now - first)) -gt $WINDOW ]; then first=$now; fails=0; fi
  fails=$((fails + 1))
  if [ $fails -ge $CRASHES ]; then
    if [ "$swapped" = 0 ] && [ -f "$BIN.prev" ] && ! cmp -s "$BIN" "$BIN.prev"; then
      mv -f "$BIN" "$BIN.bad"
      if cp -p "$BIN.prev" "$BIN.tmp" && mv "$BIN.tmp" "$BIN"; then
        swapped=1 fails=0
        say "$CRASHES crashes in ${WINDOW}s, rolled back to .prev (bad binary kept as .bad)"
        continue
      fi
      mv -f "$BIN.bad" "$BIN"
    fi
    give_back "$CRASHES crashes in ${WINDOW}s, no previous binary to fall back to"
  fi
  sleep 3
done
