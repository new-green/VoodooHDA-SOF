# VoodooHDA + Intel SOF DMIC (experimental)

This fork of [VoodooHDA](https://github.com/CloverHackyColor/VoodooHDA) makes the **built-in digital microphone (DMIC) of recent Intel laptops** work on macOS (Hackintosh).

On Alder Lake and other recent Intel laptops, the internal microphones are not on the HDA codec. They sit behind the audio DSP (Intel cAVS / Sound Open Firmware). AppleALC and stock VoodooHDA cannot see them, so the internal mic simply does not exist on macOS. This fork boots Intel's SOF firmware on the DSP from inside VoodooHDA, sets up the DMIC pipeline the same way Linux does, and publishes the microphone as a normal CoreAudio input device.

> **Status: experimental.** It has been developed and tested on a single machine: an **HP ProBook 440 G9 (Alder Lake-P, PCI 8086:51C8) on macOS Tahoe 26.7**. On that machine it records with the same quality as Linux. Anything else is untested. This is a kernel extension, so a bug can cause a kernel panic. Keep a working EFI/kext to fall back on.

## Features

- **Internal Microphone**: the DMIC pair, 48 kHz, 2 ch, 16 bit. The input volume slider in System Settings works (−50…+20 dB, DSP gain), and mute works too.
- **Internal Microphone (Noise Reduction)**: a second input device on the same stream. It applies a 90 Hz high-pass, [RNNoise](https://github.com/xiph/rnnoise) (running in the kernel, about 1–2 % of one core), and a −1 dBFS limiter. Both devices can be used at the same time, and their level and mute stay in sync.
- **SofMicBar**: a menu bar app with a live noise-reduction strength slider (0–100 %) and a mute toggle (macOS has no input mute button). It can start at login.
- **Idle power saving**: 5 s after the last recording stops, the DSP goes into D0i3. It wakes in about 7 ms when an app starts recording.
- The existing VoodooHDA features (HDA codec, speakers, headphones, jack mic) are unchanged. Without the `-voodoosof` boot-arg, none of the SOF code runs.

## Not supported / known issues

- **Sleep/wake is untested.** The test machine runs with sleep disabled. After S3, the DSP will most likely need a firmware reload, which is not implemented.
- **Other laptops**: the DMIC setup is replayed from a Linux capture made on the test machine. See [sof/README.md](sof/README.md) for how to capture your own.
- On a cold start, the first ~15–70 ms of a recording are silent while the DMIC warms up.
- The Xcode projects in `tranc/` were not updated. Build with `build-kext.sh`.

## Requirements

- An Intel HDA controller with the cAVS DSP enabled: PCI device ID in `kSofDeviceIds` (`tranc/VoodooHDASof.cpp`), currently only `0x51C8`.
- A firmware/BIOS that leaves the DSP visible. If Linux uses `snd_sof_pci_intel_tgl` for audio on your machine, the DSP is visible.
- To build: Xcode Command Line Tools (`xcode-select --install`). Xcode itself is not needed.

## Build

```sh
./build-kext.sh                 # -> build/VoodooHDA.kext (device names in English)
SOF_LANG=tr ./build-kext.sh     # Turkish device names ("Dahili Mikrofon")
tools/SofMicBar/build.sh        # -> tools/SofMicBar/build/SofMicBar.app
```

The firmware and the IPC replay data are in `sof/` and are checked against `sof/SHA256SUMS` during the build.

## Install

**Before you start**, keep a copy of your current `VoodooHDA.kext` and EFI.

1. Install the kext, using one of these two ways:
   - **OpenCore (usual):** replace `EFI/OC/Kexts/VoodooHDA.kext` with `build/VoodooHDA.kext`. Make sure there is a `Kernel → Add` entry for it in `config.plist`, as for stock VoodooHDA.
   - **/Library/Extensions:** run `sudo bash install/install-kext.sh`. It backs the old kext up to `~/VoodooHDA-backup/`.
2. Add **`-voodoosof`** to `boot-args` (`NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`) and reboot.
3. Optional: run `sudo bash install/install-sofmicbar.sh` for the menu bar app, then turn on "Start at login" in its menu. The app follows the system language (Turkish or English). To force one, run `defaults write local.voodoohda.sofmicbar language tr` (or `en`) and restart the app.

To roll back, remove `-voodoosof`. VoodooHDA then behaves like stock again.

### Boot arguments

| boot-arg | Effect |
|---|---|
| `-voodoosof` | enable the SOF/DMIC support (required) |
| `voodoosofnr=0\|1\|2` | noise-reduced second device: off / on, 50 % (default) / on, 100 % |
| `voodoosofpm=0` | keep the DSP fully on while idle (no D0i3) |
| `-voodoosofdbg` | verbose logging (register dumps, every IPC) |
| `voodoosofstep=1..4` | debugging only: stop after probe / ROM init / firmware boot / IPC |

### Checking that it works

```sh
kextstat | grep VoodooHDA
/usr/bin/log show --last boot --predicate 'eventMessage CONTAINS "VoodooHDA SOF"' --style compact
```

A normal boot logs about ten lines: `enabled`, `FW_READY: version 2.2.0 …`, `firmware boot complete`, `ready`, `capture engine published`, and `DSP idle: D0i3` a few seconds later.

`tools/sof-test/` contains two small command-line tools: `rec` records from a device and prints its level, and `sofctl` reads or sets mute and NR strength.

## How it works (short)

1. When VoodooHDA starts, `VoodooHDASof` maps the DSP BAR. It then follows Linux's `hda_dsp_cl_boot_firmware_iccmax()`: power up core 0, run ROM init, stream the signed firmware through the HDA code-loader DMA, and wait for `FW_READY` (IPC3).
2. It replays the IPC3 messages that Linux sends for `sof-hda-generic-2ch.tplg`: pipeline, volume component, and DMIC DAI config.
3. `VoodooHDASofEngine`, an `IOAudioEngine`, takes its samples directly from the DMIC host DMA ring. Positions are polled; no interrupts are used, so VoodooHDA's interrupt path is untouched. The noise-reduced engine runs RNNoise in `convertInputSamples`.
4. When idle, it writes D0I3C plus a compact `PM_GATE` IPC, the same sequence as `hda_dsp_set_D0_state()`.

The register layout and sequences are ported from the Linux SOF driver (`sound/soc/sof/intel/`). Source comments name the Linux function each part comes from.

## Licences and credits

- **VoodooHDA**: BSD 2-Clause, see [LICENSE](LICENSE) and [README-VoodooHDA.md](README-VoodooHDA.md). © CloverHackyColor, Slice and the VoodooHDA authors.
- **SOF support** (`tranc/VoodooHDASof*`): BSD 3-Clause. Ported from the Linux SOF Intel driver, whose SOF parts are dual-licensed GPL-2.0-only OR BSD-3-Clause, © Intel Corporation. The BSD-3-Clause option is used.
- **SOF firmware** (`sof/firmware/sof-adl.ri`): unmodified binary from [thesofproject/sof-bin](https://github.com/thesofproject/sof-bin), © Intel Corporation and others. See [sof/firmware/LICENCE.Intel](sof/firmware/LICENCE.Intel).
- **RNNoise** v0.1.1 (`tranc/rnnoise/`): BSD 3-Clause, © Xiph.Org Foundation, Mozilla, Jean-Marc Valin. See [tranc/rnnoise/COPYING](tranc/rnnoise/COPYING). It was adapted to run in the kernel, see [README.voodoo](tranc/rnnoise/README.voodoo).

There is NO WARRANTY, as the licences above say. Intel, HP and Apple are trademarks of their owners. This project is not affiliated with any of them.

---

## Türkçe özet

Bu fork, yeni Intel laptopların **dahili dijital mikrofonunu (DMIC)** macOS'ta (Hackintosh) çalıştırır. Mikrofon ses kodeğine değil DSP'ye bağlı olduğu için AppleALC ve normal VoodooHDA onu göremez. Bu sürüm DSP'de Intel'in SOF firmware'ini başlatır ve mikrofonu normal bir giriş cihazı olarak gösterir.

- **Deneysel:** Yalnızca **HP ProBook 440 G9 (Alder Lake, 8086:51C8), macOS Tahoe 26.7** üzerinde test edildi. Uyku/uyanma test edilmedi.
- **Cihazlar:** iki giriş cihazı var: ham mikrofon ve gürültü azaltmalı (RNNoise) mikrofon. Menü çubuğu uygulaması SofMicBar ile gürültü azaltma gücü ayarlanır ve mikrofon sessize alınır. Mikrofon kullanılmadığında DSP uyur (D0i3).
- **Derleme:** `./build-kext.sh`, Türkçe cihaz adları için `SOF_LANG=tr ./build-kext.sh`.
- **Kurulum:** kext'i OpenCore'a koyup `boot-args`'a `-voodoosof` ekleyin. Geri almak için bu parametreyi silmek yeterlidir.
- **Başka laptoplar:** [sof/README.md](sof/README.md) dosyasına bakın.
