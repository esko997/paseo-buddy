#!/bin/sh
cd "$(dirname "$0")"
# Onion starts apps with LD_PRELOAD=libpadsp.so, which routes /dev/dsp to its audioserver; keep sound
# working when launched some other way (e.g. from a shell).
if [ -z "$LD_PRELOAD" ] && [ -f /mnt/SDCARD/miyoo/lib/libpadsp.so ]; then
  export LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so
fi
./pager > pager.log 2>&1
# The pager turns the screen off when idle; make sure Onion's menu comes back lit even after a crash.
echo 1 > /sys/class/gpio/gpio4/value 2>/dev/null
echo 1 > /sys/class/gpio/gpio48/value 2>/dev/null
# It also dims the backlight at night: put back the user's own level from Onion's settings, using
# Onion's curve (src/common/system/display.h): duty = round(3 * exp(0.350656 * level)), tabulated here
# because busybox awk may be built without exp().
level=$(sed -n 's/.*"brightness":[[:space:]]*\([0-9][0-9]*\).*/\1/p' /appconfigs/system.json 2>/dev/null)
if [ -n "$level" ] && [ "$level" -le 10 ]; then
  set -- 3 4 6 9 12 17 25 35 50 70 100
  shift "$level"
  echo "$1" > /sys/devices/soc0/soc/1f003400.pwm/pwm/pwmchip0/pwm0/duty_cycle 2>/dev/null
fi
# Onion's keymon skips idle sleep while this flag exists; the pager sets it while on power.
rm -f /tmp/stay_awake
