# Kext'e gömülen SOF verileri

[English](README.md) | **Türkçe**

`build-kext.sh` bu klasördeki iki dosyayı `VoodooHDA.kext` içine gömer. Önce ikisini de `SHA256SUMS` ile doğrular.

| Dosya | Nedir | Kaynak / lisans |
|---|---|---|
| `firmware/sof-adl.ri` | Alder Lake için Intel imzalı Sound Open Firmware (SOF 2.2.0, IPC3, ABI 3:22:1) | [thesofproject/sof-bin](https://github.com/thesofproject/sof-bin) `v2.2.x/sof-v2.2/intel-signed/sof-adl.ri`, değiştirilmeden (git blob `7f5246dd…`). Lisans: `firmware/LICENCE.Intel` (BSD-3-Clause ve diğerleri; yeniden dağıtıma izin verir) |
| `ipc-replay.json` | Firmware açıldıktan sonra Linux'un gönderdiği IPC3 mesajları, içerikleriyle birlikte: topoloji `sof-hda-generic-2ch.tplg` (sof-bin `sof-tplg-v2.2.1`), DMIC DAI ayarı, PCM parametreleri vb. | Linux 6.x çalışan bir HP ProBook 440 G9'da `linux-capture/sof-trace.sh` ile kaydedildi, sonra `linux-capture/sof_ipc_extract.py` ile dönüştürüldü |

Kext firmware'i başlatır, sonra #0–19 numaralı mesajları tekrar oynatır (pipeline, bileşenler, DMIC DAI ayarı). Her kayıt başlangıcında ve bitişinde #20–23'ü (PCM parametreleri, tetikleme başlat/durdur, PCM serbest bırakma) DMA adreslerini içine yazarak gönderir. #24–31 kext kaldırılırken topolojiyi serbest bırakır.

## Başka bir laptop

Tekrar oynatılan veriler, Linux'un **bu** makine için çıkardığı DMIC ayarlarını içerir (PDM saatleri, mikrofon sayısı, …). Aynı yapıdaki laptoplarda çalışır: Alder Lake, HDA kodek artı DSP üzerinde 2 kanallı DMIC ve Linux'ta `sof-hda-generic-2ch.tplg`. Bunlardan farklı her makine için:

1. Laptopta Linux açın ve dahili mikrofonun orada SOF ile çalıştığından emin olun.
2. `sudo bash linux-capture/sof-trace.sh setup` çalıştırın, yeniden başlatın, sonra `sudo bash linux-capture/sof-trace.sh collect` çalıştırın.
3. `python3 -I linux-capture/sof_ipc_extract.py ~/sof-ref/trace-*/dmesg-trace.txt ipc-replay.json` çalıştırın.
4. Mesaj sırasının yukarıdakiyle aynı olduğunu kontrol edin (`type` alanı). Topolojinizde mesaj sayısı farklıysa `tranc/VoodooHDASof.cpp`/`.h` içindeki numaraların (`replay(0, 19)`, #20–31, `SOF_REPLAY_VOLUME`/`SWITCH`) uyarlanması gerekir.
5. `SHA256SUMS` dosyasını `shasum -a 256 firmware/sof-adl.ri ipc-replay.json > SHA256SUMS` ile güncelleyin.

Diğer platformlar (Tiger Lake `sof-tgl.ri`, Raptor Lake) farklı bir firmware dosyası kullanır, `kSofDeviceIds` listesinde de farklı PCI kimlikleri gerekebilir. Bunlar test edilmedi.

`linux-capture/nhlt_parse.py`, BIOS'un bildirdiği DMIC uç noktalarını gösteren ACPI NHLT tablosunu çözer.
