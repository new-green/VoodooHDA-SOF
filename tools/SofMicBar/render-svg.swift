// render.swift <in.svg> <out.png> <size>: rasterize an SVG with AppKit (keeps transparency)
import AppKit
let a = CommandLine.arguments
guard let img = NSImage(contentsOf: URL(fileURLWithPath: a[1])) else { print("cannot load"); exit(1) }
let n = Int(a[3])!
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: n, pixelsHigh: n, bitsPerSample: 8, samplesPerPixel: 4,
                           hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSColor.clear.set(); NSRect(x: 0, y: 0, width: n, height: n).fill()
img.draw(in: NSRect(x: 0, y: 0, width: n, height: n))
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[2]))
