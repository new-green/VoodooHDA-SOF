// SofMicBar — menu bar slider for the noise reduction strength of "Dahili Mikrofon (Gürültü Azaltma)".
// Part of the VoodooHDA SOF port. Talks to the VoodooHDASofEngine through its "SofNrStrength"
// registry property (0..100 %, 50 = -20 dB band gain floor, 100 = no floor).
// Mute: the CoreAudio input mute of the SOF devices (the kext sends it to the DSP and keeps both
// "Dahili Mikrofon" devices in sync); macOS Sound settings has no input mute button.
import AppKit
import IOKit
import CoreAudio

let kKey = "SofNrStrength"
let kDefaultsKey = "nrStrength"
let kAgentLabel = "local.voodoohda.sofmicbar"

func findNrEngine() -> io_service_t? {
    var iter: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("VoodooHDASofEngine"), &iter) == KERN_SUCCESS else { return nil }
    defer { IOObjectRelease(iter) }
    while case let s = IOIteratorNext(iter), s != 0 {
        if let v = IORegistryEntryCreateCFProperty(s, kKey as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue(), v is NSNumber {
            return s
        }
        IOObjectRelease(s)
    }
    return nil
}

func readStrength() -> Int? {
    guard let s = findNrEngine() else { return nil }
    defer { IOObjectRelease(s) }
    return (IORegistryEntryCreateCFProperty(s, kKey as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber)?.intValue
}

// CoreAudio devices published by VoodooHDASofEngine (raw + noise-reduced)
func sofDevices() -> [AudioDeviceID] {
    var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                       mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size) == noErr else { return [] }
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &ids) == noErr else { return [] }
    return ids.filter { id in
        var ua = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyDeviceUID,
                                            mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var uid: Unmanaged<CFString>?
        var us = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &ua, 0, nil, &us, &uid) == noErr, let u = uid?.takeRetainedValue() else { return false }
        return (u as String).hasPrefix("VoodooHDASofEngine")
    }
}

var muteAddress = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyMute,
                                             mScope: kAudioObjectPropertyScopeInput, mElement: kAudioObjectPropertyElementMain)

func readMute() -> Bool? {
    for d in sofDevices() {
        var v: UInt32 = 0
        var sz = UInt32(MemoryLayout<UInt32>.size)
        if AudioObjectGetPropertyData(d, &muteAddress, 0, nil, &sz, &v) == noErr { return v != 0 }
    }
    return nil
}

@discardableResult
func writeMute(_ on: Bool) -> Bool {
    var ok = false
    for d in sofDevices() {
        var v: UInt32 = on ? 1 : 0
        if AudioObjectSetPropertyData(d, &muteAddress, 0, nil, UInt32(MemoryLayout<UInt32>.size), &v) == noErr { ok = true }
    }
    return ok
}

@discardableResult
func writeStrength(_ pct: Int) -> Bool {
    guard let s = findNrEngine() else { return false }
    defer { IOObjectRelease(s) }
    let dict = [kKey: NSNumber(value: UInt32(max(0, min(100, pct))))] as CFDictionary
    return IORegistryEntrySetCFProperties(s, dict) == KERN_SUCCESS
}

