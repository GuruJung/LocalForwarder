#!/usr/bin/env swift
import AppKit
import Foundation

guard CommandLine.arguments.count == 2 else { fputs("Usage: generate-macos-icon.swift output.icns\n", stderr); exit(2) }
let output = URL(fileURLWithPath: CommandLine.arguments[1])
let iconset = FileManager.default.temporaryDirectory.appendingPathComponent("LocalForwarder-\(UUID().uuidString).iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: iconset) }
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = size * scale
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
        let transform = NSAffineTransform(); transform.scale(by: CGFloat(pixels) / 18); transform.concat()
        NSColor(calibratedRed: 0.10, green: 0.23, blue: 0.39, alpha: 1).setFill()
        NSBezierPath(roundedRect: NSRect(x: 0.5, y: 0.5, width: 17, height: 17), xRadius: 4, yRadius: 4).fill()
        NSColor.white.setStroke()
        let path = NSBezierPath(); path.lineWidth = 1.5; path.lineCapStyle = .round; path.lineJoinStyle = .round
        path.move(to: NSPoint(x: 3, y: 5)); path.line(to: NSPoint(x: 3, y: 13))
        path.move(to: NSPoint(x: 15, y: 5)); path.line(to: NSPoint(x: 15, y: 13))
        path.move(to: NSPoint(x: 6, y: 11)); path.line(to: NSPoint(x: 12, y: 11))
        path.move(to: NSPoint(x: 10, y: 13)); path.line(to: NSPoint(x: 12, y: 11)); path.line(to: NSPoint(x: 10, y: 9))
        path.move(to: NSPoint(x: 12, y: 7)); path.line(to: NSPoint(x: 6, y: 7))
        path.move(to: NSPoint(x: 8, y: 9)); path.line(to: NSPoint(x: 6, y: 7)); path.line(to: NSPoint(x: 8, y: 5))
        path.stroke(); NSGraphicsContext.restoreGraphicsState()
        let name = "icon_\(size)x\(size)" + (scale == 2 ? "@2x" : "") + ".png"
        try bitmap.representation(using: .png, properties: [:])!.write(to: iconset.appendingPathComponent(name))
    }
}
let process = Process(); process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconset.path, "-o", output.path]
try process.run(); process.waitUntilExit(); exit(process.terminationStatus)
