// sofctl mute get|0|1  |  sofctl nr get|<0..100>   — SOF mic mute (CoreAudio) and NR strength (IORegistry)
import CoreAudio
import IOKit
import Foundation
let args = CommandLine.arguments
func sofDevices() -> [AudioDeviceID] {
  var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
  var size: UInt32 = 0; AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size)
  var ids = [AudioDeviceID](repeating: 0, count: Int(size) / 4); AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &ids)
  return ids.filter { id in
    var ua = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyDeviceUID, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var uid: Unmanaged<CFString>?; var us = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(id, &ua, 0, nil, &us, &uid) == noErr, let u = uid?.takeRetainedValue() else { return false }
    return (u as String).hasPrefix("VoodooHDASofEngine") }
}
if args.count > 2 && args[1] == "mute" {
  var m = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyMute, mScope: kAudioObjectPropertyScopeInput, mElement: kAudioObjectPropertyElementMain)
  if args[2] != "get", let d = sofDevices().first { var v: UInt32 = args[2] == "1" ? 1 : 0; AudioObjectSetPropertyData(d, &m, 0, nil, 4, &v); usleep(300000) }
  for d in sofDevices() { var v: UInt32 = 9; var s: UInt32 = 4; AudioObjectGetPropertyData(d, &m, 0, nil, &s, &v); print("dev \(d) mute \(v)") }
} else if args.count > 2 && args[1] == "nr" {
  var it: io_iterator_t = 0; IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("VoodooHDASofEngine"), &it)
  while case let s = IOIteratorNext(it), s != 0 {
    guard let v = IORegistryEntryCreateCFProperty(s, "SofNrStrength" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() else { continue }
    if args[2] != "get" { let r = IORegistryEntrySetCFProperties(s, ["SofNrStrength": NSNumber(value: UInt32(args[2])!)] as CFDictionary); print("set -> 0x" + String(UInt32(bitPattern: r), radix: 16)) }
    print("SofNrStrength was \(v)")
  }
} else { print("usage: sofctl mute get|0|1 | sofctl nr get|<0..100>") }
