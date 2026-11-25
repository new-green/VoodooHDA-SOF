#!/bin/bash
# Build SofMicBar.app with the Command Line Tools (no Xcode). Output: build/SofMicBar.app
set -euo pipefail
cd "$(dirname "$0")"
APP=build/SofMicBar.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
swiftc -O -target x86_64-apple-macos13.0 -framework AppKit -framework IOKit -o "$APP/Contents/MacOS/SofMicBar" main.swift
cp Info.plist "$APP/Contents/Info.plist"
codesign -s - --force "$APP"
echo "OK: $(pwd)/$APP"
