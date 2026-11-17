import AppKit
import LocalForwarderCore

typealias Host = LocalForwarderCore.Host

private struct HostDraft: Equatable {
    var host: Host
    var portText: String
    var rulePorts: [String: [String: String]]
    init(_ host: Host) {
        self.host = host; portText = host.port.map(String.init) ?? ""
        rulePorts = Dictionary(uniqueKeysWithValues: host.forwards.map {
            ($0.id, ["localPort": String($0.localPort), "remotePort": String($0.remotePort)])
        })
    }
    func validated() throws -> Host {
        var result = host
        let text = portText.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { result.port = nil }
        else if let value = Int(text) { result.port = value }
        else { throw AppError("SSH 포트는 숫자여야 합니다.") }
        try result.validate()
        return result
    }
}

private final class FormDocumentView: NSView { override var isFlipped: Bool { true } }

final class SettingsController: NSObject, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate {
    private let store: ConfigurationStore
    private let manager: ConnectionManager
    private(set) var isOpen = false
    private var startup = false
    var onClose: ((Bool, Bool) -> Void)?
    private var window: NSWindow!
    private var statusSheet: NSWindow?
    private var ruleEditor: RuleEditor?
    private var statusHostID: String?
    private let hostsTable = NSTableView()
    private let rulesTable = NSTableView()
    private let runtimeTable = NSTableView()
    private let nameField = NSTextField()
    private let descriptionField = NSTextField()
    private let targetField = NSTextField()
    private let userField = NSTextField()
    private let portField = NSTextField()
    private let keyField = NSTextField()
    private let autoConnect = NSButton(checkboxWithTitle: "자동 연결", target: nil, action: nil)
    private let exitOnFailure = NSButton(checkboxWithTitle: "ExitOnForwardFailure", target: nil, action: nil)
    private let retryRefused = NSButton(checkboxWithTitle: "연결 거부 시 한 번 재시도", target: nil, action: nil)
    private let command = NSTextView()
    private let status = NSTextField(wrappingLabelWithString: "")
    private let statusHeading = NSTextField(wrappingLabelWithString: "")
    private var editRuleButton: NSButton!
    private var deleteRuleButton: NSButton!
    private var connectButton: NSButton!
    private var authButton: NSButton!
    private var order: [String] = []
    private var drafts: [String: HostDraft] = [:]
    private var baseline: [String: HostDraft] = [:]
    private var selectedID: String?
    private var loading = false
    private var authTimers: [String: Timer] = [:]
    private var rootStack: NSStackView!
    private var sidebarStack: NSStackView!
    private var formScrollView: NSScrollView!
    private var formDocumentView: NSView!
    private var formStack: NSStackView!
    private var footerStack: NSStackView!
    private var widePaneConstraints: [NSLayoutConstraint] = []
    private var narrowPaneConstraints: [NSLayoutConstraint] = []
    private var adaptiveRows: [AdaptiveStack] = []
    private var narrowPanes = false

