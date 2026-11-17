import AppKit
import Network
import LocalForwarderCore

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let store: ConfigurationStore
    lazy var manager = ConnectionManager(store: store)
    private lazy var settings = SettingsController(store: store, manager: manager)
    private let instance: SingleInstance
    private let openConfiguration: Bool
    private var tray: NSStatusItem!
    private var trayMenu = NSMenu()
    private var hostMenuItems: [String: NSMenuItem] = [:]
    private var forwardMenuItems: [String: [String: NSMenuItem]] = [:]
    private var allReconnectItem: NSMenuItem?
    private var allDisconnectItem: NSMenuItem?
    private var appModal: AppModal?
    private let loginItem = LoginItemController(service: MacLoginItemService())
    private var loginMenuItem: NSMenuItem?
    private var loginOperationPending = false
    private let monitor = NWPathMonitor()
    private var menuOpen = false
    private var terminationReady = false
    private var terminating = false
    private var launchReady = false
    private var pendingOpenRequest = false
    private var signals: [DispatchSourceSignal] = []

    init(instance: SingleInstance, openConfiguration: Bool, store: ConfigurationStore = ConfigurationStore()) {
        self.store = store
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
        loginItem.refresh()
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
    @objc private func quitFromMenu() { guard allowUserAction() else { return }; quit() }
    @objc private func quit() {
        guard !terminating else { return }
        terminating = true; monitor.cancel()
        appModal?.cancel()
        settings.prepareForTermination()
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
        if let appModal { pendingOpenRequest = true; appModal.window.makeKeyAndOrderFront(nil); return }
        trayMenu.cancelTrackingWithoutAnimation()
        openSettings()
    }
    private func allowUserAction() -> Bool {
        if let appModal { appModal.window.makeKeyAndOrderFront(nil); return false }
        if let sheet = NSApp.modalWindow ?? settings.activeSheet { sheet.makeKeyAndOrderFront(nil); return false }
        return !terminating && !loginOperationPending
    }
    @objc private func openSettings() { guard allowUserAction() else { return }; settings.open() }
    private func runDialog(_ dialog: AppModal) -> Bool {
        guard allowUserAction() else { return false }
        appModal = dialog; updateMenuPresentation()
        let accepted = dialog.run()
        appModal = nil; updateMenuPresentation()
        if pendingOpenRequest && !terminating {
            pendingOpenRequest = false
            DispatchQueue.main.async { [weak self] in self?.openFromSecondLaunch() }
        }
        return accepted && !terminating
    }
    @objc private func showAbout() { _ = runDialog(AppModal(.about)) }
    @objc private func disconnectAll() {
        guard manager.canDisconnectAll, allowUserAction() else { return }
        if runDialog(AppModal(.disconnectAll)) { manager.disconnectAll() }
    }
    @objc private func hostSettings(_ sender: NSMenuItem) {
        guard allowUserAction(), let id = sender.representedObject as? String else { return }
        settings.open(select: id)
    }
    @objc private func toggleLoginItem() {
        guard allowUserAction(), !loginOperationPending, loginItem.canToggle else { return }
        loginOperationPending = true
        updateMenuPresentation()
        Task { @MainActor [weak self] in
            guard let self else { return }
            defer { self.loginOperationPending = false; self.updateMenuPresentation() }
            do {
                let changed = try await self.loginItem.toggle()
                if changed, self.loginItem.status == .requiresApproval, !self.terminating {
                    let alert = NSAlert()
                    alert.messageText = "로그인 자동 시작 승인 필요"
                    alert.informativeText = "로그인 항목은 등록되었지만 아직 자동 시작할 수 없습니다. 시스템 설정의 로그인 항목에서 LocalForwarder를 허용해 주세요."
                    alert.addButton(withTitle: "시스템 설정 열기")
                    alert.addButton(withTitle: "나중에")
                    if alert.runModal() == .alertFirstButtonReturn { MacLoginItemService.openSystemSettings() }
                }
            } catch {
                if !self.terminating { self.showError("로그인 자동 시작 설정을 변경하지 못했습니다.\n" + error.localizedDescription) }
            }
        }
    }
    @objc private func reconnectAll() {
        guard allowUserAction() else { return }
        do { try manager.reconnectAll() } catch { showError(error.localizedDescription) }
    }
    @objc private func connectHost(_ sender: NSMenuItem) {
        guard allowUserAction(), let id = sender.representedObject as? String else { return }
        do { try manager.connect(id) } catch { showError(error.localizedDescription) }
    }
    @objc private func disconnectHost(_ sender: NSMenuItem) {
        guard allowUserAction() else { return }
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
    func menuWillOpen(_ menu: NSMenu) {
        loginItem.refresh()
        populateMenu(menu); menuOpen = true
    }
    func menuDidClose(_ menu: NSMenu) {
        menuOpen = false
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.menuOpen else { return }
            self.populateMenu(self.trayMenu)
        }
    }
    private func populateMenu(_ menu: NSMenu) {
        menu.removeAllItems(); hostMenuItems = [:]; forwardMenuItems = [:]
        func item(_ title: String, _ action: Selector?, in menu: NSMenu, id: String? = nil, tag: Int = 0) -> NSMenuItem {
            let item = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
            item.target = self; item.representedObject = id; item.tag = tag; return item
        }
        item("설정", #selector(openSettings), in: menu).image = menuImage("gearshape", "설정")
        loginMenuItem = item("로그인 자동 시작", #selector(toggleLoginItem), in: menu)
        allReconnectItem = item("전체 재연결", #selector(reconnectAll), in: menu)
        allReconnectItem?.image = menuImage("arrow.clockwise", "전체 재연결")
        allDisconnectItem = item("전체 연결 끊기", #selector(disconnectAll), in: menu)
        allDisconnectItem?.image = menuImage("xmark.circle", "전체 연결 끊기")
        menu.addItem(.separator())
        for host in manager.hosts {
            let hostItem = item(host.name, nil, in: menu)
            let submenu = NSMenu(); submenu.autoenablesItems = false; hostItem.submenu = submenu
            hostMenuItems[host.id] = hostItem
            item("", nil, in: submenu, tag: 1).isEnabled = false
            submenu.addItem(.separator())
            // Stable boundary for insertions/removals in the dynamic forward region.
            let boundary = NSMenuItem.separator(); boundary.tag = 9; submenu.addItem(boundary)
            item("연결", #selector(connectHost(_:)), in: submenu, id: host.id, tag: 5).image = menuImage("link", "연결")
            item("끊기", #selector(disconnectHost(_:)), in: submenu, id: host.id, tag: 6).image = menuImage("xmark.circle", "끊기")
            item("호스트 설정", #selector(hostSettings(_:)), in: submenu, id: host.id, tag: 7).image = menuImage("slider.horizontal.3", "호스트 설정")
        }
        menu.addItem(.separator())
        item("About...", #selector(showAbout), in: menu).image = menuImage("info.circle", "About")
        item("종료", #selector(quitFromMenu), in: menu).image = menuImage("power", "종료")
        updateMenuPresentation()
    }
    private func menuImage(_ symbol: String, _ label: String) -> NSImage? {
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: label)
        image?.size = NSSize(width: 16, height: 16)
        return image
    }
    private func updateForwardItems(_ host: Host, runtime: HostRuntime?, menu: NSMenu) {
        let rows = TrayPresentation.forwards(host: host, runtime: runtime)
        var items = forwardMenuItems[host.id] ?? [:]
        let ids = rows.isEmpty ? ["empty"] : rows.map(\.id)
        for id in Array(items.keys) where !ids.contains(id) {
            if let item = items.removeValue(forKey: id) { menu.removeItem(item) }
        }
        for (offset, id) in ids.enumerated() {
            let item: NSMenuItem
            if let existing = items[id] { item = existing }
            else { item = NSMenuItem(title: "", action: nil, keyEquivalent: ""); item.isEnabled = false; items[id] = item }
            let index = offset + 2
            if menu.index(of: item) != index {
                if item.menu != nil { menu.removeItem(item) }
                menu.insertItem(item, at: index)
            }
            if rows.isEmpty { item.title = "포워딩 규칙 없음"; item.image = nil; item.toolTip = nil }
            else {
                let row = rows[offset]
                item.title = row.text
                item.image = menuImage(row.state.symbol, row.state.title)
                item.toolTip = row.state.title + (row.detail.isEmpty ? "" : "\n" + row.detail)
            }
        }
        forwardMenuItems[host.id] = items
    }
    /// While tracking, mutate existing rows so status changes do not dismiss or rebuild the user's menu.
    private func updateMenuPresentation() {
        let modalBlocked = appModal != nil || NSApp.modalWindow != nil || settings.activeSheet != nil
        allDisconnectItem?.isEnabled = !terminating && !modalBlocked && manager.canDisconnectAll
        loginMenuItem?.title = loginOperationPending ? "로그인 자동 시작 (변경 중…)" : loginItem.menuTitle
        loginMenuItem?.state = loginItem.status == nil ? .mixed : (loginItem.isChecked ? .on : .off)
        loginMenuItem?.isEnabled = !terminating && !modalBlocked && !loginOperationPending && loginItem.canToggle
        loginMenuItem?.toolTip = loginItem.errorMessage
        let hosts = Dictionary(uniqueKeysWithValues: manager.hosts.map { ($0.id, $0) })
        allReconnectItem?.isEnabled = !terminating && !modalBlocked && !manager.hosts.contains {
            let state = manager.runtime($0.id)?.state
            return state == .connecting || state == .stopping
        }
        for (id, item) in hostMenuItems {
            guard let host = hosts[id] else { item.isHidden = true; continue }
            item.isHidden = false
            let runtime = manager.runtime(id)
            let connected = runtime?.state == .connected
            let busy = runtime?.state == .connecting || runtime?.state == .stopping
            let presentation = TrayPresentation.host(host, runtime: runtime)
            item.title = presentation.title
            item.image = menuImage(presentation.state.symbol, presentation.state.title)
            guard let menu = item.submenu else { continue }
            menu.item(withTag: 1)?.title = connectionSummary(runtime)
            updateForwardItems(host, runtime: runtime, menu: menu)
            menu.item(withTag: 5)?.title = connected ? "재연결" : (busy ? "연결 작업 중…" : "연결")
            menu.item(withTag: 5)?.image = menuImage(connected ? "arrow.clockwise" : "link", connected ? "재연결" : "연결")
            menu.item(withTag: 5)?.isEnabled = !busy && !terminating && !modalBlocked
            menu.item(withTag: 6)?.isHidden = !connected && !busy
            menu.item(withTag: 6)?.isEnabled = !terminating && !modalBlocked
        }
    }
    private func installApplicationMenu() {
        let menu = NSMenu(); let app = NSMenuItem(); let submenu = NSMenu()
        submenu.addItem(withTitle: "LocalForwarder 종료", action: #selector(quitFromMenu), keyEquivalent: "q").target = self
        app.submenu = submenu; menu.addItem(app)
        let edit = NSMenuItem(); let edits = NSMenu(title: "편집")
        for (title, action, key) in [("잘라내기", #selector(NSText.cut(_:)), "x"), ("복사", #selector(NSText.copy(_:)), "c"), ("붙여넣기", #selector(NSText.paste(_:)), "v"), ("전체 선택", #selector(NSText.selectAll(_:)), "a")] {
            edits.addItem(withTitle: title, action: action, keyEquivalent: key)
        }
        edit.submenu = edits; menu.addItem(edit); NSApp.mainMenu = menu
    }
    static func checkAppDialogs(store: ConfigurationStore) throws {
        final class Session: SSHSessionProtocol {
            var state: ConnectionState = .disconnected
            var lastError = ""
            var forwards: [ForwardRuntime] = []
            var onChange: (() -> Void)?
            var onError: ((String) -> Void)?
            func start(_ host: Host) { state = .connected; onChange?() }
            func stop(completion: @escaping () -> Void) { state = .disconnected; onChange?(); completion() }
        }
        let instance = SingleInstance()
        let controller = AppDelegate(instance: instance, openConfiguration: false, store: store)
        let manager = ConnectionManager(store: store, sessionFactory: { Session() })
        controller.manager = manager
        controller.launchReady = true
        controller.trayMenu.autoenablesItems = false
        controller.populateMenu(controller.trayMenu)
        let titles = controller.trayMenu.items.map(\.title)
        try NativeLayoutChecks.require(Array(titles.prefix(4)) == ["설정", controller.loginItem.menuTitle, "전체 재연결", "전체 연결 끊기"], "global tray menu order: \(titles)")
        try NativeLayoutChecks.require(Array(titles.suffix(2)) == ["About...", "종료"], "About precedes quit")
        try NativeLayoutChecks.require(controller.allDisconnectItem?.isEnabled == false, "idle disconnect all disabled")
        let id = manager.hosts[0].id
        try manager.connect(id)
        controller.updateMenuPresentation()
        try NativeLayoutChecks.require(controller.allDisconnectItem?.isEnabled == true, "active disconnect all enabled")
        let original = try Data(contentsOf: store.file)
        var failure: Error?
        func duringModal(_ action: @escaping () throws -> Void) {
            let timer = Timer(timeInterval: 0.05, repeats: false) { _ in
                MainActor.assumeIsolated {
                    do { try action() } catch { failure = error; controller.appModal?.cancel() }
                }
            }
            RunLoop.main.add(timer, forMode: .modalPanel)
        }
        var openedURL: URL?
        let about = AppModal(.about, openURL: { openedURL = $0 })
        duringModal {
            try NativeLayoutChecks.require(NSApp.modalWindow === about.window, "About is application modal")
            try NativeLayoutChecks.settle(about.window)
            let content = about.window.contentView!
            try NativeLayoutChecks.check(content.subviews.flatMap { NativeLayoutChecks.leaves($0) }, in: content, context: "About")
            try NativeLayoutChecks.require(about.labels.map(\.stringValue) == ["LocalForwarder", "버전 " + ProductVersion.current, "개발자 GuruJung"], "About product information")
            try NativeLayoutChecks.require(openedURL == nil, "About opening must not open browser")
            about.linkButton?.performClick(nil)
            try NativeLayoutChecks.require(openedURL == AppModal.repositoryURL, "About repository link")
            try NativeLayoutChecks.require(manager.runtime(id)?.state == .connected, "About preserves connection")
            controller.showAbout()
            try NativeLayoutChecks.require(controller.appModal === about, "duplicate About reuses existing modal")
            controller.openFromSecondLaunch()
            try NativeLayoutChecks.require(controller.pendingOpenRequest, "second launch waits for application modal")
            controller.quitFromMenu()
            try NativeLayoutChecks.require(!controller.terminating, "user quit is blocked while modal")
            about.window.performClose(nil)
        }
        try controller.settings.checkDraftPreserved {
            _ = controller.runDialog(about)
            if let failure { throw failure }
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.05))
            try NativeLayoutChecks.require(!controller.pendingOpenRequest, "second launch resumes after modal")
        }
        try NativeLayoutChecks.require(NSApp.modalWindow == nil && !about.window.isVisible, "About close ends modal")
        for close in ["cancel", "escape", "close", "confirm"] {
            duringModal {
                guard let modal = controller.appModal else { throw AppError("Disconnect confirmation missing") }
                try NativeLayoutChecks.require(NSApp.modalWindow === modal.window, "disconnect confirmation modal")
                try NativeLayoutChecks.settle(modal.window)
                let content = modal.window.contentView!
                try NativeLayoutChecks.check(content.subviews.flatMap { NativeLayoutChecks.leaves($0) }, in: content, context: "disconnect confirmation")
                try NativeLayoutChecks.require(modal.window.defaultButtonCell === modal.cancelButton.cell, "cancel is default")
                try NativeLayoutChecks.require(manager.runtime(id)?.desiredConnected == true, "confirmation waits for approval")
                switch close {
                case "cancel": modal.cancelButton.performClick(nil)
                case "escape": modal.window.cancelOperation(nil)
                case "close": modal.window.performClose(nil)
                default: modal.confirmButton?.performClick(nil)
                }
            }
            controller.disconnectAll()
            if let failure { throw failure }
            try NativeLayoutChecks.require(manager.runtime(id)?.desiredConnected == (close != "confirm"), "disconnect confirmation result " + close)
        }
        try NativeLayoutChecks.require(try Data(contentsOf: store.file) == original, "dialogs preserve configuration bytes")
    }
    private func showError(_ message: String) {
        let alert = NSAlert(); alert.messageText = "LocalForwarder"; alert.informativeText = message; alert.alertStyle = .warning; alert.runModal()
    }
}
