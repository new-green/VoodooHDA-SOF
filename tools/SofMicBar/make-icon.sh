#!/bin/bash
# Regenerate AppIcon.icns from docs/logo.svg (needs only the Command Line Tools).
set -euo pipefail
cd "$(dirname "$0")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
swiftc -O -o "$T/render" render-svg.swift
mkdir "$T/AppIcon.iconset"
for s in 16 32 128 256 512; do
  "$T/render" ../../docs/logo.svg "$T/AppIcon.iconset/icon_${s}x${s}.png" $s
  "$T/render" ../../docs/logo.svg "$T/AppIcon.iconset/icon_${s}x${s}@2x.png" $((s * 2))
done
iconutil -c icns -o AppIcon.icns "$T/AppIcon.iconset"
echo "OK: $(pwd)/AppIcon.icns"
