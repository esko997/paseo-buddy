#!/usr/bin/env bash
# Installs build/PaseoPager on a Miyoo over SSH (Onion: Apps > Tweaks > Network > SSH; user onion).
# Keeps the device's pager.cfg, pet.txt and lineage archives. Safe while the pager is running: files are staged and
# renamed into place, and the running copy keeps going until you quit and reopen it.
# Usage: ./deploy.sh [user@host]
set -euo pipefail
cd "$(dirname "$0")"
target=${1:-onion@192.168.1.190}
./build-miyoo.sh >/dev/null
# tar over ssh: works with dropbear whether or not an sftp/scp server is installed.
tar -C build --exclude='PaseoPager/pet.txt*' --exclude=PaseoPager/pager.cfg -cf - PaseoPager |
  ssh "$target" 'set -e
    app=/mnt/SDCARD/App/PaseoPager stage=/mnt/SDCARD/App/.PaseoPager.new
    rm -rf "$stage" && mkdir -p "$stage" && tar -C "$stage" -xf -
    mkdir -p "$app"
    for f in "$stage"/PaseoPager/*; do mv -f "$f" "$app/"; done
    rm -rf "$stage"
    chmod +x "$app/pager" "$app/launch.sh"
    cd "$app"
    [ -f pager.cfg ] || echo "warning: no pager.cfg on device; copy pager.cfg.example and fill in bridges"
    PAGER_DIR=/tmp/pager-deploy-check sh -c "mkdir -p /tmp/pager-deploy-check && cp pager.cfg /tmp/pager-deploy-check/ 2>/dev/null; ./pager --shot /tmp/pager-deploy-check/x.ppm"
    rm -rf /tmp/pager-deploy-check
    if ps | grep -v grep | grep -q "[.]/pager$"; then echo "installed; the pager is running the old build: press MENU and reopen it"
    else echo "installed"; fi'
