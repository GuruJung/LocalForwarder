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
    private let descriptionField = NSTextField()
    private let localPort = NSTextField()
    private let remoteHost = NSTextField()
    private let remotePort = NSTextField()
    private var finished = false

    init(parent: NSWindow, rules: [Forward], editing rule: Forward?, completion: @escaping ([Forward]?) -> Void) {
        draft = RuleDraft(rule ?? Forward()); self.rules = rules; replacing = rule?.id
        self.completion = completion
        let available = parent.screen?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 900)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: min(470, available.width - 40, parent.frame.width - 32), height: 252),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
        super.init()
        window.title = rule == nil ? "새 규칙" : "규칙 편집"
        window.isReleasedWhenClosed = false; window.delegate = self
        enabled.state = draft.rule.enabled ? .on : .off
        descriptionField.stringValue = draft.rule.description
        descriptionField.cell?.usesSingleLineMode = true
        localPort.stringValue = draft.localPort; remotePort.stringValue = draft.remotePort
        remoteHost.stringValue = draft.rule.remoteHost
        let body = NSStackView(); body.orientation = .vertical; body.alignment = .leading; body.spacing = 10
        body.detachesHiddenViews = false
        func row(_ title: String, _ field: NSTextField) -> NSStackView {
            let label = NSTextField(labelWithString: title); label.widthAnchor.constraint(equalToConstant: 100).isActive = true
            let row = NSStackView(views: [label, field]); row.spacing = 10
            row.detachesHiddenViews = false
            for view in row.arrangedSubviews { row.setVisibilityPriority(.mustHold, for: view) }
            field.setContentHuggingPriority(.defaultLow, for: .horizontal)
            return row
        }
        for view in [enabled, row("설명 (선택)", descriptionField), row("로컬 포트", localPort), row("목적지 호스트", remoteHost), row("목적지 포트", remotePort)] as [NSView] {
            body.addArrangedSubview(view); body.setVisibilityPriority(.mustHold, for: view)
            view.widthAnchor.constraint(equalTo: body.widthAnchor).isActive = true
        }
        let cancel = NSButton(title: "취소", target: self, action: #selector(cancel)); cancel.keyEquivalent = "\u{1b}"
        let okay = NSButton(title: "확인", target: self, action: #selector(confirm)); okay.keyEquivalent = "\r"
        let footer = NSStackView(views: [cancel, okay]); footer.spacing = 8
        footer.detachesHiddenViews = false
        for view in footer.arrangedSubviews { footer.setVisibilityPriority(.mustHold, for: view) }
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
        guard !finished, window.attachedSheet == nil else { return }
        draft.rule.enabled = enabled.state == .on; draft.rule.remoteHost = remoteHost.stringValue
        draft.rule.description = descriptionField.stringValue
        draft.localPort = localPort.stringValue; draft.remotePort = remotePort.stringValue
        do { finish(try draft.applying(to: rules, replacing: replacing)) }
        catch {
            let alert = NSAlert(); alert.messageText = "입력을 확인하세요."
            alert.informativeText = error.localizedDescription; alert.alertStyle = .warning
            alert.beginSheetModal(for: window, completionHandler: nil)
        }
    }
    @objc func cancel() { finish(nil) }
    func windowShouldClose(_ sender: NSWindow) -> Bool { cancel(); return false }
    private func finish(_ result: [Forward]?) {
        guard !finished else { return }; finished = true
        if let alert = window.attachedSheet { window.endSheet(alert); alert.orderOut(nil) }
        window.sheetParent?.endSheet(window); window.orderOut(nil)
        completion(result)
    }
}
