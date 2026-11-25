#!/bin/bash
# Reference capture for the VoodooHDA SOF/DMIC port. Run it on Linux, on the same laptop, with
# the stock SOF driver (snd_sof_pci_intel_tgl) working.
#
#   sudo bash sof-trace.sh setup     # 1) enable IPC payload dumps, then REBOOT
#   sudo bash sof-trace.sh collect   # 2) after the reboot: cold-boot DSP trace + 4 s DMIC capture
#   sudo bash sof-trace.sh cleanup   # 3) remove the debug option (reboot when convenient)
#
# setup writes /etc/modprobe.d/zz-sof-trace.conf (snd_sof sof_debug=0xA00 = IPC success logs +
# IPC payload hexdumps). collect forces a full (non-IMR) firmware load, records 4 s from the
# internal mic and saves dmesg plus the firmware/topology/NHLT in ~/sof-ref/trace-<date>/.
# Then: python3 -I sof_ipc_extract.py dmesg-trace.txt ipc-replay.json
#
# PCI address and the ALSA DMIC device (hw:0,6) are those of an Alder Lake HDA+DMIC laptop;
# check `aplay -l` / `arecord -l` and lspci on yours.
set -u
PCI=${PCI:-0000:00:1f.3}
DMIC=${DMIC:-hw:0,6}
OWNER=${SUDO_USER:-$(id -un)}
REF=$(eval echo "~$OWNER")/sof-ref
CONF=/etc/modprobe.d/zz-sof-trace.conf
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

case "${1:-}" in
setup)
  echo "options snd_sof sof_debug=0xA00" > "$CONF"
  echo "wrote $CONF: $(cat $CONF)"
  echo ">>> reboot now, then: sudo bash $0 collect"
  ;;

collect)
  dbg=$(cat /sys/module/snd_sof/parameters/sof_debug 2>/dev/null)
  echo "snd_sof.sof_debug = $dbg"
  [ "$dbg" = "2560" ] || echo "WARNING: sof_debug is not 0xA00 (2560) - did you reboot after setup?"
  OUT="$REF/trace-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$OUT"; cd "$OUT" || exit 1

  # reference files to carry over to macOS
  mkdir -p "$OUT/firmware"
  cp -L /usr/lib/firmware/intel/sof/sof-adl.ri "$OUT/firmware/" 2>/dev/null
  cp -L /usr/lib/firmware/intel/sof-tplg/sof-hda-generic-2ch.tplg "$OUT/firmware/" 2>/dev/null
  (cd "$OUT/firmware" && sha256sum * > SHA256SUMS)
  cp /sys/firmware/acpi/tables/NHLT "$OUT/firmware/NHLT.aml" 2>/dev/null

  # dynamic debug for the SOF modules (prints the IPC hexdumps enabled by sof_debug)
  DC=/sys/kernel/debug/dynamic_debug/control
  for m in snd_sof snd_sof_intel_hda_common snd_sof_intel_hda snd_sof_pci snd_sof_utils snd_sof_intel_hda_mlink snd_hda_ext_core; do
    echo "module $m +p" > "$DC" 2>/dev/null
  done
  # force a full firmware download (code loader) instead of an IMR restore on the next DSP boot
  echo 1 > /sys/kernel/debug/sof/skip_imr_boot 2>/dev/null && echo "skip_imr_boot=1"
  echo "-> waiting for the DSP to runtime-suspend (stay quiet)..."
  st=""
  for i in $(seq 1 30); do
    st=$(cat /sys/bus/pci/devices/$PCI/power/runtime_status 2>/dev/null)
    [ "$st" = suspended ] && break; sleep 1
  done
  echo "runtime_status: $st"
  dmesg -C

  echo "-> recording 4 s from the internal mic: talk / snap your fingers"
  ok=0
  for args in "-sample_rate 48000 -ch_layout stereo" "-sample_rate 48000" ""; do
    # shellcheck disable=SC2086
    if ffmpeg -hide_banner -loglevel error -f alsa $args -i "$DMIC" -t 4 -y dmic.wav 2>> ffmpeg.log; then ok=1; break; fi
  done
  if [ $ok = 0 ] && command -v pw-record >/dev/null; then
    sudo -u "$OWNER" XDG_RUNTIME_DIR=/run/user/$(id -u "$OWNER") timeout 5 pw-record --rate 48000 --channels 2 dmic.wav 2>> pwrecord.log && ok=1
  fi
  echo "recording: $([ $ok = 1 ] && echo OK || echo FAILED)"
  sleep 1
  dmesg > dmesg-trace.txt
  grep -E "ipc tx|ipc rx|ipc tx (succeeded|done)|[0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2} " dmesg-trace.txt > ipc-with-payload.txt
  grep -iE "code loader|cl_|firmware|ROM|boot|IMR|purge|stream_tag|FW Poll|core" dmesg-trace.txt > boot-path.txt
  echo 0 > /sys/kernel/debug/sof/skip_imr_boot 2>/dev/null
  chown -R "$OWNER": "$REF"
  echo
  echo "DONE: $OUT"; ls -la "$OUT"
  echo "lines: trace=$(wc -l < dmesg-trace.txt) ipc=$(wc -l < ipc-with-payload.txt) boot=$(wc -l < boot-path.txt)"
  ;;

cleanup)
  rm -f "$CONF" && echo "removed $CONF (normal again after the next boot)"
  ;;

*)
  echo "usage: sudo bash $0 setup|collect|cleanup"; exit 1 ;;
esac
