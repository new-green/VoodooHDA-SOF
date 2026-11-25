// SofMicBar — menu bar slider for the noise reduction strength of "Dahili Mikrofon (Gürültü Azaltma)".
// Part of the VoodooHDA SOF port. Talks to the VoodooHDASofEngine through its "SofNrStrength"
// registry property (0..100 %, 50 = -20 dB band gain floor, 100 = no floor).
import AppKit
import IOKit

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
    var lastSent = -1
    var retries = 0

    var saved: Int {
        get { UserDefaults.standard.object(forKey: kDefaultsKey) as? Int ?? 50 }
        set { UserDefaults.standard.set(newValue, forKey: kDefaultsKey) }
    }

    func applicationDidFinishLaunching(_ n: Notification) {
        item.button?.image = NSImage(systemSymbolName: "mic.fill", accessibilityDescription: "Gürültü azaltma")
        item.button?.toolTip = "Dahili Mikrofon — gürültü azaltma"

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

    func menuWillOpen(_ m: NSMenu) { refreshStatus() }

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
