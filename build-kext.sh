#!/bin/bash
# Build VoodooHDA.kext with the Command Line Tools only (no Xcode needed).
# Mirrors tranc/VoodooHDA_BS.xcodeproj (Release): same sources, flags, Info.plist.
#
#   ./build-kext.sh                 # -> build/VoodooHDA.kext
#   KEXT_VERSION=3.6.9 ./build-kext.sh
#   SOF_LANG=tr ./build-kext.sh     # Turkish names for the SOF microphone devices
#
# The SOF part needs sof/firmware/sof-adl.ri and sof/ipc-replay.json (checked against
# sof/SHA256SUMS); see sof/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/tranc"
OUT="$ROOT/build"
OBJ="$OUT/obj"
KEXT="$OUT/VoodooHDA.kext"

BUNDLE_ID=org.voodoo.driver.VoodooHDA
KEXT_VERSION="${KEXT_VERSION:-3.6.9}"
SOF_LANG="${SOF_LANG:-en}"
SOFDIR="${SOFDIR:-$ROOT/sof}"
case "$SOF_LANG" in en) LANGDEF=0 ;; tr) LANGDEF=1 ;; *) echo "SOF_LANG must be en or tr"; exit 1 ;; esac
MIN_OS=10.13

SDK="${SDK:-$(xcrun --sdk macosx --show-sdk-path)}"
KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
CLANG_RT="$(dirname "$(xcrun -f clang)")/../lib/clang"
CC_KEXT="$(ls "$CLANG_RT"/*/lib/darwin/libclang_rt.cc_kext.a | head -1)"
[ -d "$KHDR" ] || { echo "Kernel.framework headers not found in $SDK"; exit 1; }

# Same as the Xcode kext template + project settings (OTHER_CFLAGS, -O3, fast-math, LTO)
COMMON=(
  -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=$MIN_OS
  -mkernel -nostdinc -fno-builtin -fno-common
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -I"$KHDR" -I"$SDK/System/Library/Frameworks/Kernel.framework/PrivateHeaders" -I"$SRC"
  -O3 -ffast-math -flto
  -fno-stack-protector -DMULTICHANNEL -DDP_AUDIO -msse4 -DSOF_LANG_TR=$LANGDEF
  -Wall -Wextra -Wno-unused-parameter
)
CXXFLAGS=(-x c++ -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables)
CFLAGS=(-x c -std=gnu11)

# source  extra-flags (per-file COMPILER_FLAGS from the project)
SOURCES=(
  "AppleAudioClip.cpp|-Wno-unused-function -fvisibility=hidden"
  "TigerAdditionals.cpp|"
  "PCMBlitterLib.cpp|-fvisibility=hidden"
  "VoodooHDAFramebufferNotifier.cpp|"
  "AppleALCPinConfigs.cpp|"
  "Parser.cpp|"
  "VoodooHDADevice.cpp|"
  "VoodooHDAEngine.cpp|"
  "VoodooHDAUserClient.cpp|"
  "Tables.c|"
  "PCMBlitterLibX86.cpp|-fvisibility=hidden"
  "VoodooHDASof.cpp|"
  "VoodooHDASofEngine.cpp|-Wno-deprecated-declarations -Wno-inconsistent-missing-override"
  # RNNoise v0.1.1 (BSD-3-Clause), kernel build: no fast-math (NaN checks), no libm, no VLAs
  "rnnoise/denoise.c|-fno-fast-math -fno-math-errno -w"
  "rnnoise/rnn.c|-fno-fast-math -fno-math-errno -w"
  "rnnoise/rnn_data.c|-w"
  "rnnoise/pitch.c|-fno-fast-math -fno-math-errno -w"
  "rnnoise/celt_lpc.c|-fno-fast-math -fno-math-errno -w"
  "rnnoise/kiss_fft.c|-fno-fast-math -fno-math-errno -w"
)

rm -rf "$OUT"; mkdir -p "$OBJ"
objs=()
for entry in "${SOURCES[@]}"; do
  f="${entry%%|*}"; extra="${entry#*|}"
  o="$OBJ/${f%.*}.o"
  mkdir -p "$(dirname "$o")"
  if [[ "$f" == *.c ]]; then lang=("${CFLAGS[@]}"); else lang=("${CXXFLAGS[@]}"); fi
  echo "  CC  $f"
  # shellcheck disable=SC2086
  xcrun clang "${lang[@]}" "${COMMON[@]}" $extra -c "$SRC/$f" -o "$o"
  objs+=("$o")
done

