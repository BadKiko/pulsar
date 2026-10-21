#!/bin/sh
# cucumber-satellite control: enable | disable | restart | status | log
#   enable  - stop the Yandex stack, run cucumber-satellite now and on every boot
#   disable - stop cucumber-satellite, start the Yandex stack again (also after reboot)
D=/data/cucumber-satellite
Q=/system/vendor/quasar/q

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
  *)
    echo "usage: $0 enable|disable|restart|status|log [lines]"
    exit 1
    ;;
esac
