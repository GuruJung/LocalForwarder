import AppKit

/// Drawn as vectors at the target scale: two endpoints and opposing forwarding paths.
enum ForwardingIcon {
    static func image(size: CGFloat = 18, template: Bool = true) -> NSImage {
        let image = NSImage(size: NSSize(width: size, height: size), flipped: false) { rect in
            let scale = rect.width / 18
            NSGraphicsContext.saveGraphicsState()
            let transform = NSAffineTransform(); transform.scale(by: scale); transform.concat()
            if !template {
                NSColor(calibratedRed: 0.10, green: 0.23, blue: 0.39, alpha: 1).setFill()
                NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: 18, height: 18), xRadius: 4, yRadius: 4).fill()
            }
            (template ? NSColor.labelColor : .white).setStroke()
            let path = NSBezierPath(); path.lineWidth = 1.5; path.lineCapStyle = .round; path.lineJoinStyle = .round
            path.move(to: NSPoint(x: 3, y: 5)); path.line(to: NSPoint(x: 3, y: 13))
            path.move(to: NSPoint(x: 15, y: 5)); path.line(to: NSPoint(x: 15, y: 13))
            path.move(to: NSPoint(x: 6, y: 11)); path.line(to: NSPoint(x: 12, y: 11))
            path.move(to: NSPoint(x: 10, y: 13)); path.line(to: NSPoint(x: 12, y: 11)); path.line(to: NSPoint(x: 10, y: 9))
            path.move(to: NSPoint(x: 12, y: 7)); path.line(to: NSPoint(x: 6, y: 7))
            path.move(to: NSPoint(x: 8, y: 9)); path.line(to: NSPoint(x: 6, y: 7)); path.line(to: NSPoint(x: 8, y: 5))
            path.stroke(); NSGraphicsContext.restoreGraphicsState(); return true
        }
        image.isTemplate = template
        return image
    }
}
