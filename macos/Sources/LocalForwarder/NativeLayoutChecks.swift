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
    static func settle(_ window: NSWindow) throws {
        guard let content = window.contentView else { throw AppError("Native layout: missing window content") }
        func geometry(_ view: NSView) -> [NSRect] {
            [view.frame, view.bounds] + view.subviews.flatMap { geometry($0) }
        }
        func pendingLayout(_ view: NSView) -> Bool {
            view.needsLayout || view.needsUpdateConstraints || view.subviews.contains { pendingLayout($0) }
        }
        var previous: [NSRect] = []
        var stablePasses = 0
        // Autohiding scrollers can resize the clip/document during a layout pass;
        // AppKit then schedules the arranged rows for a subsequent pass. Drain those
        // passes before measuring, without changing production constraints or bounds.
        for _ in 0..<50 {
            content.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.02))
            content.layoutSubtreeIfNeeded()
            let current = geometry(content)
            stablePasses = current == previous && !pendingLayout(content) ? stablePasses + 1 : 0
            if stablePasses >= 3 { return }
            previous = current
        }
        throw AppError("Native layout: window geometry did not settle within 50 AppKit passes (\(window.title), \(content.bounds))")
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
            func alignedRect() -> NSRect { view.superview!.convert(view.alignmentRect(forFrame: view.frame), to: container) }
            var rect = alignedRect()
            try require(rect.width > 0 && rect.height > 0, label + " zero size \(rect)")
            try require(rect.minX >= -1 && rect.maxX <= container.bounds.width + 1,
                        label + " horizontal clipping \(rect) in \(container.bounds)")
            try require(rect.minY >= -1 && rect.maxY <= container.bounds.height + 1,
                        label + " vertical clipping \(rect) in \(container.bounds)")
            if let field = view as? NSTextField, !field.isEditable, field.cell?.wraps == false,
               field.intrinsicContentSize.width > 0 {
                try require(frame.width + 1 >= field.intrinsicContentSize.width, label + " truncated label")
            }
            if let button = view as? NSButton {
                try require(frame.width + 1 >= button.intrinsicContentSize.width, label + " truncated button")
            }
            if let scroll {
                let clip = scroll.contentView
                if container.bounds.height <= clip.bounds.height {
                    let visible = clip.convert(clip.bounds, to: container)
                    try require(rect.minY >= visible.minY - 1 && rect.maxY <= visible.maxY + 1,
                                label + " outside fully fitting document viewport")
                    continue
                }
                // Check both ends of tall scroll widgets, and full access for each button/field.
                for edge in [rect.minY, rect.maxY - clip.bounds.height] {
                    let y = max(0, min(edge, container.bounds.height - clip.bounds.height))
                    let before = container.bounds
                    clip.scroll(to: NSPoint(x: 0, y: y)); scroll.reflectScrolledClipView(clip)
                    if let window = scroll.window { try settle(window) }
                    // Scroller visibility can change the document width during the probe.
                    // Measure the resulting layout rather than mixing pre-scroll row frames
                    // with post-scroll container bounds on the next control.
                    rect = alignedRect()
                    if before.size != container.bounds.size {
                        writeLine("LF_UI_SCROLL_LAYOUT \(label): document \(before) -> \(container.bounds), clip \(clip.bounds), control \(rect)")
                    }
                    try require(rect.minX >= -1 && rect.maxX <= container.bounds.width + 1,
                                label + " horizontal clipping after scrolling \(rect) in \(container.bounds)")
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
        try store.save(Host(name: "Layout test", target: "example.invalid", forwards: [
            Forward(id: "web", description: "사내 웹 대시보드"),
            Forward(id: "database", enabled: false, localPort: 15432, remoteHost: "database", remotePort: 5432,
                    description: "읽기 전용 DB")], autoConnect: false))
        let manager = ConnectionManager(store: store, sessionFactory: { Session() })
        let application = NSApplication.shared
        application.setActivationPolicy(.accessory); application.finishLaunching()
        for language in L10n.languages {
            L10n.configure(selection: language)
            writeLine("LF_UI_LANGUAGE " + language)
            try SettingsController.checkNativeLayout(store: store, manager: manager)
            try MainActor.assumeIsolated { try AppDelegate.checkAppDialogs(store: store) }
        }
    }
}
