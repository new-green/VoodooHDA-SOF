#!/bin/bash
# Build the two command line test tools (Command Line Tools only):
#   rec    <device name|list> <seconds> [out.raw]  - record 48 kHz/2 ch/S16 from an input device, print RMS/peak
#   sofctl mute get|0|1  |  sofctl nr get|0..100     - SOF mic mute (CoreAudio) and NR strength (IORegistry)
set -euo pipefail
cd "$(dirname "$0")"
clang -O2 -o rec rec.c -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
swiftc -O -o sofctl sofctl.swift
echo "OK: $(pwd)/rec $(pwd)/sofctl"
