import AppKit
import LocalForwarderCore

private final class AppModalWindow: NSWindow {
    var onCancel: (() -> Void)?
    override func cancelOperation(_ sender: Any?) { onCancel?() }
}

/// Application-modal dialogs keep the normal run loop alive for SSH callbacks.
final class AppModal: NSObject, NSWindowDelegate {
    enum Kind { case about, disconnectAll }
    static let repositoryURL = URL(string: "https://github.com/GuruJung/LocalForwarder")!
    static let licenseURL = URL(string: "https://github.com/GuruJung/LocalForwarder/blob/main/LICENSE")!
    let window: NSWindow
    let cancelButton: NSButton
    let confirmButton: NSButton?
    let linkButton: NSButton?
    let licenseButton: NSButton?
    let labels: [NSTextField]
    private let openURL: (URL) -> Void
    private var running = false

    init(_ kind: Kind, openURL: @escaping (URL) -> Void = { _ = NSWorkspace.shared.open($0) }) {
        self.openURL = openURL
        let panel = AppModalWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: kind == .about ? 300 : 200),
                                   styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window = panel
        cancelButton = NSButton(title: kind == .about ? "닫기" : "취소", target: nil, action: nil)
        confirmButton = kind == .disconnectAll ? NSButton(title: "연결 끊기", target: nil, action: nil) : nil
        linkButton = kind == .about ? NSButton(title: Self.repositoryURL.absoluteString, target: nil, action: nil) : nil
        licenseButton = kind == .about ? NSButton(title: "MIT License", target: nil, action: nil) : nil
        labels = (kind == .about ? ["LocalForwarder", "버전 " + ProductVersion.current] :
            ["모든 호스트의 연결을 끊으시겠습니까?", "연결 중 작업과 재시도·자동 복구 대기도 취소됩니다.\n저장된 자동 연결 옵션은 유지됩니다."])
            .map { NSTextField(wrappingLabelWithString: $0) }
        super.init()
        panel.onCancel = { [weak self] in self?.cancel() }
        panel.title = kind == .about ? "About LocalForwarder" : "전체 연결 끊기"
        panel.isReleasedWhenClosed = false; panel.delegate = self
        labels[0].font = .boldSystemFont(ofSize: 17)
        let body = NSStackView(); body.orientation = .vertical; body.alignment = .centerX; body.spacing = 10
        body.detachesHiddenViews = false
        if kind == .about {
            let icon = NSImageView(image: ForwardingIcon.image(size: 64, template: false))
            icon.widthAnchor.constraint(equalToConstant: 64).isActive = true
            icon.heightAnchor.constraint(equalToConstant: 64).isActive = true
            body.addArrangedSubview(icon)
        }
        for label in labels { label.alignment = .center; body.addArrangedSubview(label) }
        licenseButton?.action = #selector(openLicense)
        linkButton?.action = #selector(openRepository)
        for button in [licenseButton, linkButton].compactMap({ $0 }) {
            button.bezelStyle = .inline; button.isBordered = false
            button.contentTintColor = .linkColor; button.target = self
            body.addArrangedSubview(button)
        }
        cancelButton.target = self; cancelButton.action = #selector(cancel)
        cancelButton.keyEquivalent = "\r"
        confirmButton?.target = self; confirmButton?.action = #selector(confirm)
        let buttons = NSStackView(views: [cancelButton] + (confirmButton.map { [$0] } ?? [])); buttons.spacing = 8
        let content = panel.contentView!
        for view in [body, buttons] { view.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(view) }
        NSLayoutConstraint.activate([
            body.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            body.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            body.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            body.bottomAnchor.constraint(lessThanOrEqualTo: buttons.topAnchor, constant: -16),
            buttons.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            buttons.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16)])
        panel.defaultButtonCell = cancelButton.cell as? NSButtonCell
    }
    func run() -> Bool {
        guard !running else { window.makeKeyAndOrderFront(nil); return false }
        running = true
        let previous = NSApp.keyWindow
        let responder = previous?.firstResponder
        window.center(); NSApp.activate(ignoringOtherApps: true); window.makeKeyAndOrderFront(nil)
        let accepted = NSApp.runModal(for: window) == .OK
        running = false; window.orderOut(nil)
        if let previous, previous.isVisible { previous.makeKeyAndOrderFront(nil); previous.makeFirstResponder(responder) }
        return accepted
    }
    @objc func cancel() { finish(.cancel) }
    @objc private func confirm() { finish(.OK) }
    @objc private func openLicense() { openURL(Self.licenseURL) }
    @objc private func openRepository() { openURL(Self.repositoryURL) }
    func windowShouldClose(_ sender: NSWindow) -> Bool { cancel(); return false }
    private func finish(_ response: NSApplication.ModalResponse) {
        guard running else { return }
        running = false
        NSApp.stopModal(withCode: response)
    }
}