final class App: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    let menu = NSMenu()
    let slider = NSSlider(value: 50, minValue: 0, maxValue: 100, target: nil, action: nil)
    let valueLabel = NSTextField(labelWithString: "")
    let statusLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    let loginItem = NSMenuItem(title: "Açılışta başlat", action: #selector(toggleLogin), keyEquivalent: "")
    let muteItem = NSMenuItem(title: "Mikrofonu sessize al", action: #selector(toggleMute), keyEquivalent: "m")
    var watched = Set<AudioDeviceID>()
    var lastSent = -1
    var retries = 0

    var saved: Int {
        get { UserDefaults.standard.object(forKey: kDefaultsKey) as? Int ?? 50 }
        set { UserDefaults.standard.set(newValue, forKey: kDefaultsKey) }
    }

    func applicationDidFinishLaunching(_ n: Notification) {
        updateIcon(muted: false)

        muteItem.target = self
        menu.addItem(muteItem)
        menu.addItem(.separator())

        let title = NSMenuItem(title: "Gürültü azaltma", action: nil, keyEquivalent: "")
        title.isEnabled = false
        menu.addItem(title)

        let view = NSView(frame: NSRect(x: 0, y: 0, width: 260, height: 54))
        slider.frame = NSRect(x: 18, y: 22, width: 224, height: 24)
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(sliderMoved)
        let lo = NSTextField(labelWithString: "Kapalı")
        let hi = NSTextField(labelWithString: "Güçlü")
        for l in [lo, hi, valueLabel] { l.font = NSFont.systemFont(ofSize: 11); l.textColor = .secondaryLabelColor }
        lo.frame = NSRect(x: 18, y: 4, width: 60, height: 16)
        hi.frame = NSRect(x: 192, y: 4, width: 50, height: 16); hi.alignment = .right
        valueLabel.frame = NSRect(x: 80, y: 4, width: 100, height: 16); valueLabel.alignment = .center
        view.addSubview(slider); view.addSubview(lo); view.addSubview(hi); view.addSubview(valueLabel)
        let sliderItem = NSMenuItem(); sliderItem.view = view
        menu.addItem(sliderItem)

        statusLine.isEnabled = false
        menu.addItem(statusLine)
        menu.addItem(.separator())
        loginItem.target = self
        menu.addItem(loginItem)
        menu.addItem(NSMenuItem(title: "Çık", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        menu.delegate = self
        item.menu = menu

        slider.integerValue = saved
        updateLabel()
        pushSaved()
        watchMute()
        // the device list changes when the kext (re)loads: watch the new devices too
        var da = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                            mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &da, DispatchQueue.main) { [weak self] _, _ in
            self?.watchMute()
        }
    }

    // icon + check mark follow the mute state, also when another app changes it
    func watchMute() {
        for d in sofDevices() where !watched.contains(d) {
            if AudioObjectAddPropertyListenerBlock(d, &muteAddress, DispatchQueue.main, { [weak self] _, _ in
                self?.refreshMute()
            }) == noErr { watched.insert(d) }
        }
        refreshMute()
    }

    func refreshMute() {
        let m = readMute()
        muteItem.isEnabled = m != nil
        muteItem.state = m == true ? .on : .off
        updateIcon(muted: m == true)
    }

    func updateIcon(muted: Bool) {
        item.button?.image = NSImage(systemSymbolName: muted ? "mic.slash.fill" : "mic.fill",
                                     accessibilityDescription: muted ? "Mikrofon sessiz" : "Mikrofon açık")
        item.button?.toolTip = muted ? "Dahili Mikrofon — sessiz" : "Dahili Mikrofon — gürültü azaltma"
    }

    @objc func toggleMute() {
        if let m = readMute() { writeMute(!m) }
        refreshMute()
    }

    // the kext starts at its own default after every boot: push the saved value (retry while it loads)
    func pushSaved() {
        if writeStrength(saved) {
            lastSent = saved
            refreshStatus()
        } else {
            refreshStatus()
            if retries < 24 {
                retries += 1
                DispatchQueue.main.asyncAfter(deadline: .now() + 5) { [weak self] in self?.pushSaved() }
            }
        }
    }

    func updateLabel() {
        let p = slider.integerValue
        valueLabel.stringValue = p == 0 ? "%0" : p == 100 ? "%100" : "%\(p) (−\(Int((Double(p) * 0.4).rounded())) dB)"
    }

    func refreshStatus() {
        if let cur = readStrength() {
            statusLine.title = "Sürücüde: %\(cur)"
            slider.isEnabled = true
        } else {
            statusLine.title = "Cihaz bulunamadı (sürücü d10+ gerekli)"
            slider.isEnabled = false
        }
        loginItem.state = FileManager.default.fileExists(atPath: agentPath) ? .on : .off
    }

    func menuWillOpen(_ m: NSMenu) { refreshStatus(); refreshMute() }

    @objc func sliderMoved() {
        let p = slider.integerValue
        updateLabel()
        if p != lastSent, writeStrength(p) { lastSent = p }
        saved = p
        if NSApp.currentEvent?.type == .leftMouseUp { refreshStatus() }
    }

    // login start: a per-user LaunchAgent (works with an ad-hoc signed app, no admin rights)
    var agentPath: String { NSHomeDirectory() + "/Library/LaunchAgents/\(kAgentLabel).plist" }

    @objc func toggleLogin() {
        let fm = FileManager.default
        if fm.fileExists(atPath: agentPath) {
            try? fm.removeItem(atPath: agentPath)
        } else if let exe = Bundle.main.executablePath {
            let plist: [String: Any] = ["Label": kAgentLabel, "ProgramArguments": [exe], "RunAtLoad": true,
                                        "LimitLoadToSessionType": "Aqua", "ProcessType": "Interactive"]
            try? fm.createDirectory(atPath: NSHomeDirectory() + "/Library/LaunchAgents", withIntermediateDirectories: true)
            (plist as NSDictionary).write(toFile: agentPath, atomically: true)
        }
        refreshStatus()
    }
}

let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
