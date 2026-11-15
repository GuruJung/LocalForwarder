import AppKit
import Network
import LocalForwarderCore

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let store = ConfigurationStore()
    lazy var manager = ConnectionManager(store: store)
    private lazy var settings = SettingsController(store: store, manager: manager)
    private let instance: SingleInstance
    private let openConfiguration: Bool
    private var tray: NSStatusItem!
    private var trayMenu = NSMenu()
    private var hostMenuItems: [String: NSMenuItem] = [:]
    private var allReconnectItem: NSMenuItem?
    private let monitor = NWPathMonitor()
    private var menuOpen = false
    private var terminationReady = false
    private var terminating = false
    private var launchReady = false
    private var pendingOpenRequest = false
    private var signals: [DispatchSourceSignal] = []

    init(instance: SingleInstance, openConfiguration: Bool) {
        self.instance = instance; self.openConfiguration = openConfiguration
        super.init()
        instance.onOpenSettings = { [weak self] in self?.openFromSecondLaunch() }
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        NSApp.applicationIconImage = ForwardingIcon.image(size: 512, template: false)
        installApplicationMenu()
        tray = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        tray.button?.image = ForwardingIcon.image()
        trayMenu.autoenablesItems = false; trayMenu.delegate = self; tray.menu = trayMenu
        manager.onChange = { [weak self] in self?.refresh() }
        // Runtime errors remain visible per host; a failed host never dismisses the settings or exits the app.
        manager.onError = { [weak self] _, _ in self?.refresh() }
        manager.onExit = { [weak self] in self?.finishTermination() }
        settings.onClose = { [weak self] accepted, startup in
            guard let self, accepted && startup else { return }
            do { try self.manager.startAll() } catch { self.showError(error.localizedDescription) }
        }
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(woke), name: NSWorkspace.didWakeNotification, object: nil)
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(slept), name: NSWorkspace.willSleepNotification, object: nil)
        monitor.pathUpdateHandler = { [weak self] path in
            DispatchQueue.main.async { self?.manager.networkChanged(available: path.status == .satisfied) }
        }
        monitor.start(queue: DispatchQueue(label: "LocalForwarder.network"))
        for number in [SIGTERM, SIGINT] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { [weak self] in self?.quit() }; source.resume(); signals.append(source)
        }
        launchReady = true; refresh()
        do {
            try manager.reloadConfiguration()
            if openConfiguration || !FileManager.default.fileExists(atPath: store.file.path) {
                settings.open(startup: true)
            } else { try manager.startAll() }
        } catch {
            showError(error.localizedDescription + "\n기존 설정 파일은 보존했습니다.")
            settings.open(startup: true)
        }
        if pendingOpenRequest { pendingOpenRequest = false; openFromSecondLaunch() }
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        openFromSecondLaunch(); return false
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if terminationReady { return .terminateNow }
        quit(); return .terminateCancel
    }
    @objc private func woke() { manager.wake() }
    @objc private func slept() { manager.sleep() }
    @objc private func quit() {
        guard !terminating else { return }
        terminating = true; monitor.cancel()
        ProcessTools.preventNewCaptures()
        manager.quit()
    }
    private func finishTermination() {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            ProcessTools.cancelCapturedProcesses()
            DispatchQueue.main.async {
                guard let self else { return }
                self.instance.stop(); self.terminationReady = true; NSApp.terminate(nil)
            }
        }
    }
    private func openFromSecondLaunch() {
        guard launchReady else { pendingOpenRequest = true; return }
        guard !settings.isOpen else { return }
        trayMenu.cancelTrackingWithoutAnimation()
        openSettings()
    }
    @objc private func openSettings() { settings.open() }
    @objc private func hostSettings(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        settings.open(select: id)
    }
    @objc private func reconnectAll() {
        do { try manager.reconnectAll() } catch { showError(error.localizedDescription) }
    }
    @objc private func connectHost(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        do { try manager.connect(id) } catch { showError(error.localizedDescription) }
    }
    @objc private func disconnectHost(_ sender: NSMenuItem) {
        if let id = sender.representedObject as? String { manager.disconnect(id) }
    }
    private func refresh() {
        guard tray != nil else { return }
        let connected = manager.hosts.filter { manager.runtime($0.id)?.state == .connected }.count
        let disconnected = manager.hosts.filter { $0.autoConnect && manager.runtime($0.id)?.state != .connected }.count
        tray.button?.toolTip = "LocalForwarder · 연결됨 \(connected) · 연결 안 됨 \(disconnected)"
        if menuOpen { updateMenuPresentation() } else { populateMenu(trayMenu) }
        settings.refresh()
    }
    func menuWillOpen(_ menu: NSMenu) { populateMenu(menu); menuOpen = true }
    func menuDidClose(_ menu: NSMenu) {
        menuOpen = false
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.menuOpen else { return }
            self.populateMenu(self.trayMenu)
        }
    }
    private func populateMenu(_ menu: NSMenu) {
        menu.removeAllItems(); hostMenuItems = [:]
        func item(_ title: String, _ action: Selector?, in menu: NSMenu, id: String? = nil, tag: Int = 0) -> NSMenuItem {
            let item = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
            item.target = self; item.representedObject = id; item.tag = tag; return item
        }
        _ = item("설정", #selector(openSettings), in: menu)
        allReconnectItem = item("전체 재연결", #selector(reconnectAll), in: menu)
        menu.addItem(.separator())
        for host in manager.hosts {
            let hostItem = item(host.name, nil, in: menu)
            let submenu = NSMenu(); submenu.autoenablesItems = false; hostItem.submenu = submenu
            hostMenuItems[host.id] = hostItem
            for tag in 1...4 {
                let description = item("", nil, in: submenu, tag: tag)
                description.isEnabled = false
            }
            submenu.addItem(.separator())
            _ = item("연결", #selector(connectHost(_:)), in: submenu, id: host.id, tag: 5)
            _ = item("끊기", #selector(disconnectHost(_:)), in: submenu, id: host.id, tag: 6)
            _ = item("호스트 설정", #selector(hostSettings(_:)), in: submenu, id: host.id, tag: 7)
        }
        menu.addItem(.separator()); _ = item("종료", #selector(quit), in: menu)
        updateMenuPresentation()
    }
    /// While tracking, mutate existing rows so status changes do not dismiss or rebuild the user's menu.
    private func updateMenuPresentation() {
        let hosts = Dictionary(uniqueKeysWithValues: manager.hosts.map { ($0.id, $0) })
        allReconnectItem?.isEnabled = !terminating && !manager.hosts.contains {
            let state = manager.runtime($0.id)?.state
            return state == .connecting || state == .stopping
        }
        for (id, item) in hostMenuItems {
            guard let host = hosts[id] else { item.isHidden = true; continue }
            item.isHidden = false
            let runtime = manager.runtime(id)
            let connected = runtime?.state == .connected
            let busy = runtime?.state == .connecting || runtime?.state == .stopping
            let warning = connected && runtime?.forwards.contains(where: { $0.state == .failed }) == true
            let temporary = runtime?.temporary == true ? " · 임시" : ""
            let progress = busy ? " · 작업 중…" : ""
            item.title = host.name + temporary + progress + (warning ? " · 포워딩 경고" : "")
            let symbol: String
            let state: String
            if busy { symbol = "arrow.triangle.2.circlepath"; state = "연결 작업 중" }
            else if warning { symbol = "exclamationmark.triangle.fill"; state = "SSH 연결됨, 포워딩 경고" }
            else if connected { symbol = "checkmark.circle.fill"; state = "연결됨" }
            else if host.autoConnect { symbol = "circle"; state = "연결 안 됨" }
            else { symbol = "minus.circle"; state = "자동 연결 꺼짐, 연결 안 됨" }
            item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: state)
            item.image?.size = NSSize(width: 16, height: 16)
            item.toolTip = state + " · " + connectionSummary(runtime)
            guard let menu = item.submenu else { continue }
            menu.item(withTag: 1)?.title = connectionSummary(runtime)
            let error = runtime?.lastError ?? ""
            menu.item(withTag: 2)?.isHidden = error.isEmpty
            menu.item(withTag: 2)?.title = "최근 오류: " + String(error.replacingOccurrences(of: "\n", with: " ").prefix(140))
            menu.item(withTag: 2)?.toolTip = error
            var executionNote = ""
            if runtime?.temporary == true { executionNote = "이번 실행의 임시 설정 사용 중" }
            else if let execution = runtime?.executionHost, execution != host, connected || busy {
                executionNote = "현재 실행 설정과 저장된 설정이 다릅니다"
            }
            menu.item(withTag: 3)?.isHidden = executionNote.isEmpty
            menu.item(withTag: 3)?.title = executionNote
            menu.item(withTag: 4)?.isHidden = runtime?.recentErrors.isEmpty != false
            if let error = runtime?.recentErrors.last {
                menu.item(withTag: 4)?.title = DateFormatter.localizedString(from: error.time, dateStyle: .short, timeStyle: .medium) + " · " + String(error.message.prefix(120))
                menu.item(withTag: 4)?.toolTip = error.message
            }
            menu.item(withTag: 5)?.title = connected ? "재연결" : (busy ? "연결 작업 중…" : "연결")
            menu.item(withTag: 5)?.isEnabled = !busy && !terminating
            menu.item(withTag: 6)?.isHidden = !connected && !busy
            menu.item(withTag: 6)?.isEnabled = !terminating
        }
    }
    private func installApplicationMenu() {
        let menu = NSMenu(); let app = NSMenuItem(); let submenu = NSMenu()
        submenu.addItem(withTitle: "LocalForwarder 종료", action: #selector(quit), keyEquivalent: "q").target = self
        app.submenu = submenu; menu.addItem(app)
        let edit = NSMenuItem(); let edits = NSMenu(title: "편집")
        for (title, action, key) in [("잘라내기", #selector(NSText.cut(_:)), "x"), ("복사", #selector(NSText.copy(_:)), "c"), ("붙여넣기", #selector(NSText.paste(_:)), "v"), ("전체 선택", #selector(NSText.selectAll(_:)), "a")] {
            edits.addItem(withTitle: title, action: action, keyEquivalent: key)
        }
        edit.submenu = edits; menu.addItem(edit); NSApp.mainMenu = menu
    }
    private func showError(_ message: String) {
        let alert = NSAlert(); alert.messageText = "LocalForwarder"; alert.informativeText = message; alert.alertStyle = .warning; alert.runModal()
    }
}
