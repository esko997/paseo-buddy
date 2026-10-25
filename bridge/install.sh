#!/usr/bin/env bash
# Installs the bridge as a systemd user service on this machine and prints the pager.cfg line.
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
# Non-login shells (ssh one-liners) often lack Homebrew/nvm on PATH, so look in the usual places too.
node=${NODE:-$(command -v node || true)}
for cand in /home/linuxbrew/.linuxbrew/bin/node "$HOME"/.nvm/versions/node/*/bin/node; do
  [ -n "$node" ] && break
  [ -x "$cand" ] && node=$cand
done
[ -x "$node" ] || { echo "node not found; set NODE=/path/to/node" >&2; exit 1; }
major=$("$node" -p 'process.versions.node.split(".")[0]')
[ "$major" -ge 22 ] || { echo "node $major is too old (need 22+ for global WebSocket)" >&2; exit 1; }
PATH="$(dirname "$node"):$PATH" npm --prefix "$dir" install --omit=dev --silent
unit_dir="$HOME/.config/systemd/user"
mkdir -p "$unit_dir"
sed -e "s|NODE|$node|" -e "s|BRIDGE_DIR|$dir|g" "$dir/paseo-pager-bridge.service" > "$unit_dir/paseo-pager-bridge.service"
systemctl --user daemon-reload
systemctl --user enable paseo-pager-bridge.service
systemctl --user restart paseo-pager-bridge.service
loginctl show-user "$USER" -p Linger | grep -q yes ||
  echo "note: run 'sudo loginctl enable-linger $USER' so the bridge runs without a login session"
for _ in 1 2 3 4 5 6 7 8 9 10; do
  [ -s "$HOME/.config/paseo-pager/token" ] && break
  sleep 0.5
done
ip=$(ip -4 route get 1.1.1.1 | awk '{for (i = 1; i < NF; i++) if ($i == "src") print $(i + 1)}')
echo "pager.cfg line for this machine:"
echo "bridge = $ip:6780 $(cat "$HOME/.config/paseo-pager/token")"
