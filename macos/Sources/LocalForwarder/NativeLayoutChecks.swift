import AppKit
import LocalForwarderCore

/// Runs against real AppKit constraints in CI, without loading the user's configuration or SSH.
enum NativeLayoutChecks {
    final class Session: SSHSessionProtocol {
        let state: ConnectionState = .disconnected
        let lastError = ""
        let forwards: [ForwardRuntime] = []
        var onChange: (() -> Void)?
        var onError: ((String) -> Void)?
        func start(_ host: Host) { fatalError("UI layout checks must never start SSH") }
        func stop(completion: @escaping () -> Void) { completion() }
    }
    static func require(_ condition: Bool, _ message: String) throws {
        if !condition { throw AppError("Native layout: " + message) }
    }
    static func settle(_ window: NSWindow) {
        window.contentView?.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.03))
        window.contentView?.layoutSubtreeIfNeeded()
    }
    /// Enumerate arranged children, including controls AppKit could otherwise detach under pressure.
    static func leaves(_ view: NSView) -> [NSView] {
        if let stack = view as? NSStackView { return stack.arrangedSubviews.flatMap { leaves($0) } }
        return [view]
    }
    static func check(_ views: [NSView], in container: NSView, scroll: NSScrollView? = nil, context: String) throws {
        for view in views {
            let name = (view as? NSButton)?.title ?? (view as? NSTextField)?.stringValue ?? String(describing: type(of: view))
            let label = "\(context): \(name)"
            var ancestor: NSView? = view
            while let current = ancestor, current !== container {
                try require(!current.isHidden && current.superview != nil, label + " hidden/detached")
                ancestor = current.superview
            }
            try require(ancestor === container, label + " missing from container")
            let frame = view.convert(view.bounds, to: container)
            // AppKit positions controls by alignment rects. NSTextField frames have
            // optical outsets (for example x=-2, width=116 for a 112pt label) that
            // contain no clipped text. Ask the control for its native content geometry
            // instead of treating those frame outsets as lost content.
            let rect = view.superview!.convert(view.alignmentRect(forFrame: view.frame), to: container)
            try require(rect.width > 0 && rect.height > 0, label + " zero size \(rect)")
            try require(rect.minX >= -1 && rect.maxX <= container.bounds.width + 1,
                        label + " horizontal clipping \(rect) in \(container.bounds)")
            try require(rect.minY >= -1 && rect.maxY <= container.bounds.height + 1,
                        label + " vertical clipping \(rect) in \(container.bounds)")
            if let button = view as? NSButton {
                try require(frame.width + 1 >= button.intrinsicContentSize.width, label + " truncated button")
            }
            if let scroll {
                let clip = scroll.contentView
                // Check both ends of tall scroll widgets, and full access for each button/field.
                for edge in [rect.minY, rect.maxY - clip.bounds.height] {
                    let y = max(0, min(edge, container.bounds.height - clip.bounds.height))
                    clip.scroll(to: NSPoint(x: 0, y: y)); scroll.reflectScrolledClipView(clip)
                    let visible = clip.convert(clip.bounds, to: container)
                    try require(visible.intersects(rect), label + " unreachable by scrolling")
                    if rect.height <= visible.height {
                        try require(rect.minY >= visible.minY - 1 && rect.maxY <= visible.maxY + 1,
                                    label + " cannot scroll fully into view")
                    }
                }
            }
        }
    }
    static func run() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("LocalForwarder-UI-" + UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(directory: directory)
        try store.save(Host(name: "Layout test", target: "example.invalid", forwards: [Forward()], autoConnect: false))
        let manager = ConnectionManager(store: store, sessionFactory: { Session() })
        let application = NSApplication.shared
        application.setActivationPolicy(.accessory); application.finishLaunching()
        try SettingsController.checkNativeLayout(store: store, manager: manager)
    }
}
