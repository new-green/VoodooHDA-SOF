# Değişiklik günlüğü

[English](CHANGELOG.md) | **Türkçe**

## 3.6.9-sof2 (2026-11-28), deneysel

### Düzeltilenler
- **Bazı soğuk açılışlarda DSP açılmıyordu** (`ROM halted`, `ROM_STATUS 80000003`): ROM firmware'i yüklerken DEVC (0x78) bit 11 NSNPEN açık olmalı. Kext, firmware/BIOS'un orada bıraktığı değeri geri koyuyordu. Test makinesinde bu değer 1'den 0'a değişti ve mikrofon kayboldu. NSNPEN artık açılış süresince her durumda açılıyor. Bu olduğunda log'da `note: firmware left DEVC NSNPEN=0` satırı çıkar.
- **Gürültü azaltmalı cihaz bazı uygulamalarda duraklıyor ve cızırdıyordu** (Brave'de Google Meet'te görüldü): tarayıcılar ses IO'sunu sık sık yeniden başlatıyor veya tamponun aynı kısmını iki kez okuyor. NR motoru bunu sıçrama sanıp RNNoise'u sıfırlıyor ve kısa bir sessizlik ekliyordu. Artık her okuma DMA yazma konumuna göre değerlendiriliyor. Tekrarlanan okumalar zaten işlenmiş sesi alıyor, küçük boşluklar da işleniyor ve yalnızca gerçekten büyük bir sıçramada ileri atlanıyor (ön beslemeyle, sıfırlamadan). Durdurma log'u atlama ve tekrar okuma sayılarını gösterir.

### Değişenler
- ROM durursa firmware yüklemesi en fazla 3 kez yeniden deneniyor.
- Kod yükleyici ve ICCMAX DMA tamponları 4 GB altında ayrılıyor.

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
