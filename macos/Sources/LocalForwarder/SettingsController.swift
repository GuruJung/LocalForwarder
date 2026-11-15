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
    private let hostsTable = NSTableView()
    private let rulesTable = NSTableView()
    private let runtimeTable = NSTableView()
    private let nameField = NSTextField()
    private let targetField = NSTextField()
    private let userField = NSTextField()
    private let portField = NSTextField()
    private let keyField = NSTextField()
    private let autoConnect = NSButton(checkboxWithTitle: "자동 연결", target: nil, action: nil)
    private let exitOnFailure = NSButton(checkboxWithTitle: "ExitOnForwardFailure", target: nil, action: nil)
    private let retryRefused = NSButton(checkboxWithTitle: "연결 거부 시 한 번 재시도", target: nil, action: nil)
    private let command = NSTextView()
    private let status = NSTextField(wrappingLabelWithString: "")
    private let validation = NSTextField(wrappingLabelWithString: "")
    private var connectButton: NSButton!
    private var authButton: NSButton!
    private var order: [String] = []
    private var drafts: [String: HostDraft] = [:]
    private var baseline: [String: HostDraft] = [:]
    private var selectedID: String?
    private var loading = false
    private var authTimers: [String: Timer] = [:]

    init(store: ConfigurationStore, manager: ConnectionManager) {
        self.store = store; self.manager = manager
        super.init(); makeWindow()
    }
    func open(select id: String? = nil, startup: Bool = false) {
        if isOpen {
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
        if let loadError { validation.stringValue = loadError + "\n원본 파일은 보존되며 이 파일로 자동 연결하지 않습니다." }
        window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
    }
    func refresh() {
        guard isOpen else { return }
        runtimeTable.reloadData(); updateStatus(); hostsTable.reloadData()
    }
    private func dirty(_ id: String) -> Bool { drafts[id] != baseline[id] }
    private func capture() {
        guard !loading, let id = selectedID, var draft = drafts[id] else { return }
        draft.host.name = nameField.stringValue
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
        userField.stringValue = host.user ?? ""; portField.stringValue = draft.portText
        keyField.stringValue = host.identityFile ?? ""
        autoConnect.state = host.autoConnect ? .on : .off
        exitOnFailure.state = host.exitOnForwardFailure ? .on : .off
        retryRefused.state = host.retryOnConnectionRefused ? .on : .off
        if let index = order.firstIndex(of: id) { hostsTable.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false) }
        rulesTable.reloadData(); runtimeTable.reloadData(); validation.stringValue = ""
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
        status.stringValue = connectionSummary(manager.runtime(id))
        if let runtime = manager.runtime(id) {
            if runtime.temporary {
                status.stringValue += "\n임시 실행 설정 사용 중 — 다음 수동 연결과 자동 복구에도 적용됩니다."
                if let execution = runtime.executionHost, let draft = drafts[id], execution != draft.host {
                    status.stringValue += "\n편집값과 임시 실행 설정이 다릅니다. 변경 없이 재연결하면 임시 실행 설정을 재사용합니다. 편집값을 적용하려면 저장하세요."
                }
            }
            else if let execution = runtime.executionHost, let saved = manager.hosts.first(where: { $0.id == id }), execution != saved,
                    runtime.state == .connected || runtime.state == .connecting {
                status.stringValue += "\n현재 실행 설정과 저장된 설정이 다릅니다. 저장한 값은 다음 연결부터 적용됩니다."
            }
            if !runtime.lastError.isEmpty { status.stringValue += "\n최근 오류: " + runtime.lastError }
            for error in runtime.recentErrors.suffix(5) {
                status.stringValue += "\n" + DateFormatter.localizedString(from: error.time, dateStyle: .short, timeStyle: .medium) + " · " + error.message
            }
            connectButton.isEnabled = runtime.state != .connecting && runtime.state != .stopping
        } else { connectButton.isEnabled = true }
        authButton.isEnabled = authTimers[id] == nil
    }
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
        } catch { validation.stringValue = error.localizedDescription }
    }
    private func saveSelected() throws -> Host {
        finishEditing()
        guard let id = selectedID, let draft = drafts[id] else { throw AppError("호스트를 선택하세요.") }
        let host = try draft.validated()
        try store.save(host)
        manager.saved(host)
        baseline[id] = HostDraft(host); drafts[id] = HostDraft(host)
        hostsTable.reloadData(); validation.stringValue = "저장했습니다. 변경 사항은 다음 연결부터 적용됩니다."
        updateStatus(); return host
    }
    @objc private func saveClicked() { do { _ = try saveSelected() } catch { validation.stringValue = error.localizedDescription } }
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
            validation.stringValue = ""; updateStatus()
        } catch { validation.stringValue = error.localizedDescription }
    }
    @objc private func chooseKey() {
        let panel = NSOpenPanel(); panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { keyField.stringValue = url.path; changed() }
    }
    @objc private func addRule() {
        finishEditing()
        guard let id = selectedID else { return }
        let rule = Forward(); drafts[id]?.host.forwards.append(rule)
        drafts[id]?.rulePorts[rule.id] = ["localPort": String(rule.localPort), "remotePort": String(rule.remotePort)]
        rulesTable.reloadData(); changed()
    }
    @objc private func deleteRule() {
        let row = rulesTable.selectedRow; finishEditing()
        guard let id = selectedID, let draft = drafts[id], draft.host.forwards.indices.contains(row) else { return }
        drafts[id]?.rulePorts[draft.host.forwards[row].id] = nil
        drafts[id]?.host.forwards.remove(at: row); rulesTable.reloadData(); changed()
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
        window.makeFirstResponder(nil)
        window.orderOut(nil); isOpen = false
        drafts = [:]; baseline = [:]; order = []; selectedID = nil
        let wasStartup = startup; startup = false; onClose?(accepted, wasStartup)
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { cancel(); return false }

    func numberOfRows(in tableView: NSTableView) -> Int {
        if tableView === hostsTable { return order.count }
        guard let id = selectedID else { return 0 }
        if tableView === runtimeTable { return manager.runtime(id)?.forwards.count ?? 0 }
        return drafts[id]?.host.forwards.count ?? 0
    }
    func tableViewSelectionDidChange(_ notification: Notification) {
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
        guard let id = selectedID else { return nil }
        if tableView === runtimeTable {
            guard let forwards = manager.runtime(id)?.forwards, forwards.indices.contains(row) else { return nil }
            let forward = forwards[row]
            switch column {
            case "description": return forward.description
            case "source": return forward.source
            case "state": return forwardStateTitle(forward.state)
            case "detail": return forward.detail
            default: return nil
            }
        }
        guard let draft = drafts[id], draft.host.forwards.indices.contains(row) else { return nil }
        let rule = draft.host.forwards[row]
        switch column {
        case "enabled": return rule.enabled ? NSControl.StateValue.on.rawValue : NSControl.StateValue.off.rawValue
        case "localPort", "remotePort": return draft.rulePorts[rule.id]?[column] ?? ""
        case "remoteHost": return rule.remoteHost
        default: return nil
        }
    }
    func tableView(_ tableView: NSTableView, setObjectValue object: Any?, for tableColumn: NSTableColumn?, row: Int) {
        guard tableView === rulesTable, let id = selectedID, var draft = drafts[id], draft.host.forwards.indices.contains(row) else { return }
        let value = String(describing: object ?? "")
        switch tableColumn?.identifier.rawValue {
        case "enabled": draft.host.forwards[row].enabled = (object as? NSNumber)?.intValue == NSControl.StateValue.on.rawValue
        case "localPort": draft.host.forwards[row].localPort = Int(value) ?? 0; draft.rulePorts[draft.host.forwards[row].id]?["localPort"] = value
        case "remoteHost": draft.host.forwards[row].remoteHost = value
        case "remotePort": draft.host.forwards[row].remotePort = Int(value) ?? 0; draft.rulePorts[draft.host.forwards[row].id]?["remotePort"] = value
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
        } catch { validation.stringValue = error.localizedDescription }
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
        let alert = NSAlert(); alert.messageText = "LocalForwarder"; alert.informativeText = message; alert.alertStyle = .warning; alert.runModal()
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
    private func makeWindow() {
        let visible = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 900)
        let size = NSSize(width: min(1080, visible.width - 40), height: min(850, visible.height - 70))
        window = NSWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "LocalForwarder 설정"; window.delegate = self; window.isReleasedWhenClosed = false
        window.contentMinSize = NSSize(width: min(800, size.width), height: min(430, size.height)); window.center()
        let content = window.contentView!
        let sidebar = NSStackView(); sidebar.orientation = .vertical; sidebar.alignment = .leading; sidebar.spacing = 8
        sidebar.addArrangedSubview(button("새로 만들기", #selector(addHost)))
        let hostColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("host")); hostColumn.title = "호스트 · ● 미저장"; hostColumn.width = 185
        hostsTable.addTableColumn(hostColumn); hostsTable.dataSource = self; hostsTable.delegate = self; hostsTable.rowHeight = 28
        hostsTable.allowsEmptySelection = false
        let hostScroll = NSScrollView(); hostScroll.documentView = hostsTable; hostScroll.hasVerticalScroller = true; hostScroll.borderType = .bezelBorder
        sidebar.addArrangedSubview(hostScroll); sidebar.addArrangedSubview(button("호스트 삭제…", #selector(deleteHost)))
        hostScroll.widthAnchor.constraint(equalTo: sidebar.widthAnchor).isActive = true
        let formScroll = NSScrollView(); formScroll.hasVerticalScroller = true; formScroll.autohidesScrollers = true
        let document = FormDocumentView(); document.translatesAutoresizingMaskIntoConstraints = false; formScroll.documentView = document
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 10; form.translatesAutoresizingMaskIntoConstraints = false
        document.addSubview(form)
        let root = NSStackView(views: [sidebar, formScroll]); root.orientation = .horizontal; root.alignment = .top; root.spacing = 16
        root.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(root)
        NSLayoutConstraint.activate([
            root.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16), root.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            root.topAnchor.constraint(equalTo: content.topAnchor, constant: 16), root.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16),
            sidebar.widthAnchor.constraint(equalToConstant: 205), sidebar.heightAnchor.constraint(equalTo: root.heightAnchor),
            formScroll.heightAnchor.constraint(equalTo: root.heightAnchor),
            document.leadingAnchor.constraint(equalTo: formScroll.contentView.leadingAnchor), document.topAnchor.constraint(equalTo: formScroll.contentView.topAnchor),
            document.widthAnchor.constraint(equalTo: formScroll.contentView.widthAnchor),
            form.leadingAnchor.constraint(equalTo: document.leadingAnchor), form.trailingAnchor.constraint(equalTo: document.trailingAnchor, constant: -8),
            form.topAnchor.constraint(equalTo: document.topAnchor), form.bottomAnchor.constraint(equalTo: document.bottomAnchor, constant: -12)])
        func add(_ view: NSView, fullWidth: Bool = true) {
            form.addArrangedSubview(view)
            if fullWidth { view.widthAnchor.constraint(equalTo: form.widthAnchor).isActive = true }
        }
        for (label, field) in [("이름", nameField), ("SSH 별칭 / 주소", targetField), ("사용자명 (선택)", userField), ("SSH 포트 (선택)", portField), ("개인키 (선택)", keyField)] {
            let title = NSTextField(labelWithString: label); title.widthAnchor.constraint(equalToConstant: 112).isActive = true
            field.delegate = self; field.setContentHuggingPriority(.defaultLow, for: .horizontal)
            let row = NSStackView(views: [title, field]); row.spacing = 10; add(row)
        }
        targetField.placeholderString = "예: my-server 또는 server.example.com"; keyField.placeholderString = "비워두면 기존 SSH 설정 사용"
        add(button("개인키 파일 선택…", #selector(chooseKey)), fullWidth: false)
        for control in [autoConnect, exitOnFailure, retryRefused] { control.target = self; control.action = #selector(changed) }
        let options = NSStackView(views: [autoConnect, exitOnFailure, retryRefused]); options.spacing = 12; add(options, fullWidth: false)
        add(NSTextField(labelWithString: "포워딩 편집 — 내 컴퓨터(127.0.0.1)에서만 접속"))
        for (id, title, width) in [("enabled", "사용", 48.0), ("localPort", "로컬 포트", 90.0), ("remoteHost", "목적지 호스트", 270.0), ("remotePort", "목적지 포트", 100.0)] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); column.title = title; column.width = width; column.isEditable = true
            if id == "enabled" { let cell = NSButtonCell(); cell.setButtonType(.switch); cell.title = ""; column.dataCell = cell }
            rulesTable.addTableColumn(column)
        }
        rulesTable.dataSource = self; rulesTable.delegate = self; rulesTable.rowHeight = 24
        add(scroll(rulesTable, height: 130))
        add(NSStackView(views: [button("규칙 추가", #selector(addRule)), button("선택 규칙 삭제", #selector(deleteRule))]), fullWidth: false)
        add(NSTextField(labelWithString: "편집한 값의 SSH 명령 (읽기 전용)")); add(scroll(command, height: 76))
        add(NSTextField(labelWithString: "현재 연결 상태 — 실행 중인 규칙 (읽기 전용)"))
        add(status)
        for (id, title, width) in [("description", "포워딩", 260.0), ("source", "출처", 90.0), ("state", "상태", 90.0), ("detail", "상세", 280.0)] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); column.title = title; column.width = width; column.isEditable = false; runtimeTable.addTableColumn(column)
        }
        runtimeTable.dataSource = self; runtimeTable.delegate = self; runtimeTable.rowHeight = 24; add(scroll(runtimeTable, height: 120))
        add(NSTextField(wrappingLabelWithString: "‘준비됨’은 포워딩 개설 성공을 뜻합니다. 목적지 서비스의 정상 동작을 보장하지 않습니다."))
        validation.textColor = .systemRed; add(validation)
        connectButton = button("연결·재연결", #selector(connectSelected)); authButton = button("인증 설정…", #selector(setupAuthentication))
        add(NSStackView(views: [button("선택 호스트 저장", #selector(saveClicked)), connectButton, authButton]), fullWidth: false)
        let okay = button("OK", #selector(okay)); okay.keyEquivalent = "\r"
        let cancel = button("Cancel", #selector(cancel)); cancel.keyEquivalent = "\u{1b}"
        add(NSStackView(views: [cancel, okay]), fullWidth: false)
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
