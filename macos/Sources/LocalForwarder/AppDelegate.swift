import AppKit
import LocalForwarderCore

typealias Host = LocalForwarderCore.Host

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate {
    let store = ConfigurationStore()
    lazy var session = SSHSession(store: store)
    var window: NSWindow!
    let selector = NSPopUpButton()
    let nameField = NSTextField()
    let targetField = NSTextField()
    let userField = NSTextField()
    let portField = NSTextField()
    let keyField = NSTextField()
    let table = NSTableView()
    let inherited = NSTextView()
    let statusText = NSTextView()
    let validation = NSTextField(wrappingLabelWithString: "")
    var connectButton: NSButton!
    var addHostButton: NSButton!
    var deleteHostButton: NSButton!
    var selected: Host?
    var hosts: [Host] = []
    var rules: [Forward] = []
    var activeID: String?
    var tray: NSStatusItem?
    var inspectionGeneration = 0
    var finishing = false
    var terminationReady = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        installMenu()
        makeWindow()
        session.onChange = { [weak self] state in self?.changed(state) }
        session.onInitialFailure = { [weak self] error in self?.initialFailure(error) }
        session.onExit = { [weak self] in self?.finishTermination() }
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(woke), name: NSWorkspace.didWakeNotification, object: nil)
        openSettings()
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.createsNewApplicationInstance = true
        NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, configuration: configuration) { [weak self] _, error in
            if let error { DispatchQueue.main.async { self?.showError(error.localizedDescription) } }
        }
        return false
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if terminationReady { return .terminateNow }
        session.quit(); return .terminateCancel
    }
    private func finishTermination() {
        guard !finishing else { return }
        finishing = true; inspectionGeneration += 1
        DispatchQueue.global(qos: .utility).async { [weak self] in
            ProcessTools.cancelCapturedProcesses()
            DispatchQueue.main.async {
                self?.terminationReady = true
                NSApp.terminate(nil)
            }
        }
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if session.state == .editing { session.quit() } else { sender.orderOut(nil) }
        return false
    }
    @objc func woke() { session.wake() }
    private func installMenu() {
        let menu = NSMenu()
        let app = NSMenuItem(); let submenu = NSMenu()
        submenu.addItem(withTitle: "LocalForwarder 종료", action: #selector(quit), keyEquivalent: "q").target = self
        app.submenu = submenu; menu.addItem(app)
        let edit = NSMenuItem(); let edits = NSMenu(title: "편집")
        for (title, action, key) in [("잘라내기", #selector(NSText.cut(_:)), "x"), ("복사", #selector(NSText.copy(_:)), "c"), ("붙여넣기", #selector(NSText.paste(_:)), "v"), ("전체 선택", #selector(NSText.selectAll(_:)), "a")] {
            edits.addItem(withTitle: title, action: action, keyEquivalent: key)
        }
        edit.submenu = edits; menu.addItem(edit); NSApp.mainMenu = menu
    }
    private func row(_ label: String, _ control: NSView) -> NSStackView {
        let title = NSTextField(labelWithString: label); title.widthAnchor.constraint(equalToConstant: 110).isActive = true
        let stack = NSStackView(views: [title, control]); stack.orientation = .horizontal; stack.alignment = .centerY; stack.spacing = 10
        control.setContentHuggingPriority(.defaultLow, for: .horizontal)
        return stack
    }
    private func button(_ title: String, _ selector: Selector) -> NSButton { NSButton(title: title, target: self, action: selector) }
    private func scrolling(_ view: NSView, height: CGFloat) -> NSScrollView {
        let scroll = NSScrollView(); scroll.hasVerticalScroller = true; scroll.borderType = .bezelBorder; scroll.documentView = view
        scroll.heightAnchor.constraint(equalToConstant: height).isActive = true
        if let text = view as? NSTextView {
            text.isEditable = false; text.isSelectable = true; text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
            text.isVerticallyResizable = true; text.isHorizontallyResizable = false
            text.textContainer?.widthTracksTextView = true
            text.autoresizingMask = [.width]
        }
        return scroll
    }
    private func makeWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 820, height: 790), styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "LocalForwarder"; window.delegate = self; window.isReleasedWhenClosed = false; window.center()
        let stack = NSStackView(); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        window.contentView!.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor, constant: 20), stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor, constant: -20), stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor, constant: 20)])
        selector.target = self; selector.action = #selector(selectedHost)
        selector.widthAnchor.constraint(equalToConstant: 480).isActive = true
        addHostButton = button("추가", #selector(addHost)); deleteHostButton = button("삭제", #selector(deleteHost))
        let header = NSStackView(views: [selector, addHostButton, deleteHostButton]); header.spacing = 8; stack.addArrangedSubview(header)
        for (label, field) in [("이름", nameField), ("SSH 별칭 / 주소", targetField), ("사용자명 (선택)", userField), ("SSH 포트 (선택)", portField), ("개인키 (선택)", keyField)] {
            let r = row(label, field); stack.addArrangedSubview(r); r.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
        targetField.placeholderString = "예: my-server 또는 server.example.com"
        keyField.placeholderString = "비워두면 기존 SSH 설정 사용"
        let chooseKey = button("개인키 파일 선택…", #selector(chooseKey)); stack.addArrangedSubview(chooseKey)
        stack.addArrangedSubview(NSTextField(labelWithString: "앱 포워딩 규칙 — 내 컴퓨터(127.0.0.1)에서만 접속"))
        for (id, title, width) in [("enabled", "사용", 55.0), ("localPort", "로컬 포트", 125.0), ("remoteHost", "목적지 호스트", 380.0), ("remotePort", "목적지 포트", 130.0)] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id)); column.title = title; column.width = width; column.isEditable = true
            if id == "enabled" { let cell = NSButtonCell(); cell.setButtonType(.switch); cell.title = ""; column.dataCell = cell }
            table.addTableColumn(column)
        }
        table.dataSource = self; table.delegate = self; table.rowHeight = 24; table.allowsMultipleSelection = false
        let tableScroll = scrolling(table, height: 150); tableScroll.hasHorizontalScroller = true
        stack.addArrangedSubview(tableScroll); tableScroll.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        stack.addArrangedSubview(NSStackView(views: [button("규칙 추가", #selector(addRule)), button("선택 규칙 삭제", #selector(deleteRule))]))
        let inheritedRow = NSStackView(views: [NSTextField(labelWithString: "기존 SSH 포워딩 (읽기 전용)"), button("새로 확인", #selector(inspectHost))]); inheritedRow.spacing = 15; stack.addArrangedSubview(inheritedRow)
        let inheritedScroll = scrolling(inherited, height: 70); stack.addArrangedSubview(inheritedScroll); inheritedScroll.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        validation.textColor = .systemRed; stack.addArrangedSubview(validation); validation.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        connectButton = button("저장 후 연결", #selector(connect)); connectButton.keyEquivalent = "\r"
        let actions = NSStackView(views: [button("저장", #selector(saveClicked)), connectButton, button("인증 설정…", #selector(setupAuthentication)), button("종료", #selector(quit))]); actions.spacing = 12; stack.addArrangedSubview(actions)
        let statusScroll = scrolling(statusText, height: 95); stack.addArrangedSubview(statusScroll); statusScroll.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
    }
    @objc func openSettings() {
        do {
            hosts = try store.load().hosts
            let desired = activeID ?? selected?.id
            selector.removeAllItems(); selector.addItems(withTitles: hosts.map { $0.name })
            if let index = hosts.firstIndex(where: { $0.id == desired }) { selector.selectItem(at: index); fill(hosts[index]) }
            else if activeID != nil { throw AppError("연결 중인 호스트가 설정에서 삭제되었습니다.") }
            else if let first = hosts.first { selector.selectItem(at: 0); fill(first) }
            else { fill(Host()); selector.title = "새 호스트" }
        } catch { validation.stringValue = error.localizedDescription }
        window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        updateStatus()
    }
    func fill(_ host: Host) {
        selected = host; nameField.stringValue = host.name; targetField.stringValue = host.target
        userField.stringValue = host.user ?? ""; portField.stringValue = host.port.map(String.init) ?? ""
        keyField.stringValue = host.identityFile ?? ""; rules = host.forwards; table.reloadData(); validation.stringValue = ""
        if !host.target.isEmpty { inspectHost() } else { inherited.string = "호스트를 입력한 뒤 새로 확인하세요." }
    }
    @objc func selectedHost() { if hosts.indices.contains(selector.indexOfSelectedItem) { fill(hosts[selector.indexOfSelectedItem]) } }
    @objc func addHost() { fill(Host()); selector.select(nil) }
    @objc func deleteHost() {
        guard let host = selected else { return }
        let alert = NSAlert(); alert.messageText = "\(host.name)을 삭제할까요?"; alert.addButton(withTitle: "삭제"); alert.addButton(withTitle: "취소")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        do { try store.delete(host.id); selected = nil; openSettings() } catch { showError(error.localizedDescription) }
    }
    @objc func chooseKey() {
        let panel = NSOpenPanel(); panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { keyField.stringValue = url.path }
    }
    @objc func addRule() { window.makeFirstResponder(nil); rules.append(Forward()); table.reloadData() }
    @objc func deleteRule() { window.makeFirstResponder(nil); if rules.indices.contains(table.selectedRow) { rules.remove(at: table.selectedRow); table.reloadData() } }
    func numberOfRows(in tableView: NSTableView) -> Int { rules.count }
    func tableView(_ tableView: NSTableView, objectValueFor tableColumn: NSTableColumn?, row: Int) -> Any? {
        guard rules.indices.contains(row) else { return nil }
        let rule = rules[row]
        switch tableColumn?.identifier.rawValue {
        case "enabled": return rule.enabled ? NSControl.StateValue.on.rawValue : NSControl.StateValue.off.rawValue
        case "localPort": return String(rule.localPort)
        case "remoteHost": return rule.remoteHost
        case "remotePort": return String(rule.remotePort)
        default: return nil
        }
    }
    func tableView(_ tableView: NSTableView, setObjectValue object: Any?, for tableColumn: NSTableColumn?, row: Int) {
        guard rules.indices.contains(row) else { return }
        switch tableColumn?.identifier.rawValue {
        case "enabled": rules[row].enabled = (object as? NSNumber)?.intValue == NSControl.StateValue.on.rawValue
        case "localPort": rules[row].localPort = Int(String(describing: object ?? "")) ?? 0
        case "remoteHost": rules[row].remoteHost = String(describing: object ?? "")
        case "remotePort": rules[row].remotePort = Int(String(describing: object ?? "")) ?? 0
        default: break
        }
    }
    func draft() throws -> Host {
        window.makeFirstResponder(nil)
        var host = selected ?? Host(); host.name = nameField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        host.target = targetField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        let user = userField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines); host.user = user.isEmpty ? nil : user
        let port = portField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        if port.isEmpty { host.port = nil } else if let number = Int(port) { host.port = number } else { throw AppError("SSH 포트는 숫자여야 합니다.") }
        host.identityFile = keyField.stringValue.isEmpty ? nil : keyField.stringValue
        host.forwards = rules; try host.validate(); return host
    }
    @discardableResult func saveDraft() -> Host? {
        do {
            let host = try draft(); try store.save(host); selected = host
            hosts = try store.load().hosts; selector.removeAllItems(); selector.addItems(withTitles: hosts.map { $0.name })
            if let index = hosts.firstIndex(where: { $0.id == host.id }) { selector.selectItem(at: index) }
            validation.stringValue = ""; return host
        } catch { validation.stringValue = error.localizedDescription; return nil }
    }
    @objc func saveClicked() {
        guard saveDraft() != nil else { return }
        inspectHost()
        if session.state == .connected {
            let alert = NSAlert(); alert.messageText = "설정을 저장했습니다."; alert.informativeText = "변경 사항을 다음 연결부터 적용하거나 지금 재연결할 수 있습니다."
            alert.addButton(withTitle: "다음 연결부터 적용"); alert.addButton(withTitle: "지금 재연결")
            if alert.runModal() == .alertSecondButtonReturn { session.reconnect() }
        }
    }
    @objc func connect() { guard let host = saveDraft() else { return }; activeID = host.id; session.start(host.id) }
    @objc func inspectHost() {
        inspectionGeneration += 1; let token = inspectionGeneration
        do {
            let host = try draft(); inherited.string = "SSH 설정을 확인하고 있습니다…"
            DispatchQueue.global().async { [weak self] in
                let result: String
                do {
                    let capture = try ProcessTools.capture(SSHCommand.executable, SSHCommand.inspect(host))
                    if capture.status != 0 { result = capture.output }
                    else { let forwards = SSHCommand.inheritedForwards(capture.output); result = forwards.isEmpty ? "기존 포워딩 없음" : forwards.joined(separator: "\n") }
                } catch { result = error.localizedDescription }
                DispatchQueue.main.async { [weak self] in if self?.inspectionGeneration == token { self?.inherited.string = result } }
            }
        } catch { inherited.string = error.localizedDescription }
    }
    func changed(_ state: ConnectionState) {
        let editing = state == .editing
        selector.isEnabled = editing; addHostButton.isEnabled = editing; deleteHostButton.isEnabled = editing; connectButton.isHidden = !editing
        if state == .connected { window.orderOut(nil); if tray == nil { tray = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength) } }
        if let tray {
            let connected = state == .connected
            let image = NSImage(size: NSSize(width: 18, height: 18), flipped: false) { rect in
                NSColor.labelColor.set(); let circle = NSBezierPath(ovalIn: rect.insetBy(dx: 3, dy: 3))
                if connected { circle.fill() } else { circle.lineWidth = 2; circle.stroke(); let slash = NSBezierPath(); slash.move(to: NSPoint(x: 3, y: 3)); slash.line(to: NSPoint(x: 15, y: 15)); slash.lineWidth = 2; slash.stroke() }; return true
            }; image.isTemplate = true; tray.button?.image = image
            tray.button?.toolTip = "\(session.hostName): \(connected ? "connected" : "disconnected")"
            let menu = NSMenu(); menu.addItem(withTitle: "설정", action: #selector(openSettings), keyEquivalent: "").target = self
            if state == .waitingReconnect || state == .waitingExit || state == .reconnecting {
                let item = menu.addItem(withTitle: state == .reconnecting ? "재연결 중…" : "재연결", action: #selector(reconnect), keyEquivalent: ""); item.target = self; item.isEnabled = state != .reconnecting
            }
            menu.addItem(withTitle: "종료", action: #selector(quit), keyEquivalent: "").target = self
            menu.autoenablesItems = false; tray.menu = menu
        }
        updateStatus()
    }
    func updateStatus() {
        let detail: String
        switch session.state {
        case .editing: detail = "호스트를 선택하고 연결하세요."
        case .connecting: detail = "연결 중…"
        case .connected: detail = "connected — SSH 연결됨"
        case .waitingReconnect: detail = "disconnected — 단절 감지 1분 뒤 자동 재연결합니다."
        case .reconnecting: detail = "disconnected — 재연결 중…"
        case .waitingExit: detail = "disconnected — 재연결 실패. 최종 실패 1분 뒤 종료합니다."
        case .exiting: detail = "종료 중…"
        }
        statusText.string = detail + (session.lastError.isEmpty ? "" : "\n\n" + session.lastError)
    }
    @objc func reconnect() { session.reconnect() }
    @objc func quit() { session.quit() }
    func showError(_ message: String) {
        let alert = NSAlert(); alert.messageText = "LocalForwarder"; alert.informativeText = message; alert.alertStyle = .warning; alert.runModal()
    }
    func initialFailure(_ error: String) {
        let canRecover = SSHCommand.needsAuthenticationHelp(error)
        let alert = NSAlert(); alert.messageText = "SSH 연결에 실패했습니다."
        alert.informativeText = canRecover ? "다음 터미널에서 서버 지문을 확인하고 필요한 비밀번호를 입력하세요. 공개키 등록과 자동 로그인 검증을 지원합니다." : "원인을 확인한 뒤 앱을 다시 실행하세요."
        let text = NSTextView(frame: NSRect(x: 0, y: 0, width: 620, height: 200)); text.string = error
        let scroll = scrolling(text, height: 200); scroll.frame = NSRect(x: 0, y: 0, width: 620, height: 200); alert.accessoryView = scroll
        if canRecover { alert.addButton(withTitle: "터미널에서 해결"); alert.addButton(withTitle: "종료") } else { alert.addButton(withTitle: "종료") }
        if alert.runModal() == .alertFirstButtonReturn && canRecover { launchTerminal() } else { session.quit() }
    }
    @objc func setupAuthentication() {
        guard saveDraft() != nil else { return }
        let alert = NSAlert(); alert.messageText = "외부 터미널에서 인증을 설정합니다."
        alert.informativeText = "서버 지문을 확인하고 필요한 비밀번호를 터미널에 입력하세요. 기존 공개키를 보존하며 키 등록 후 자동 로그인을 검증합니다. 이 앱 인스턴스는 종료됩니다."
        alert.addButton(withTitle: "터미널 열기"); alert.addButton(withTitle: "취소")
        if alert.runModal() == .alertFirstButtonReturn { launchTerminal() }
    }
    func launchTerminal() {
        guard let id = activeID ?? selected?.id, let executable = Bundle.main.executableURL else { return }
        do {
            try FileManager.default.createDirectory(at: store.directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            let script = store.directory.appendingPathComponent("recovery-\(UUID().uuidString).command")
            let body = "#!/bin/sh\nrm -f -- \"$0\"\n" + SSHCommand.shellQuote(executable.path) + " --setup-auth " + SSHCommand.shellQuote(id) + "\n"
            try Data(body.utf8).write(to: script, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: script.path)
            let configuration = NSWorkspace.OpenConfiguration()
            NSWorkspace.shared.open([script], withApplicationAt: URL(fileURLWithPath: "/System/Applications/Utilities/Terminal.app"), configuration: configuration) { [weak self] _, error in
                DispatchQueue.main.async {
                    if let error { try? FileManager.default.removeItem(at: script); self?.showError(error.localizedDescription); self?.session.quit() }
                    else { self?.session.quit() }
                }
            }
        } catch { showError(error.localizedDescription); session.quit() }
    }
}