# Embedded SOF firmware (sof-bin v2.2.x/sof-v2.2/intel-signed/sof-adl.ri), verified before embedding
sumcheck() {	# <path relative to $SOFDIR>
  local want have
  want=$(awk -v f="$1" '$2==f{print $1}' "$SOFDIR/SHA256SUMS")
  have=$(shasum -a 256 "$SOFDIR/$1" 2>/dev/null | awk '{print $1}')
  [ -n "$want" ] && [ "$want" = "$have" ] || { echo "error: $SOFDIR/$1 missing or SHA256 mismatch (see sof/README.md)"; exit 1; }
}
FW="$SOFDIR/firmware/sof-adl.ri"
sumcheck firmware/sof-adl.ri
have=$(shasum -a 256 "$FW" | awk '{print $1}')
{
  echo "/* generated from sof-adl.ri sha256 $have */"
  echo "__attribute__((visibility(\"hidden\"), aligned(16))) const unsigned char gSofFirmware[] = {"
  xxd -i < "$FW"
  echo "};"
  echo "__attribute__((visibility(\"hidden\"))) const unsigned int gSofFirmwareSize = sizeof(gSofFirmware);"
} > "$OBJ/sof_firmware.c"
echo "  CC  sof_firmware.c ($(wc -c < "$FW" | tr -d ' ') bytes)"
xcrun clang "${CFLAGS[@]}" -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=$MIN_OS -mkernel \
  -nostdinc -O0 -c "$OBJ/sof_firmware.c" -o "$OBJ/sof_firmware.o"
objs+=("$OBJ/sof_firmware.o")

# IPC3 replay list (topology + DMIC setup captured from Linux), verified before use
RJ="$SOFDIR/ipc-replay.json"
sumcheck ipc-replay.json
python3 -I - "$RJ" "$OBJ/sof_replay.cpp" <<'PY'
import json, sys
msgs = json.load(open(sys.argv[1]))
out = ['/* generated from sof/ipc-replay.json */', '#include "VoodooHDASof.h"']
for i, m in enumerate(msgs):
    assert m['seq'] == i
    p = bytes.fromhex(m['payload'])
    assert len(p) == m['size'] - 8, (i, len(p), m['size'])
    out.append('static const UInt8 p%d[] = { %s };' % (i, ', '.join('0x%02x' % b for b in p)))
out.append('extern "C" {')
out.append('extern __attribute__((visibility("hidden"))) const SofReplayMsg gSofReplay[] = {')
for i, m in enumerate(msgs):
    out.append('\t{ %sU, %dU, p%d, "%s" },' % (m['cmd'], m['size'], i, m['type']))
out.append('};')
out.append('extern __attribute__((visibility("hidden"))) const unsigned int gSofReplayCount = %d;' % len(msgs))
out.append('}')
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
PY
echo "  CC  sof_replay.cpp"
xcrun clang "${CXXFLAGS[@]}" "${COMMON[@]}" -c "$OBJ/sof_replay.cpp" -o "$OBJ/sof_replay.o"
objs+=("$OBJ/sof_replay.o")

# kmod_info (what Xcode generates as <product>_info.c)
cat > "$OBJ/kmod_info.c" <<EOF
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BUNDLE_ID, "$KEXT_VERSION", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOF
echo "  CC  kmod_info.c"
xcrun clang -x c -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=$MIN_OS -mkernel -nostdinc \
  -DKERNEL -DKERNEL_PRIVATE -I"$KHDR" -c "$OBJ/kmod_info.c" -o "$OBJ/kmod_info.o"

mkdir -p "$KEXT/Contents/MacOS"
echo "  LD  VoodooHDA"
xcrun clang -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=$MIN_OS -flto \
  -nostdlib -Xlinker -kext -Xlinker -no_source_version -Xlinker -dead_strip \
  -L"$SDK/usr/lib" -lkmodc++ "${objs[@]}" "$OBJ/kmod_info.o" -lkmod "$CC_KEXT" \
  -o "$KEXT/Contents/MacOS/VoodooHDA"

# Our own symbols must never be left undefined (kext linking defers them to load time)
if nm -u "$KEXT/Contents/MacOS/VoodooHDA" | grep -E '_gSof|VoodooHDASof|_rnnk|_rnn_|_rnnoise|_celt|_compute_rnn' ; then
  echo "error: unresolved SOF symbols above"; exit 1
fi

# Info.plist: substitute Xcode build variables, add the keys Xcode would add
sed -e "s/\${EXECUTABLE_NAME}/VoodooHDA/; s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$BUNDLE_ID/; s/\${PRODUCT_NAME}/VoodooHDA/" \
    -e "s/\$(MARKETING_VERSION)/$KEXT_VERSION/; s/\$(CURRENT_PROJECT_VERSION)/$KEXT_VERSION/" \
    "$SRC/Info.plist" > "$KEXT/Contents/Info.plist"
P="$KEXT/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $MIN_OS" "$P"
/usr/libexec/PlistBuddy -c "Add :CFBundleSupportedPlatforms array" -c "Add :CFBundleSupportedPlatforms:0 string MacOSX" "$P"
if BUILD_ID=$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null); then
  (cd "$ROOT" && git diff --quiet HEAD -- tranc sof) || BUILD_ID="$BUILD_ID-dirty"
else
  BUILD_ID=nogit
fi
/usr/libexec/PlistBuddy -c "Add :VoodooSofBuild string $BUILD_ID-$SOF_LANG" "$P"
grep -q '\$(' "$P" && { echo "unsubstituted variable in Info.plist"; exit 1; }
plutil -lint "$P" >/dev/null

codesign --force --sign - "$KEXT" 2>&1 | grep -v 'replacing existing signature' || true
echo "OK: $KEXT ($KEXT_VERSION, SOF_LANG=$SOF_LANG)"
