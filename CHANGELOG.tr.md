# Değişiklik günlüğü

[English](CHANGELOG.md) | **Türkçe**

## 3.6.9-sof1 (2026-11-25), deneysel

Intel SOF DMIC desteğinin ilk herkese açık sürümü. VoodooHDA Release368 tabanlıdır. Yalnızca macOS Tahoe 26.7 çalışan bir HP ProBook 440 G9 (Alder Lake-P, 8086:51C8) üzerinde test edildi.

### Eklenenler
- **SOF DSP açılışı** VoodooHDA'dan, `-voodoosof` ile açılıyor. Core 0'ı açıp ROM init'i çalıştırır, sonra imzalı firmware'i (sof-bin v2.2 `sof-adl.ri`) HDA kod yükleyici DMA'sı üzerinden yükler ve `FW_READY` (IPC3) mesajını bekler. Sıra, Linux SOF Intel sürücüsünden uyarlandı. Her şey yoklamayla (polling) yapılır, VoodooHDA'nın kesme yoluna dokunulmaz.
- **DMIC hattı**, Linux'un `sof-hda-generic-2ch.tplg` için gönderdiği IPC3 mesajları tekrar oynatılarak kurulur.
- **"Internal Microphone" (Dahili Mikrofon)**: 48 kHz, 2 kanal, 16 bit bir CoreAudio giriş cihazı. Örnekleri doğrudan DMIC host DMA halkasından alır.
- **Giriş seviyesi ve sessize alma**: Sistem Ayarları kaydırıcısı (−50…+20 dB) DSP'nin ses seviyesi bileşenini, sessize alma ise DSP anahtarını kullanır.
- **"Internal Microphone (Noise Reduction)" (Dahili Mikrofon (Gürültü Azaltma))**: aynı akış üzerinde ikinci bir cihaz. 90 Hz yüksek geçiren filtre, RNNoise v0.1.1 (çekirdekte çalışır) ve −1 dBFS limiter uygular. Gücü canlı olarak değiştirilebilir (%0–100). İki cihaz aynı anda kullanılabilir.
- **SofMicBar** menü çubuğu uygulaması: gürültü azaltma gücü kaydırıcısı, sessize alma düğmesi, her uygulamada çalışan ⌃⌥M kısayolu, açılışta başlatma ve Türkçe/İngilizce arayüz.
- **Boşta güç tasarrufu**: son kayıt bittikten 5 sn sonra DSP D0i3 uyku moduna geçer, yaklaşık 7 ms'de uyanır.
- **Açılış parametreleri**: `voodoosofnr=0|1|2`, `voodoosofpm=0`, `-voodoosofdbg`, `voodoosofstep=1..4`.
- **Derleme**: `build-kext.sh` yalnızca Command Line Tools ile derler. `SOF_LANG=tr` Türkçe cihaz adları verir.
- **Araçlar**: `sof/linux-capture/` başka bir laptop için IPC sırasını Linux'ta kaydeder. `tools/sof-test/` (`rec`, `sofctl`) test içindir.

### Bilinen sorunlar
- Uyku/uyanma test edilmedi ve S3 sonrasında DSP yeniden yüklenmiyor.
- Soğuk başlangıçta kaydın ilk ~15–70 ms'si sessiz gelir.
- Yalnızca PCI kimliği 0x51C8 ve 2 kanallı DMIC topolojisi destekleniyor.
