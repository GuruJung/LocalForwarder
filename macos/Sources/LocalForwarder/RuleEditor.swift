import AppKit
import LocalForwarderCore

/// A sheet-local draft. Only the successful confirmation callback changes its parent.
final class RuleEditor: NSObject, NSWindowDelegate {
    let window: NSWindow
    private var draft: RuleDraft
    private let rules: [Forward]
    private let replacing: String?
    private let completion: ([Forward]?) -> Void
    private let enabled = NSButton(checkboxWithTitle: "이 규칙 사용", target: nil, action: nil)
    private let localPort = NSTextField()
    private let remoteHost = NSTextField()
    private let remotePort = NSTextField()
    private let error = NSTextField(wrappingLabelWithString: "")
    private var finished = false

    init(parent: NSWindow, rules: [Forward], editing rule: Forward?, completion: @escaping ([Forward]?) -> Void) {
        draft = RuleDraft(rule ?? Forward()); self.rules = rules; replacing = rule?.id
        self.completion = completion
        let available = parent.screen?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 900)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: min(470, available.width - 40), height: 260),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
        super.init()
        window.title = rule == nil ? "새 규칙" : "규칙 편집"
        window.isReleasedWhenClosed = false; window.delegate = self
        enabled.state = draft.rule.enabled ? .on : .off
        localPort.stringValue = draft.localPort; remotePort.stringValue = draft.remotePort
        remoteHost.stringValue = draft.rule.remoteHost
        let body = NSStackView(); body.orientation = .vertical; body.alignment = .leading; body.spacing = 10
        func row(_ title: String, _ field: NSTextField) -> NSStackView {
            let label = NSTextField(labelWithString: title); label.widthAnchor.constraint(equalToConstant: 100).isActive = true
            let row = NSStackView(views: [label, field]); row.spacing = 10
            field.setContentHuggingPriority(.defaultLow, for: .horizontal)
            return row
        }
        for view in [enabled, row("로컬 포트", localPort), row("목적지 호스트", remoteHost), row("목적지 포트", remotePort), error] as [NSView] {
            body.addArrangedSubview(view); view.widthAnchor.constraint(equalTo: body.widthAnchor).isActive = true
        }
        error.textColor = .systemRed
        let cancel = NSButton(title: "취소", target: self, action: #selector(cancel)); cancel.keyEquivalent = "\u{1b}"
        let okay = NSButton(title: "확인", target: self, action: #selector(confirm)); okay.keyEquivalent = "\r"
        let footer = NSStackView(views: [cancel, okay]); footer.spacing = 8
        let content = window.contentView!
        body.translatesAutoresizingMaskIntoConstraints = false; footer.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(body); content.addSubview(footer)
        NSLayoutConstraint.activate([
            body.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16),
            body.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            body.topAnchor.constraint(equalTo: content.topAnchor, constant: 16),
            body.bottomAnchor.constraint(lessThanOrEqualTo: footer.topAnchor, constant: -12),
            footer.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            footer.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16)])
    }
    @objc private func confirm() {
        draft.rule.enabled = enabled.state == .on; draft.rule.remoteHost = remoteHost.stringValue
        draft.localPort = localPort.stringValue; draft.remotePort = remotePort.stringValue
        do { finish(try draft.applying(to: rules, replacing: replacing)) }
        catch { self.error.stringValue = error.localizedDescription }
    }
    @objc func cancel() { finish(nil) }
    func windowShouldClose(_ sender: NSWindow) -> Bool { cancel(); return false }
    private func finish(_ result: [Forward]?) {
        guard !finished else { return }; finished = true
        window.sheetParent?.endSheet(window); window.orderOut(nil)
        completion(result)
    }
}
