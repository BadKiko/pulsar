#!/bin/sh
# Boot hook (called from /data/quasar/rootkeeper.sh, which onboot.sh starts in background).
# If /data/cucumber-satellite/enabled exists: stop the Yandex stack started by onboot.sh and run
# cucumber-satellite under a restart loop. adbd and dropbear are never touched.
D=/data/cucumber-satellite
[ -f "$D/enabled" ] || exit 0
# Never stop the Yandex stack when there is nothing to replace it with.
[ -x "$D/cucumber-satellite" ] && [ -x "$D/supervise.sh" ] || exit 0
pidof cucumber-satellite >/dev/null 2>&1 && exit 0

# onboot.sh launches quasar right before calling us; wait for it, then stop it.
i=0
while [ $i -lt 30 ] && ! pidof quasar_launcher2 >/dev/null 2>&1; do
  sleep 1
  i=$((i + 1))
done
/system/vendor/quasar/q stop >/dev/null 2>&1

exec setsid "$D/supervise.sh" </dev/null >/dev/null 2>&1
