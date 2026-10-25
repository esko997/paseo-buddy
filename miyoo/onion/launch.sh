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
