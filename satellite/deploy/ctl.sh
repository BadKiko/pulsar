#!/bin/sh
# cucumber-satellite control: enable | disable | restart | status | log
#   enable  - stop the Yandex stack, run cucumber-satellite now and on every boot
#   disable - stop cucumber-satellite, start the Yandex stack again (also after reboot)
D=/data/cucumber-satellite
Q=/system/vendor/quasar/q
case "$1" in
  enable)
    touch "$D/enabled"
    $Q stop >/dev/null 2>&1
    pidof supervise.sh >/dev/null 2>&1 || (setsid "$D/supervise.sh" </dev/null >/dev/null 2>&1 &)
    echo "enabled"
    ;;
  disable)
    rm -f "$D/enabled"
    killall supervise.sh cucumber-satellite 2>/dev/null
    sleep 1
    pidof quasar_launcher2 >/dev/null 2>&1 || $Q start >/dev/null 2>&1
    echo "disabled, Yandex stack started"
    ;;
  restart)
    killall cucumber-satellite 2>/dev/null
    echo "restarting (supervisor restarts it in ~3 s)"
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
