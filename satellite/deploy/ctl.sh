#!/bin/sh
# cucumber-satellite control: enable | disable | restart | status | log | api-key
#   enable  - stop the Yandex stack, run cucumber-satellite now and on every boot
#   disable - stop cucumber-satellite, start the Yandex stack again (also after reboot)
#   api-key [base64|clear] - ESPHome Noise key ($D/api_key, base64 of 32 bytes, mode 0600): no
#             argument shows whether one is set; a value sets it, `clear` removes it; both restart.
#             Home Assistant (2026.8+) provisions a key automatically over the API on first
#             connect; setting or clearing a different key by hand makes HA ask to re-authenticate.
#             A corrupt file makes the daemon fail closed, hence the validation below.
D=${CS_DIR:-/data/cucumber-satellite}
Q=${CS_Q:-/system/vendor/quasar/q}

# SIGTERM, wait up to 10 s for the process to go, then SIGKILL. Returns 1 if it is still there.
stop_daemon() {
  pidof cucumber-satellite >/dev/null 2>&1 || return 0
  killall cucumber-satellite 2>/dev/null
  i=0
  while [ $i -lt 10 ] && pidof cucumber-satellite >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
  if pidof cucumber-satellite >/dev/null 2>&1; then
    echo "did not exit in 10 s, killing"
    killall -9 cucumber-satellite 2>/dev/null
    i=0
    while [ $i -lt 5 ] && pidof cucumber-satellite >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
  fi
  ! pidof cucumber-satellite >/dev/null 2>&1
}

valid_key() { # 44 base64 chars (32 bytes + one '='), and 32 bytes when a decoder exists
  case "$1" in
    *[!A-Za-z0-9+/=]*) return 1 ;;
  esac
  [ ${#1} -eq 44 ] || return 1
  case "$1" in *=) ;; *) return 1 ;; esac
  if command -v base64 >/dev/null 2>&1; then
    [ "$(printf '%s' "$1" | base64 -d 2>/dev/null | wc -c)" -eq 32 ] || return 1
  fi
}

case "$1" in
  enable)
    if [ ! -x "$D/cucumber-satellite" ] || [ ! -x "$D/supervise.sh" ]; then
      echo "$D/cucumber-satellite or supervise.sh missing or not executable, nothing changed" >&2
      exit 1
    fi
    touch "$D/enabled"
    $Q stop >/dev/null 2>&1
    pidof supervise.sh >/dev/null 2>&1 || (setsid "$D/supervise.sh" </dev/null >/dev/null 2>&1 &)
    echo "enabled"
    ;;
  disable)
    rm -f "$D/enabled"
    killall supervise.sh 2>/dev/null
    if ! stop_daemon; then
      echo "cucumber-satellite is still running, Yandex stack not started" >&2
      exit 1
    fi
    pidof quasar_launcher2 >/dev/null 2>&1 || $Q start >/dev/null 2>&1
    echo "disabled, Yandex stack started"
    ;;
  restart)
    if stop_daemon; then
      echo "stopped (supervisor restarts it in ~1 s)"
    else
      echo "cucumber-satellite did not exit" >&2
      exit 1
    fi
    ;;
  status)
    [ -f "$D/enabled" ] && echo "mode: cucumber-satellite" || echo "mode: yandex"
    echo "cucumber-satellite pid: $(pidof cucumber-satellite)"
    echo "quasar pid: $(pidof quasar_launcher2 maind)"
    ;;
  log)
    tail -n "${2:-50}" /tmp/cucumber-satellite.log
    ;;
  api-key)
    case "${2-}" in
      "")
        [ -s "$D/api_key" ] && echo "api key: set" || echo "api key: not set (plaintext API)"
        ;;
      clear)
        rm -f "$D/api_key"
        echo "api key removed"
        stop_daemon || { echo "cucumber-satellite did not exit" >&2; exit 1; }
        ;;
      *)
        valid_key "$2" || { echo "not a base64 encoding of 32 bytes (44 characters), nothing changed" >&2; exit 1; }
        (umask 077; printf '%s' "$2" > "$D/api_key.tmp") && chmod 600 "$D/api_key.tmp" && mv "$D/api_key.tmp" "$D/api_key" \
          || { rm -f "$D/api_key.tmp"; echo "cannot write $D/api_key" >&2; exit 1; }
        echo "api key set"
        stop_daemon || { echo "cucumber-satellite did not exit" >&2; exit 1; }
        ;;
    esac
    ;;
  *)
    echo "usage: $0 enable|disable|restart|status|log [lines]|api-key [base64|clear]"
    exit 1
    ;;
esac
