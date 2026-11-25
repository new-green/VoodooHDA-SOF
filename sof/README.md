# SOF data embedded in the kext

**English** | [Türkçe](README.tr.md)

`build-kext.sh` embeds two files from this directory into `VoodooHDA.kext`. It checks both against `SHA256SUMS` first.

| File | What it is | Source / licence |
|---|---|---|
| `firmware/sof-adl.ri` | Intel-signed Sound Open Firmware for Alder Lake (SOF 2.2.0, IPC3, ABI 3:22:1) | [thesofproject/sof-bin](https://github.com/thesofproject/sof-bin) `v2.2.x/sof-v2.2/intel-signed/sof-adl.ri`, unmodified (git blob `7f5246dd…`). Licence: `firmware/LICENCE.Intel` (BSD-3-Clause and others; redistribution permitted) |
| `ipc-replay.json` | The IPC3 messages Linux sends after the firmware boots, with their payloads: topology `sof-hda-generic-2ch.tplg` (sof-bin `sof-tplg-v2.2.1`), DMIC DAI config, PCM params and so on | Captured on an HP ProBook 440 G9 running Linux 6.x with `linux-capture/sof-trace.sh`, then converted with `linux-capture/sof_ipc_extract.py` |

The kext boots the firmware, then replays messages #0–19 (pipeline, components, DMIC DAI config). It sends #20–23 (PCM params, trigger start/stop, PCM free) on every capture start and stop, with the DMA addresses patched in. #24–31 free the topology at unload.

## Another laptop

The replay contains the DMIC configuration (PDM clocks, number of microphones, …) that Linux derived for **this** machine. It works on laptops with the same setup: Alder Lake, HDA codec plus a 2-channel DMIC on the DSP, and `sof-hda-generic-2ch.tplg` under Linux. For anything else:

1. Boot Linux on the laptop and make sure the internal mic works there with SOF.
2. Run `sudo bash linux-capture/sof-trace.sh setup`, reboot, then run `sudo bash linux-capture/sof-trace.sh collect`.
3. Run `python3 -I linux-capture/sof_ipc_extract.py ~/sof-ref/trace-*/dmesg-trace.txt ipc-replay.json`.
4. Check that the message order matches the one above (`type` field). If your topology has a different number of messages, the indices in `tranc/VoodooHDASof.cpp`/`.h` (`replay(0, 19)`, #20–31, `SOF_REPLAY_VOLUME`/`SWITCH`) need adjusting.
5. Update `SHA256SUMS` with `shasum -a 256 firmware/sof-adl.ri ipc-replay.json > SHA256SUMS`.

Other platforms (Tiger Lake `sof-tgl.ri`, Raptor Lake) use a different firmware file, and possibly different PCI IDs in `kSofDeviceIds`. They are untested.

`linux-capture/nhlt_parse.py` decodes the ACPI NHLT table, which shows the DMIC endpoints the BIOS declares.