    init(store: ConfigurationStore, manager: ConnectionManager, availableFrame: NSRect? = nil) {
        self.store = store; self.manager = manager
        super.init(); makeWindow(availableFrame: availableFrame)
    }
    func open(select id: String? = nil, startup: Bool = false) {
        if isOpen {
            if var sheet = window.attachedSheet {
                while let child = sheet.attachedSheet { sheet = child }
                window.makeKeyAndOrderFront(nil); sheet.makeKeyAndOrderFront(nil)
                NSApp.activate(ignoringOtherApps: true)
                return
            }
            if let id, drafts[id] != nil { capture(); select(id) }
            window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
            return
        }
        self.startup = startup; isOpen = true
        drafts = [:]; baseline = [:]; order = []; selectedID = nil
        var loadError: String?
        do {
            try manager.reloadConfiguration()
            for host in try store.load().hosts {
                order.append(host.id); drafts[host.id] = HostDraft(host); baseline[host.id] = HostDraft(host)
            }
            for host in manager.hosts where drafts[host.id] == nil {
                order.append(host.id); drafts[host.id] = HostDraft(manager.runtime(host.id)?.executionHost ?? host)
            }
        } catch { loadError = error.localizedDescription }
        if order.isEmpty { addDraft() }
        hostsTable.reloadData()
        select(id.flatMap { drafts[$0] == nil ? nil : $0 } ?? order[0])
        window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        if let loadError { showError(loadError + "\n원본 파일은 보존되며 이 파일로 자동 연결하지 않습니다.") }
    }
    func refresh() {
        guard isOpen else { return }
        updateStatus(); hostsTable.reloadData()
    }
    private func dirty(_ id: String) -> Bool { drafts[id] != baseline[id] }
    private func capture() {
        guard !loading, let id = selectedID, var draft = drafts[id] else { return }
        draft.host.name = nameField.stringValue
        draft.host.description = descriptionField.stringValue
        draft.host.target = targetField.stringValue
        draft.host.user = userField.stringValue.isEmpty ? nil : userField.stringValue
        draft.portText = portField.stringValue
        draft.host.port = Int(portField.stringValue)
        draft.host.identityFile = keyField.stringValue.isEmpty ? nil : keyField.stringValue
        draft.host.autoConnect = autoConnect.state == .on
        draft.host.exitOnForwardFailure = exitOnFailure.state == .on
        draft.host.retryOnConnectionRefused = retryRefused.state == .on
        drafts[id] = draft
    }
    private func finishEditing() { window.makeFirstResponder(nil); capture() }
    private func select(_ id: String) {
        guard let draft = drafts[id] else { return }
        loading = true; selectedID = id
        let host = draft.host
        nameField.stringValue = host.name; targetField.stringValue = host.target
        descriptionField.stringValue = host.description
        userField.stringValue = host.user ?? ""; portField.stringValue = draft.portText
        keyField.stringValue = host.identityFile ?? ""
        autoConnect.state = host.autoConnect ? .on : .off
        exitOnFailure.state = host.exitOnForwardFailure ? .on : .off
        retryRefused.state = host.retryOnConnectionRefused ? .on : .off
        if let index = order.firstIndex(of: id) { hostsTable.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false) }
        rulesTable.reloadData(); updateRuleButtons()
        loading = false; updateCommand(); updateStatus()
    }
    func controlTextDidChange(_ obj: Notification) { changed() }
    @objc private func changed() { capture(); hostsTable.reloadData(); updateCommand() }
    private func updateCommand() {
        guard let id = selectedID, let draft = drafts[id] else { command.string = ""; return }
        do {
            let host = try draft.validated()
            command.string = ([SSHCommand.executable] + SSHCommand.connect(host)).map(SSHCommand.shellQuote).joined(separator: " ")
        } catch { command.string = "입력을 확인하세요: " + error.localizedDescription }
    }
    private func updateStatus() {
        guard let id = selectedID else { return }
        if let runtime = manager.runtime(id) {
            connectButton.isEnabled = runtime.state != .connecting && runtime.state != .stopping
        } else { connectButton.isEnabled = true }
        authButton.isEnabled = authTimers[id] == nil
        updateConnectionStatus()
    }
    private func updateConnectionStatus() {
        guard let id = statusHostID else { return }
        if let host = manager.runtime(id)?.executionHost ?? drafts[id]?.host {
            statusHeading.stringValue = "현재 연결 상태 — " + host.name + "\nSSH 대상: " + host.target
        }
        status.stringValue = connectionSummary(manager.runtime(id))
        if let runtime = manager.runtime(id) {
            if runtime.temporary {
                status.stringValue += "\n임시 실행 설정 사용 중 — 다음 수동 연결과 자동 복구에도 적용됩니다."
                if let execution = runtime.executionHost, let draft = drafts[id], !execution.hasSameExecutionSettings(as: draft.host) {
                    status.stringValue += "\n편집값과 임시 실행 설정이 다릅니다. 변경 없이 재연결하면 임시 실행 설정을 재사용합니다. 편집값을 적용하려면 저장하세요."
                }
            }
            else if let execution = runtime.executionHost, let saved = manager.hosts.first(where: { $0.id == id }), !execution.hasSameExecutionSettings(as: saved),
                    runtime.state == .connected || runtime.state == .connecting {
                status.stringValue += "\n현재 실행 설정과 저장된 설정이 다릅니다. 저장한 값은 다음 연결부터 적용됩니다."
            }
            if !runtime.lastError.isEmpty { status.stringValue += "\n최근 오류: " + runtime.lastError }
            for error in runtime.recentErrors.suffix(5) {
                status.stringValue += "\n" + DateFormatter.localizedString(from: error.time, dateStyle: .short, timeStyle: .medium) + " · " + error.message
            }
        }
        runtimeTable.reloadData()
    }
    @objc private func showConnectionStatus() {
        guard window.attachedSheet == nil, statusSheet == nil, ruleEditor == nil else { return }
        finishEditing()
        guard let id = selectedID, drafts[id] != nil else { return }
        statusHostID = id
        let visible = window.screen?.visibleFrame ?? NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 900)
        let size = NSSize(width: min(760, visible.width - 40), height: min(480, visible.height - 70))
        let sheet = NSWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        sheet.title = "현재 연결 상태"; sheet.isReleasedWhenClosed = false
        sheet.contentMinSize = NSSize(width: min(600, size.width), height: min(320, size.height))
        let content = sheet.contentView!
        let bodyScroll = NSScrollView(); bodyScroll.hasVerticalScroller = true; bodyScroll.autohidesScrollers = true
        let document = FormDocumentView(); document.translatesAutoresizingMaskIntoConstraints = false; bodyScroll.documentView = document
        let body = NSStackView(); body.orientation = .vertical; body.alignment = .leading; body.spacing = 12
        body.translatesAutoresizingMaskIntoConstraints = false; document.addSubview(body)
        statusHeading.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        let explanation = NSTextField(wrappingLabelWithString: "‘준비됨’은 포워딩 개설 성공을 뜻합니다. 목적지 서비스의 정상 동작을 보장하지 않습니다.")
        for view in [statusHeading, status, scroll(runtimeTable, height: 160), explanation] as [NSView] {
            body.addArrangedSubview(view)
            view.widthAnchor.constraint(equalTo: body.widthAnchor).isActive = true
        }
        let close = button("닫기", #selector(closeConnectionStatus)); close.keyEquivalent = "\u{1b}"
        bodyScroll.translatesAutoresizingMaskIntoConstraints = false; close.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(bodyScroll); content.addSubview(close)
        NSLayoutConstraint.activate([
            bodyScroll.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16),
            bodyScroll.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            bodyScroll.topAnchor.constraint(equalTo: content.topAnchor, constant: 16),
            bodyScroll.bottomAnchor.constraint(equalTo: close.topAnchor, constant: -16),
            close.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            close.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16),
            document.leadingAnchor.constraint(equalTo: bodyScroll.contentView.leadingAnchor),
            document.topAnchor.constraint(equalTo: bodyScroll.contentView.topAnchor),
            document.widthAnchor.constraint(equalTo: bodyScroll.contentView.widthAnchor),
            body.leadingAnchor.constraint(equalTo: document.leadingAnchor),
            body.trailingAnchor.constraint(equalTo: document.trailingAnchor, constant: -8),
            body.topAnchor.constraint(equalTo: document.topAnchor),
            body.bottomAnchor.constraint(equalTo: document.bottomAnchor, constant: -12)])
        statusSheet = sheet; updateConnectionStatus()
        window.beginSheet(sheet) { [weak self, weak sheet] _ in
            guard let self, self.statusSheet === sheet else { return }
            sheet?.orderOut(nil); self.statusSheet = nil; self.statusHostID = nil
        }
    }
    @objc private func closeConnectionStatus() {
        guard let sheet = statusSheet else { return }
        window.endSheet(sheet)
        sheet.orderOut(nil); statusSheet = nil; statusHostID = nil
    }
    func prepareForTermination() { ruleEditor?.cancel(); closeConnectionStatus() }
    private func addDraft() {
        let host = Host(); order.append(host.id); drafts[host.id] = HostDraft(host)
    }
    @objc private func addHost() { finishEditing(); addDraft(); hostsTable.reloadData(); select(order.last!) }
    @objc private func deleteHost() {
        finishEditing()
        guard let id = selectedID, let draft = drafts[id] else { return }
        let alert = NSAlert(); alert.messageText = "\(draft.host.name)을 삭제할까요?"
        alert.informativeText = "이 호스트의 연결과 예약을 취소하고 저장한 설정도 삭제합니다."
        alert.addButton(withTitle: "삭제"); alert.addButton(withTitle: "취소")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        do {
            if manager.hosts.contains(where: { $0.id == id }) || baseline[id] != nil { try manager.delete(id) }
            drafts[id] = nil; baseline[id] = nil; order.removeAll { $0 == id }
            if order.isEmpty { addDraft() }
            hostsTable.reloadData(); select(order[0])
        } catch { showError(error.localizedDescription) }
    }
    private func saveSelected() throws -> Host {
        finishEditing()
        guard let id = selectedID, let draft = drafts[id] else { throw AppError("호스트를 선택하세요.") }
        let host = try draft.validated()
        try store.save(host)
        manager.saved(host)
        baseline[id] = HostDraft(host); drafts[id] = HostDraft(host)
        hostsTable.reloadData()
        updateStatus(); return host
    }
    @objc private func saveClicked() { do { _ = try saveSelected() } catch { showError(error.localizedDescription) } }
    @objc private func connectSelected() {
        finishEditing()
        guard let id = selectedID, let draft = drafts[id] else { return }
        do {
            // Validate before prompting/saving or stopping the currently running connection.
            var host = try draft.validated()
            var temporary = false
            if dirty(id) {
                let alert = NSAlert(); alert.messageText = "편집한 설정으로 연결·재연결합니다."
                alert.informativeText = "저장 없이 연결한 값은 이번 앱 실행 동안 유지됩니다."
                alert.addButton(withTitle: "저장 후 재연결"); alert.addButton(withTitle: "저장 없이 재연결"); alert.addButton(withTitle: "취소")
                switch alert.runModal() {
                case .alertFirstButtonReturn: host = try saveSelected()
                case .alertSecondButtonReturn: temporary = true
                default: return
                }
                try manager.connect(host, temporary: temporary)
            } else {
                // The manager preserves any already applied temporary execution configuration.
                try manager.connect(id)
            }
            updateStatus()
        } catch { showError(error.localizedDescription) }
    }
    @objc private func chooseKey() {
        let panel = NSOpenPanel(); panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { keyField.stringValue = url.path; changed() }
    }
    @objc private func addRule() {
        showRuleEditor(editing: nil)
    }
    @objc private func editRule() {
        guard let id = selectedID, let draft = drafts[id], draft.host.forwards.indices.contains(rulesTable.selectedRow) else { return }
        showRuleEditor(editing: draft.host.forwards[rulesTable.selectedRow])
    }
    private func showRuleEditor(editing rule: Forward?) {
        guard window.attachedSheet == nil, ruleEditor == nil, statusSheet == nil else { return }
        finishEditing()
        guard let id = selectedID, let draft = drafts[id] else { return }
        let editor = RuleEditor(parent: window, rules: draft.host.forwards, editing: rule) { [weak self] rules in
            guard let self else { return }
            self.ruleEditor = nil
            if let rules, var draft = self.drafts[id] {
                draft.host.forwards = rules
                draft.rulePorts = Dictionary(uniqueKeysWithValues: rules.map {
                    ($0.id, ["localPort": String($0.localPort), "remotePort": String($0.remotePort)])
                })
                self.drafts[id] = draft; self.rulesTable.reloadData()
                let row = rule.flatMap { edited in rules.firstIndex(where: { $0.id == edited.id }) } ?? (rules.count - 1)
                if row >= 0 { self.rulesTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false) }
                self.hostsTable.reloadData(); self.updateCommand()
            }
            self.updateRuleButtons()
        }
        ruleEditor = editor
        window.beginSheet(editor.window, completionHandler: nil)
    }
    private func updateRuleButtons() {
        let valid = selectedID.flatMap { drafts[$0]?.host.forwards.indices.contains(rulesTable.selectedRow) } ?? false
        editRuleButton?.isEnabled = valid; deleteRuleButton?.isEnabled = valid
    }
    @objc private func deleteRule() {
        let row = rulesTable.selectedRow; finishEditing()
        guard let id = selectedID, let draft = drafts[id], draft.host.forwards.indices.contains(row) else { return }
        drafts[id]?.rulePorts[draft.host.forwards[row].id] = nil
        drafts[id]?.host.forwards.remove(at: row); rulesTable.reloadData(); updateRuleButtons(); changed()
    }
    @objc private func okay() {
        finishEditing()
        if order.contains(where: dirty) {
            let alert = NSAlert(); alert.messageText = "저장하지 않은 편집 내용이 있습니다."
            alert.informativeText = "OK는 자동 저장하지 않습니다. 이미 저장하거나 연결에 적용한 결과는 유지됩니다."
            alert.addButton(withTitle: "계속 편집"); alert.addButton(withTitle: "저장하지 않고 닫기")
            guard alert.runModal() == .alertSecondButtonReturn else { return }
        }
        finish(accepted: true)
    }
    @objc private func cancel() { finish(accepted: false) }
    private func finish(accepted: Bool) {
        ruleEditor?.cancel()
        closeConnectionStatus()
        window.makeFirstResponder(nil)
        window.orderOut(nil); isOpen = false
        drafts = [:]; baseline = [:]; order = []; selectedID = nil
        let wasStartup = startup; startup = false; onClose?(accepted, wasStartup)
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { cancel(); return false }

    func numberOfRows(in tableView: NSTableView) -> Int {
        if tableView === hostsTable { return order.count }
        if tableView === runtimeTable {
            return statusHostID.flatMap { manager.runtime($0)?.forwards.count } ?? 0
        }
        guard let id = selectedID else { return 0 }
        return drafts[id]?.host.forwards.count ?? 0
    }
    func tableViewSelectionDidChange(_ notification: Notification) {
        if notification.object as? NSTableView === rulesTable { updateRuleButtons(); return }
        guard !loading, let table = notification.object as? NSTableView, table === hostsTable,
              order.indices.contains(table.selectedRow) else { return }
        let next = order[table.selectedRow]
        guard selectedID != next else { return }
        finishEditing(); select(next)
    }
    func tableView(_ tableView: NSTableView, objectValueFor tableColumn: NSTableColumn?, row: Int) -> Any? {
        let column = tableColumn?.identifier.rawValue ?? ""
        if tableView === hostsTable {
            guard order.indices.contains(row), let draft = drafts[order[row]] else { return nil }
            let id = order[row]
            let temporary = baseline[id] == nil && manager.runtime(id) != nil ? " · 임시" : ""
            return (dirty(id) ? "● " : "") + draft.host.name + temporary
        }
        if tableView === runtimeTable {
            guard let id = statusHostID, let forwards = manager.runtime(id)?.forwards, forwards.indices.contains(row) else { return nil }
            let forward = forwards[row]
            switch column {
            case "description": return forward.description
            case "source": return forward.source
            case "state": return forwardStateTitle(forward.state)
            case "detail": return forward.detail
            default: return nil
            }
        }
        guard let id = selectedID, let draft = drafts[id], draft.host.forwards.indices.contains(row) else { return nil }
        let rule = draft.host.forwards[row]
        switch column {
        case "enabled": return rule.enabled ? NSControl.StateValue.on.rawValue : NSControl.StateValue.off.rawValue
        case "localPort", "remotePort": return draft.rulePorts[rule.id]?[column] ?? ""
        case "remoteHost": return rule.remoteHost
        case "description": return rule.description
        default: return nil
        }
    }
    func tableView(_ tableView: NSTableView, setObjectValue object: Any?, for tableColumn: NSTableColumn?, row: Int) {
        guard tableView === rulesTable, let id = selectedID, var draft = drafts[id], draft.host.forwards.indices.contains(row) else { return }
        switch tableColumn?.identifier.rawValue {
        case "enabled": draft.host.forwards[row].enabled = (object as? NSNumber)?.intValue == NSControl.StateValue.on.rawValue
        default: break
        }
        drafts[id] = draft; hostsTable.reloadData(); updateCommand()
    }

    @objc private func setupAuthentication() {
        finishEditing()
        guard let id = selectedID, authTimers[id] == nil else { return }
        let alert = NSAlert(); alert.messageText = "선택한 호스트의 인증을 설정합니다."
        alert.informativeText = "선택한 호스트를 저장하고 외부 터미널에서 서버 지문과 비밀번호를 확인합니다. 앱과 다른 호스트의 연결은 계속 유지됩니다. 완료 후 연결은 직접 요청하세요."
        alert.addButton(withTitle: "저장 후 터미널 열기"); alert.addButton(withTitle: "취소")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        do {
            let host = try saveSelected()
            try launchAuthentication(host)
        } catch { showError(error.localizedDescription) }
    }
    private func launchAuthentication(_ host: Host) throws {
        guard let executable = Bundle.main.executableURL else { throw AppError("앱 실행 파일을 찾을 수 없습니다.") }
        let token = UUID().uuidString
        let script = store.directory.appendingPathComponent("authentication-\(token).command")
        let marker = store.directory.appendingPathComponent("authentication-\(token).result")
        let quote = SSHCommand.shellQuote
        let body = "#!/bin/sh\numask 077\nrm -f -- \"$0\"\nresult_file=" + quote(marker.path) + "\ntrap 'exit 130' HUP INT TERM\ntrap 'result=$?; printf \"%s\\n\" \"$result\" > \"$result_file\"' EXIT\n" + quote(executable.path) + " --setup-auth " + quote(host.id) + "\nexit $?\n"
        try Data(body.utf8).write(to: script, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: script.path)
        let timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] timer in
            guard let self else { timer.invalidate(); return }
            guard let result = try? String(contentsOf: marker, encoding: .utf8) else { return }
            timer.invalidate(); self.authTimers[host.id] = nil
            try? FileManager.default.removeItem(at: marker)
            self.authenticationFinished(host, status: result.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        authTimers[host.id] = timer; updateStatus()
        NSWorkspace.shared.open([script], withApplicationAt: URL(fileURLWithPath: "/System/Applications/Utilities/Terminal.app"), configuration: NSWorkspace.OpenConfiguration()) { [weak self] _, error in
            guard let error else { return }
            DispatchQueue.main.async {
                timer.invalidate(); self?.authTimers[host.id] = nil
                try? FileManager.default.removeItem(at: script)
                self?.showError(error.localizedDescription); self?.updateStatus()
            }
        }
    }
    private func authenticationFinished(_ original: Host, status: String) {
        if isOpen { finishEditing() }
        do {
            let updated = try store.host(original.id)
            manager.saved(updated)
            if var draft = drafts[original.id] {
                if draft.host.identityFile == original.identityFile { draft.host.identityFile = updated.identityFile }
                drafts[original.id] = draft; baseline[original.id] = HostDraft(updated)
                if selectedID == original.id { select(original.id) }
                hostsTable.reloadData()
            }
            if status != "0" { showError("인증 설정을 완료하지 못했습니다. 터미널에서 결과를 확인하세요.") }
        } catch { showError(error.localizedDescription) }
    }
    private func showError(_ message: String) {
        let alert = NSAlert(); alert.messageText = "LocalForwarder"; alert.informativeText = message; alert.alertStyle = .warning
        if window.isVisible { alert.beginSheetModal(for: ruleEditor?.window ?? statusSheet ?? window, completionHandler: nil) }
        else { alert.runModal() }
    }

    private func button(_ title: String, _ action: Selector) -> NSButton { NSButton(title: title, target: self, action: action) }
    private func scroll(_ view: NSView, height: CGFloat) -> NSScrollView {
        let result = NSScrollView(); result.hasVerticalScroller = true; result.hasHorizontalScroller = view is NSTableView
        result.borderType = .bezelBorder; result.documentView = view
        result.heightAnchor.constraint(equalToConstant: height).isActive = true
        if let text = view as? NSTextView {
            text.isEditable = false; text.isSelectable = true; text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
            text.isVerticallyResizable = true; text.isHorizontallyResizable = false
            text.textContainer?.widthTracksTextView = true; text.autoresizingMask = [.width]
        }
        return result
    }
    private func makeWindow(availableFrame: NSRect?) {
        let visible = availableFrame ?? NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 900)
        let size = NSSize(width: min(900, visible.width - 40), height: min(650, visible.height - 70))
        window = NSWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "LocalForwarder 설정"; window.delegate = self; window.isReleasedWhenClosed = false
        window.contentMinSize = NSSize(width: min(800, size.width), height: min(430, size.height)); window.center()
        let content = window.contentView!
        let sidebar = NSStackView(); sidebar.orientation = .vertical; sidebar.alignment = .leading; sidebar.spacing = 8
        sidebar.detachesHiddenViews = false; sidebarStack = sidebar
        let hostActions = NSStackView(views: [button("새로 만들기", #selector(addHost)), button("호스트 삭제…", #selector(deleteHost))])
        hostActions.spacing = 8
        hostActions.detachesHiddenViews = false
        for view in hostActions.arrangedSubviews { hostActions.setVisibilityPriority(.mustHold, for: view) }
        sidebar.addArrangedSubview(hostActions)
        let hostColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("host")); hostColumn.title = "호스트 · ● 미저장"; hostColumn.width = 185
        hostsTable.addTableColumn(hostColumn); hostsTable.dataSource = self; hostsTable.delegate = self; hostsTable.rowHeight = 28
        hostsTable.allowsEmptySelection = false
        let hostScroll = NSScrollView(); hostScroll.documentView = hostsTable; hostScroll.hasVerticalScroller = true; hostScroll.borderType = .bezelBorder
        sidebar.addArrangedSubview(hostScroll)
        hostScroll.widthAnchor.constraint(equalTo: sidebar.widthAnchor).isActive = true
        let formScroll = NSScrollView(); formScroll.hasVerticalScroller = true
        // Reserve legacy scrollbar space while rows reflow. Auto-hiding can resize
        // the document independently of its constraints when returning to a wide
        // layout, leaving its width smaller than both the clip view and the form.
        formScroll.autohidesScrollers = false
        formScrollView = formScroll
        let document = FormDocumentView(); document.translatesAutoresizingMaskIntoConstraints = false; formScroll.documentView = document
        formDocumentView = document
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 10; form.translatesAutoresizingMaskIntoConstraints = false
        form.detachesHiddenViews = false; formStack = form
        document.addSubview(form)
        let sidebarWidth: CGFloat = 205
        let paneSpacing: CGFloat = 16
        let root = NSStackView(views: [sidebar, formScroll]); root.orientation = .horizontal; root.alignment = .top; root.spacing = paneSpacing
        root.detachesHiddenViews = false; rootStack = root
        root.setVisibilityPriority(.mustHold, for: sidebar); root.setVisibilityPriority(.mustHold, for: formScroll)
        root.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(root)
        let okay = button("OK", #selector(okay)); okay.keyEquivalent = "\r"
        let cancel = button("Cancel", #selector(cancel)); cancel.keyEquivalent = "\u{1b}"
        let footer = NSStackView(views: [cancel, okay]); footer.spacing = 8
        footer.detachesHiddenViews = false; footerStack = footer
        footer.setVisibilityPriority(.mustHold, for: cancel); footer.setVisibilityPriority(.mustHold, for: okay)
        footer.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(footer)
        NSLayoutConstraint.activate([
            root.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16), root.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            root.topAnchor.constraint(equalTo: content.topAnchor, constant: 16), root.bottomAnchor.constraint(equalTo: footer.topAnchor, constant: -16),
            footer.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            footer.leadingAnchor.constraint(greaterThanOrEqualTo: content.leadingAnchor, constant: 16),
            footer.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16),
            document.leadingAnchor.constraint(equalTo: formScroll.contentView.leadingAnchor), document.topAnchor.constraint(equalTo: formScroll.contentView.topAnchor),
            document.widthAnchor.constraint(equalTo: formScroll.contentView.widthAnchor),
            form.leadingAnchor.constraint(equalTo: document.leadingAnchor), form.trailingAnchor.constraint(equalTo: document.trailingAnchor, constant: -8),
            form.topAnchor.constraint(equalTo: document.topAnchor), form.bottomAnchor.constraint(equalTo: document.bottomAnchor, constant: -12)])
        widePaneConstraints = [
            sidebar.widthAnchor.constraint(equalToConstant: sidebarWidth),
            sidebar.heightAnchor.constraint(equalTo: root.heightAnchor),
            formScroll.heightAnchor.constraint(equalTo: root.heightAnchor),
            formScroll.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -(sidebarWidth + paneSpacing))]
        narrowPaneConstraints = [
            sidebar.widthAnchor.constraint(equalTo: root.widthAnchor),
            hostScroll.heightAnchor.constraint(equalToConstant: 72),
            formScroll.widthAnchor.constraint(equalTo: root.widthAnchor)]
        NSLayoutConstraint.activate(widePaneConstraints)
        for view in sidebar.arrangedSubviews { sidebar.setVisibilityPriority(.mustHold, for: view) }
        func add(_ view: NSView, fullWidth: Bool = true) {
            form.addArrangedSubview(view)
            form.setVisibilityPriority(.mustHold, for: view)
            if fullWidth { view.widthAnchor.constraint(equalTo: form.widthAnchor).isActive = true }
        }
        func adaptive(_ views: [NSView], spacing: CGFloat = 10, constraints: [NSLayoutConstraint] = []) -> AdaptiveStack {
            let row = AdaptiveStack(views: views, spacing: spacing, horizontalConstraints: constraints)
            adaptiveRows.append(row); return row
        }
        func fieldRow(_ label: String, _ field: NSTextField, trailing: NSView? = nil) -> AdaptiveStack {
            let title = NSTextField(labelWithString: label)
            field.delegate = self; field.setContentHuggingPriority(.defaultLow, for: .horizontal)
            field.widthAnchor.constraint(greaterThanOrEqualToConstant: 80).isActive = true
            return adaptive([title, field] + (trailing.map { [$0] } ?? []),
                            constraints: [title.widthAnchor.constraint(equalToConstant: 112)])
        }
        add(fieldRow("이름", nameField))
        add(fieldRow("설명 (선택)", descriptionField))
        add(fieldRow("SSH 별칭 / 주소", targetField))
        let credentials = adaptive([fieldRow("사용자명 (선택)", userField), fieldRow("SSH 포트 (선택)", portField)], spacing: 12)
        add(credentials)
        let keyRow = fieldRow("개인키 (선택)", keyField, trailing: button("개인키 파일 선택…", #selector(chooseKey)))
        add(keyRow)
        targetField.placeholderString = "예: my-server 또는 server.example.com"; keyField.placeholderString = "비워두면 기존 SSH 설정 사용"
        for control in [autoConnect, exitOnFailure, retryRefused] { control.target = self; control.action = #selector(changed) }
        add(adaptive([autoConnect, exitOnFailure, retryRefused], spacing: 12))
        add(NSTextField(wrappingLabelWithString: "포워딩 편집 — 내 컴퓨터(127.0.0.1)에서만 접속"))
        for (id, title, width) in [("enabled", "사용", 48.0), ("localPort", "로컬 포트", 90.0), ("remoteHost", "목적지 호스트", 270.0), ("remotePort", "목적지 포트", 100.0), ("description", "설명", 220.0)] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); column.title = title; column.width = width; column.isEditable = id == "enabled"
            if id == "enabled" { let cell = NSButtonCell(); cell.setButtonType(.switch); cell.title = ""; column.dataCell = cell }
            rulesTable.addTableColumn(column)
        }
        rulesTable.dataSource = self; rulesTable.delegate = self; rulesTable.rowHeight = 24
        rulesTable.target = self; rulesTable.doubleAction = #selector(editRule)
        add(scroll(rulesTable, height: 120))
        editRuleButton = button("규칙 편집…", #selector(editRule)); deleteRuleButton = button("규칙 삭제", #selector(deleteRule))
        add(adaptive([button("새 규칙…", #selector(addRule)), editRuleButton, deleteRuleButton]))
        add(NSTextField(wrappingLabelWithString: "편집한 값의 SSH 명령 (읽기 전용)")); add(scroll(command, height: 64))
        for (id, title, width) in [("description", "포워딩", 260.0), ("source", "출처", 90.0), ("state", "상태", 90.0), ("detail", "상세", 280.0)] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); column.title = title; column.width = width; column.isEditable = false; runtimeTable.addTableColumn(column)
        }
        runtimeTable.dataSource = self; runtimeTable.delegate = self; runtimeTable.rowHeight = 24
        connectButton = button("연결·재연결", #selector(connectSelected)); authButton = button("인증 설정…", #selector(setupAuthentication))
        add(adaptive([button("선택 호스트 저장", #selector(saveClicked)), connectButton, authButton, button("연결 상태…", #selector(showConnectionStatus))]))
        updateResponsiveLayout()
    }
    func windowDidResize(_ notification: Notification) { updateResponsiveLayout() }
    private func updateResponsiveLayout() {
        guard let root = rootStack, let content = window.contentView else { return }
        let narrow = content.bounds.width < 720
        if narrow != narrowPanes {
            NSLayoutConstraint.deactivate(narrow ? widePaneConstraints : narrowPaneConstraints)
            root.orientation = narrow ? .vertical : .horizontal; root.alignment = narrow ? .leading : .top
            NSLayoutConstraint.activate(narrow ? narrowPaneConstraints : widePaneConstraints)
            narrowPanes = narrow
        }
        let formWidth = content.bounds.width - 32 - (narrow ? 0 : 221) - 24
        for row in adaptiveRows { row.reflow(compact: formWidth < 560) }
    }
}

extension SettingsController {
    /// Exercise production windows and constraints, including resize back to the normal sidebar.
    static func checkNativeLayout(store: ConfigurationStore, manager: ConnectionManager) throws {
        for (area, sizes) in [
            (NSSize(width: 940, height: 720), [NSSize(width: 900, height: 650), NSSize(width: 800, height: 430), NSSize(width: 900, height: 650)]),
            (NSSize(width: 520, height: 360), [NSSize(width: 480, height: 290)])
        ] {
            let controller = SettingsController(store: store, manager: manager, availableFrame: NSRect(origin: .zero, size: area))
            controller.open()
            defer { controller.finish(accepted: false) }
            let expectedForm = NativeLayoutChecks.leaves(controller.formStack)
            let expectedSidebar = NativeLayoutChecks.leaves(controller.sidebarStack)
            let expectedFooter = NativeLayoutChecks.leaves(controller.footerStack)
            try NativeLayoutChecks.require(expectedForm.count == 27 && expectedSidebar.count == 3 && expectedFooter.count == 2,
                                           "unexpected control inventory; update the coverage when adding controls")
            for size in sizes {
                controller.window.setContentSize(size)
                controller.updateResponsiveLayout()
                try NativeLayoutChecks.settle(controller.window)
                let content = controller.window.contentView!
                let context = "settings \(Int(size.width))x\(Int(size.height))"
                try NativeLayoutChecks.require(abs(content.bounds.width - size.width) < 1 && abs(content.bounds.height - size.height) < 1,
                                               context + " expanded beyond requested bounds: \(content.bounds)")
                try NativeLayoutChecks.require(controller.rootStack.orientation == (size.width < 720 ? .vertical : .horizontal), context + " wrong pane orientation")
                try NativeLayoutChecks.check(expectedSidebar + expectedFooter + [controller.formScrollView!], in: content, context: context)
                try NativeLayoutChecks.require(controller.formScrollView.contentView.bounds.height >= 60, context + " form viewport unusable")
                writeLine("LF_UI_GEOMETRY \(context): clip \(controller.formScrollView.contentView.bounds), document \(controller.formDocumentView.bounds), form \(controller.formStack.frame)")
                try NativeLayoutChecks.require(abs(controller.formDocumentView.bounds.width - controller.formScrollView.contentView.bounds.width) < 1,
                                               context + " document width differs from the scroll viewport")
                try NativeLayoutChecks.check(expectedForm, in: controller.formDocumentView, scroll: controller.formScrollView, context: context)
                let before = controller.drafts
                let selectedID = controller.selectedID!
                let originalRules = before[selectedID]!.host.forwards
                let originalFile = try Data(contentsOf: store.file)
                let columns = controller.rulesTable.tableColumns
                try NativeLayoutChecks.require(columns.map { $0.identifier.rawValue } ==
                                               ["enabled", "localPort", "remoteHost", "remotePort", "description"],
                                               context + " missing or reordered rule columns")
                let descriptionColumn = columns.last!
                try NativeLayoutChecks.require(descriptionColumn.title == "설명" && !descriptionColumn.isEditable,
                                               context + " description column must use the rule editor")
                try NativeLayoutChecks.require(controller.rulesTable.enclosingScrollView?.hasHorizontalScroller == true,
                                               context + " rule columns cannot scroll horizontally")
                func checkDescriptionCell(_ expected: String, row: Int = 0) throws {
                    let displayed = controller.tableView(controller.rulesTable, objectValueFor: descriptionColumn, row: row) as? String
                    try NativeLayoutChecks.require(displayed == expected, context + " incorrect description table cell")
                }
                try checkDescriptionCell(originalRules[0].description)
                func openRule(_ editing: Forward?) throws -> (RuleEditor, [NSTextField]) {
                    controller.showRuleEditor(editing: editing)
                    guard let editor = controller.ruleEditor else { throw AppError(context + " missing rule sheet") }
                    try NativeLayoutChecks.settle(editor.window)
                    try NativeLayoutChecks.require(controller.window.attachedSheet === editor.window, context + " rule editor is not modal")
                    try NativeLayoutChecks.require(editor.window.frame.width <= area.width && editor.window.frame.height <= area.height,
                                                   context + " rule sheet exceeds available work area")
                    let modalContent = editor.window.contentView!
                    let controls = modalContent.subviews.flatMap { NativeLayoutChecks.leaves($0) }
                    try NativeLayoutChecks.require(controls.count == 11, context + " missing rule controls")
                    try NativeLayoutChecks.check(controls, in: modalContent, context: context + " rule sheet")
                    let fields = controls.compactMap { $0 as? NSTextField }.filter { $0.isEditable }
                    let labels = controls.compactMap { $0 as? NSTextField }.filter { !$0.isEditable }.map(\.stringValue)
                    try NativeLayoutChecks.require(fields.count == 4 && labels == ["설명 (선택)", "로컬 포트", "목적지 호스트", "목적지 포트"],
                                                   context + " wrong rule field order")
                    try NativeLayoutChecks.require(fields[0].stringValue == (editing?.description ?? "") &&
                                                   fields[0].cell?.usesSingleLineMode == true,
                                                   context + " incorrect initial or multiline rule description")
                    return (editor, fields)
                }
                func click(_ title: String, editor: RuleEditor, usingEscape: Bool = false) throws {
                    let buttons = editor.window.contentView!.subviews.flatMap { NativeLayoutChecks.leaves($0) }.compactMap { $0 as? NSButton }
                    guard let button = buttons.first(where: { $0.title == title }) else { throw AppError(context + " missing " + title) }
                    // Escape is a cancellation requirement. Confirmation is checked through
                    // its actual button action and resulting draft, not a framework key string.
                    if title == "취소" {
                        try NativeLayoutChecks.require(button.keyEquivalent == "\u{1b}",
                                                       context + " missing rule Escape equivalent")
                    }
                    if usingEscape {
                        guard let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                                                           windowNumber: editor.window.windowNumber, context: nil,
                                                           characters: "\u{1b}", charactersIgnoringModifiers: "\u{1b}",
                                                           isARepeat: false, keyCode: 53) else { throw AppError(context + " missing Escape event") }
                        try NativeLayoutChecks.require(button.performKeyEquivalent(with: event), context + " Escape did not activate Cancel")
                    } else { button.performClick(nil) }
                    try NativeLayoutChecks.settle(controller.window)
                    try NativeLayoutChecks.require(controller.ruleEditor == nil && controller.window.attachedSheet == nil,
                                                   context + " rule sheet remained open after " + title)
                    try NativeLayoutChecks.require(try Data(contentsOf: store.file) == originalFile,
                                                   context + " rule modal wrote the configuration file")
                }
                let cancellations: [(String, Forward?)] = [("취소", nil), ("Escape", originalRules.first), ("닫기", originalRules.first)]
                for (action, editing) in cancellations {
                    let (editor, fields) = try openRule(editing)
                    fields[0].stringValue = "취소할 설명 변경"
                    if action != "닫기" { try click("취소", editor: editor, usingEscape: action == "Escape") }
                    else {
                        let closeButton = editor.window.standardWindowButton(.closeButton)
                        if let closeButton, closeButton.isEnabled && !closeButton.isHiddenOrHasHiddenAncestor {
                            writeLine("LF_UI_RULE_CLOSE " + context + " close-button simulation")
                            editor.window.performClose(nil)
                        } else {
                            // AppKit sheets can omit/disable the close affordance; performClose only
                            // simulates that button. Exercise the production delegate contract in
                            // that case, without claiming a user close-button click was available.
                            writeLine("LF_UI_RULE_CLOSE " + context + " no close affordance; checking windowShouldClose delegate")
                            try NativeLayoutChecks.require(editor.window.delegate === editor,
                                                           context + " close delegate is not the rule editor")
                            let result = editor.window.delegate?.windowShouldClose?(editor.window)
                            try NativeLayoutChecks.require(result == false,
                                                           context + " close delegate did not handle sheet cancellation")
                        }
                        try NativeLayoutChecks.settle(controller.window)
                    }
                    try NativeLayoutChecks.require(controller.ruleEditor == nil && controller.window.attachedSheet == nil,
                                                   context + " " + action + " left the rule modal open")
                    try NativeLayoutChecks.require(controller.drafts == before,
                                                   context + " " + action + " changed the host draft")
                    try NativeLayoutChecks.require(try Data(contentsOf: store.file) == originalFile,
                                                   context + " " + action + " wrote the configuration file")
                    writeLine("LF_UI_RULE_CANCEL " + context + " " + action + " passed")
                }
                for description in ["수정한 한글 웹 설명", ""] {
                    let (editor, fields) = try openRule(controller.drafts[selectedID]!.host.forwards[0])
                    fields[0].stringValue = description
                    try click("확인", editor: editor)
                    var expected = before
                    expected[selectedID]!.host.forwards[0].description = description
                    try NativeLayoutChecks.require(controller.drafts == expected,
                                                   context + " description confirmation changed other draft fields, IDs, or order")
                    try checkDescriptionCell(description)
                }
                let beforeAddition = controller.drafts[selectedID]!.host.forwards
                let (editor, fields) = try openRule(nil)
                fields[0].stringValue = "새 규칙 설명"; fields[1].stringValue = "18081"
                try click("확인", editor: editor)
                let added = controller.drafts[selectedID]!.host.forwards
                try NativeLayoutChecks.require(added.count == beforeAddition.count + 1 && Array(added.dropLast()) == beforeAddition &&
                                               added.last?.description == "새 규칙 설명" && added.last?.localPort == 18081 &&
                                               Set(added.map(\.id)).count == added.count,
                                               context + " new rule lost description or changed previous rules")
                try checkDescriptionCell("새 규칙 설명", row: added.count - 1)
                // Reset only the isolated test fixture before testing the next window size.
                controller.drafts = before
                controller.rulesTable.reloadData(); controller.hostsTable.reloadData(); controller.updateCommand()
                writeLine("LF_UI_LAYOUT " + context + " passed")
            }
        }
    }
}

func forwardStateTitle(_ state: ForwardState) -> String {
    switch state {
    case .preparing: return "준비 중"
    case .ready: return "준비됨"
    case .failed: return "개설 실패"
    case .disconnected: return "연결 끊김"
    case .unknown: return "확인 불가"
    }
}
func connectionSummary(_ runtime: HostRuntime?) -> String {
    guard let runtime else { return "SSH 연결 안 됨" }
    let label: String
    switch runtime.state {
    case .connected: label = "SSH 연결됨"
    case .connecting: label = "SSH 연결 중…"
    case .stopping: label = "SSH 정리 중…"
    case .disconnected: label = "SSH 연결 안 됨"
    }
    let forwards = runtime.forwards
    let ready = forwards.filter { $0.state == .ready }.count
    let failed = forwards.filter { $0.state == .failed }.count
    let unknown = forwards.filter { $0.state == .unknown }.count
    let preparing = forwards.filter { $0.state == .preparing }.count
    let disconnected = forwards.filter { $0.state == .disconnected }.count
    var text = "\(label) · 포워딩 준비 \(ready) / 실패 \(failed)"
    if unknown > 0 { text += " / 확인 불가 \(unknown)" }
    if preparing > 0 { text += " / 준비 중 \(preparing)" }
    if disconnected > 0 { text += " / 연결 끊김 \(disconnected)" }
    return text
}
