#!/bin/bash
# Install the menu bar app (noise reduction slider + mute) to /Applications and start it.
#   sudo bash install/install-sofmicbar.sh
# "Start at login" is switched on from the app's menu (a per-user LaunchAgent, no admin needed).
# Remove: switch "Start at login" off, Quit, delete /Applications/SofMicBar.app.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/SofMicBar/build/SofMicBar.app"
DST="/Applications/SofMicBar.app"

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo bash $0"; exit 1; }
[ -d "$SRC" ] || { echo "error: $SRC not found - run tools/SofMicBar/build.sh first"; exit 1; }
USR="${SUDO_USER:-}"
[ -n "$USR" ] && [ "$USR" != root ] || { echo "error: run it with sudo from your normal user"; exit 1; }

pkill -x SofMicBar 2>/dev/null
rm -rf "$DST"
ditto "$SRC" "$DST"
chown -R "$USR":admin "$DST"
xattr -cr "$DST" 2>/dev/null
echo "== installed: $DST"
sudo -u "$USR" open "$DST" && echo "== started: microphone icon in the menu bar"
