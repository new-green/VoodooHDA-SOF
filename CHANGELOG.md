# Changelog

**English** | [Türkçe](CHANGELOG.tr.md)

## 3.6.9-sof2 (2026-11-28), experimental

### Fixed
- **DSP boot failed on some cold boots** (`ROM halted`, `ROM_STATUS 80000003`): the ROM needs DEVC (0x78) bit 11 NSNPEN set while it loads the firmware. The kext used to restore the value the firmware/BIOS left there, but on the test machine that value changed from 1 to 0, so the microphone disappeared. NSNPEN is now set for the boot window regardless; a `note: firmware left DEVC NSNPEN=0` line shows when this happens.
- **Noise-reduction device stuttered and crackled in some apps** (seen in Google Meet in Brave): browsers often restart their audio IO or read the same part of the buffer twice. The NR engine treated that as a jump, reset RNNoise and inserted a short silence. Each read is now checked against the DMA write position. Repeated reads get the already denoised audio, small gaps are denoised as well, and only a real big jump skips ahead (with a pre-roll, no reset). The stop log shows the skip and re-read counters.

### Changed
- The firmware download is retried up to 3 times if the ROM halts.
- The code-loader and ICCMAX DMA buffers are allocated below 4 GB.

## 3.6.9-sof1 (2026-11-25), experimental

The first public version of the Intel SOF DMIC support. It is based on VoodooHDA Release368. Tested only on an HP ProBook 440 G9 (Alder Lake-P, 8086:51C8) with macOS Tahoe 26.7.

### Added
- **SOF DSP boot** from VoodooHDA, gated by `-voodoosof`. It powers up core 0 and runs ROM init, then loads the signed firmware (sof-bin v2.2 `sof-adl.ri`) through the HDA code-loader DMA and waits for `FW_READY` (IPC3). The sequence is a port of the Linux SOF Intel driver. Everything is polled, so VoodooHDA's interrupt path is not touched.
- **DMIC pipeline** set up by replaying the IPC3 messages Linux sends for `sof-hda-generic-2ch.tplg`.
- **"Internal Microphone"**: a CoreAudio input device, 48 kHz, 2 ch, 16 bit. Its samples come straight from the DMIC host DMA ring.
- **Input level and mute**: the System Settings slider (−50…+20 dB) controls the DSP volume component, and mute uses the DSP switch.
- **"Internal Microphone (Noise Reduction)"**: a second device on the same stream. It applies a 90 Hz high-pass, RNNoise v0.1.1 (running in the kernel) and a −1 dBFS limiter. Its strength can be changed live (0–100 %). Both devices can be used at the same time.
- **SofMicBar**, a menu bar app: noise-reduction strength slider, mute toggle, the global shortcut ⌃⌥M, start at login, and Turkish/English UI.
- **Idle power saving**: the DSP enters D0i3 5 s after the last capture stops and wakes in about 7 ms.
- **Boot arguments**: `voodoosofnr=0|1|2`, `voodoosofpm=0`, `-voodoosofdbg`, `voodoosofstep=1..4`.
- **Build**: `build-kext.sh` builds with the Command Line Tools only. `SOF_LANG=tr` gives Turkish device names.
- **Tools**: `sof/linux-capture/` captures the IPC sequence on Linux for another laptop. `tools/sof-test/` (`rec`, `sofctl`) is for testing.

### Known issues
- Sleep/wake is untested and the DSP is not reloaded after S3.
- On a cold start, the first ~15–70 ms of a recording are silent.
- Only PCI ID 0x51C8 and the 2-channel DMIC topology are supported.
