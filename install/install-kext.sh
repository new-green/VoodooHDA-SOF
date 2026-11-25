#!/bin/bash
# Install build/VoodooHDA.kext into /Library/Extensions (alternative to OpenCore injection).
#   sudo bash install/install-kext.sh
# The kext that is there now is moved to ~/VoodooHDA-backup/VoodooHDA-<version>-<date>.kext.
# To go back: copy that backup to /Library/Extensions/VoodooHDA.kext and run
#   sudo kmutil install --update-all   (then reboot)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/VoodooHDA.kext"
DST="/Library/Extensions/VoodooHDA.kext"
USR="${SUDO_USER:-root}"
BK="$(eval echo "~$USR")/VoodooHDA-backup"

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo bash $0"; exit 1; }
[ -d "$SRC" ] || { echo "error: $SRC not found - run ./build-kext.sh first"; exit 1; }
[ "$(defaults read "$SRC/Contents/Info" CFBundleIdentifier)" = org.voodoo.driver.VoodooHDA ] || { echo "error: unexpected bundle"; exit 1; }
ver=$(defaults read "$SRC/Contents/Info" CFBundleVersion)

if [ -d "$DST" ]; then
  old=$(defaults read "$DST/Contents/Info" CFBundleVersion 2>/dev/null || echo unknown)
  mkdir -p "$BK"
  dest="$BK/VoodooHDA-$old-$(date +%Y%m%d-%H%M%S).kext"
  echo "== backing up the installed VoodooHDA $old -> $dest"
  cp -R "$DST" "$dest" && chown -R "$USR" "$BK"
fi

echo "== installing VoodooHDA $ver"
rm -rf "$DST"
cp -R "$SRC" "$DST"
xattr -cr "$DST" 2>/dev/null
chown -R root:wheel "$DST"
chmod -R 755 "$DST"
touch /Library/Extensions
echo "== rebuilding the auxiliary kext collection (1-2 minutes)..."
kmutil install --update-all 2>&1 | tail -15
echo
echo "If macOS says 'System software was blocked': System Settings > Privacy & Security > Allow."
echo "Then REBOOT. Check afterwards: kextstat | grep VoodooHDA   (should be $ver)"
