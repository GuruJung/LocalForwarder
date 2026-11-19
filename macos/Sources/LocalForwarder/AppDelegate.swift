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
    private var languageMenuItem: NSMenuItem?
    private let languagePreferences: LanguagePreferences
    private var loginOperationPending = false
    private let monitor = NWPathMonitor()
    private var menuOpen = false
    private var terminationReady = false
    private var terminating = false
    private var launchReady = false
    private var pendingOpenRequest = false
    private var signals: [DispatchSourceSignal] = []

    init(instance: SingleInstance, openConfiguration: Bool, store: ConfigurationStore = ConfigurationStore(), languagePreferences: LanguagePreferences = .system) {
        self.store = store; self.languagePreferences = languagePreferences
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
            showError(error.localizedDescription + L10n.text("mac.the.existing.configuration.file.was.preserved"))
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
                    alert.messageText = L10n.text("mac.launch.at.login.approval.required")
                    alert.informativeText = L10n.text("mac.the.login.item.is.registered.but.cannot.start.automatically")
                    alert.addButton(withTitle: L10n.text("mac.open.system.settings"))
                    alert.addButton(withTitle: L10n.text("mac.later"))
                    if alert.runModal() == .alertFirstButtonReturn { MacLoginItemService.openSystemSettings() }
                }
            } catch {
                if !self.terminating { self.showError(L10n.text("mac.could.not.change.the.launch.at.login.setting") + error.localizedDescription) }
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
        tray.button?.toolTip = L10n.text("mac.localforwarder.connected.connected.disconnected.disconnected", ["connected": String(describing: connected), "disconnected": String(describing: disconnected)])
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
    @objc private func changeLanguage(_ sender: NSMenuItem) {
        guard allowUserAction(), let choice = sender.representedObject as? String else { return }
        do {
            try languagePreferences.select(choice)
            settings.languageChanged()
            installApplicationMenu()
            // The selected menu is closing; rebuild only after tracking has completed.
            DispatchQueue.main.async { [weak self] in self?.refresh() }
        } catch { showError(error.localizedDescription) }
    }
    private func populateMenu(_ menu: NSMenu) {
        menu.removeAllItems(); hostMenuItems = [:]; forwardMenuItems = [:]
        func item(_ title: String, _ action: Selector?, in menu: NSMenu, id: String? = nil, tag: Int = 0) -> NSMenuItem {
            let item = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
            item.target = self; item.representedObject = id; item.tag = tag; return item
        }
        item(L10n.text("mac.settings"), #selector(openSettings), in: menu).image = menuImage("gearshape", L10n.text("mac.settings"))
        let languages = NSMenu(); languages.autoenablesItems = false
        languageMenuItem = item(L10n.text("mac.language"), nil, in: menu)
        languageMenuItem?.submenu = languages
        let automatic = item(L10n.automaticTitle, #selector(changeLanguage(_:)), in: languages, id: "auto")
        automatic.state = L10n.selection == "auto" ? .on : .off
        languages.addItem(.separator())
        for (id, name) in zip(L10n.languages, L10n.languageNames) {
            let entry = item(name, #selector(changeLanguage(_:)), in: languages, id: id)
            entry.state = L10n.selection == id ? .on : .off
        }
        loginMenuItem = item(L10n.text("mac.launch.at.login"), #selector(toggleLoginItem), in: menu)
        allReconnectItem = item(L10n.text("mac.reconnect.all"), #selector(reconnectAll), in: menu)
        allReconnectItem?.image = menuImage("arrow.clockwise", L10n.text("mac.reconnect.all"))
        allDisconnectItem = item(L10n.text("mac.disconnect.all"), #selector(disconnectAll), in: menu)
        allDisconnectItem?.image = menuImage("xmark.circle", L10n.text("mac.disconnect.all"))
        menu.addItem(.separator())
        for host in manager.hosts {
            let hostItem = item(host.name, nil, in: menu)
            let submenu = NSMenu(); submenu.autoenablesItems = false; hostItem.submenu = submenu
            hostMenuItems[host.id] = hostItem
            item("", nil, in: submenu, tag: 1).isEnabled = false
            submenu.addItem(.separator())
            // Stable boundary for insertions/removals in the dynamic forward region.
            let boundary = NSMenuItem.separator(); boundary.tag = 9; submenu.addItem(boundary)
            item(L10n.text("mac.connect"), #selector(connectHost(_:)), in: submenu, id: host.id, tag: 5).image = menuImage("link", L10n.text("mac.connect"))
            item(L10n.text("mac.disconnect"), #selector(disconnectHost(_:)), in: submenu, id: host.id, tag: 6).image = menuImage("xmark.circle", L10n.text("mac.disconnect"))
            item(L10n.text("mac.host.settings"), #selector(hostSettings(_:)), in: submenu, id: host.id, tag: 7).image = menuImage("slider.horizontal.3", L10n.text("mac.host.settings"))
        }
        menu.addItem(.separator())
        item(L10n.text("mac.about"), #selector(showAbout), in: menu).image = menuImage("info.circle", "About")
        item(L10n.text("mac.quit"), #selector(quitFromMenu), in: menu).image = menuImage("power", L10n.text("mac.quit"))
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
            if rows.isEmpty { item.title = L10n.text("mac.no.forwarding.rules"); item.image = nil; item.toolTip = nil }
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
        languageMenuItem?.isEnabled = !terminating && !modalBlocked
        allDisconnectItem?.isEnabled = !terminating && !modalBlocked && manager.canDisconnectAll
        loginMenuItem?.title = loginOperationPending ? L10n.text("mac.launch.at.login.updating") : loginItem.menuTitle
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
            menu.item(withTag: 5)?.title = connected ? L10n.text("mac.reconnect") : (busy ? L10n.text("mac.connecting") : L10n.text("mac.connect"))
            menu.item(withTag: 5)?.image = menuImage(connected ? "arrow.clockwise" : "link", connected ? L10n.text("mac.reconnect") : L10n.text("mac.connect"))
            menu.item(withTag: 5)?.isEnabled = !busy && !terminating && !modalBlocked
            menu.item(withTag: 6)?.isHidden = !connected && !busy
            menu.item(withTag: 6)?.isEnabled = !terminating && !modalBlocked
        }
    }
    private func installApplicationMenu() {
        let menu = NSMenu(); let app = NSMenuItem(); let submenu = NSMenu()
        submenu.addItem(withTitle: L10n.text("mac.quit.localforwarder"), action: #selector(quitFromMenu), keyEquivalent: "q").target = self
        app.submenu = submenu; menu.addItem(app)
        let edit = NSMenuItem(); let edits = NSMenu(title: L10n.text("mac.edit"))
        for (title, action, key) in [(L10n.text("mac.cut"), #selector(NSText.cut(_:)), "x"), (L10n.text("mac.copy"), #selector(NSText.copy(_:)), "c"), (L10n.text("mac.paste"), #selector(NSText.paste(_:)), "v"), (L10n.text("mac.select.all"), #selector(NSText.selectAll(_:)), "a")] {
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
        let controller = AppDelegate(instance: instance, openConfiguration: false, store: store, languagePreferences: LanguagePreferences(load: { L10n.selection }, save: { _ in }, preferred: { ["en-US"] }))
        let manager = ConnectionManager(store: store, sessionFactory: { Session() })
        controller.manager = manager
        controller.launchReady = true
        controller.trayMenu.autoenablesItems = false
        controller.populateMenu(controller.trayMenu)
        let titles = controller.trayMenu.items.map(\.title)
        try NativeLayoutChecks.require(Array(titles.prefix(5)) == [L10n.text("mac.settings"), L10n.text("mac.language"), controller.loginItem.menuTitle, L10n.text("mac.reconnect.all"), L10n.text("mac.disconnect.all")], "global tray menu order: \(titles)")
        try NativeLayoutChecks.require(Array(titles.suffix(2)) == [L10n.text("mac.about"), L10n.text("mac.quit")], "About precedes quit")
        try NativeLayoutChecks.require(controller.allDisconnectItem?.isEnabled == false, "idle disconnect all disabled")
        let id = manager.hosts[0].id
        try manager.connect(id)
        controller.updateMenuPresentation()
        try NativeLayoutChecks.require(controller.allDisconnectItem?.isEnabled == true, "active disconnect all enabled")
        let original = try Data(contentsOf: store.file)
        let originalLanguage = L10n.selection
        guard let languages = controller.languageMenuItem?.submenu else { throw AppError("Missing language submenu") }
        try NativeLayoutChecks.require(languages.items.count == 9 && languages.items[1].isSeparatorItem,
                                       "automatic and separator precede seven languages")
        try NativeLayoutChecks.require(Array(languages.items.dropFirst(2)).map(\.title) ==
            ["Chinese (Simplified) — 简体中文", "Chinese (Traditional) — 繁體中文", "English", "German — Deutsch", "Japanese — 日本語", "Korean — 한국어", "Spanish — Español"], "fixed alphabetical language names")
        try controller.settings.checkLanguagePreservation {
            for choice in ["ko", "auto", originalLanguage] {
                let entry = NSMenuItem(); entry.representedObject = choice
                controller.changeLanguage(entry)
                controller.populateMenu(controller.trayMenu)
                let items = controller.languageMenuItem!.submenu!.items
                if choice == "ko" || choice == "auto" {
                    let expected = choice == "ko" ? "자동(시스템 언어)" : "Automatic (System Language)"
                    try NativeLayoutChecks.require(items.first?.title == expected, "localized automatic menu title")
                }
                let checked = items.filter { $0.state == .on }
                try NativeLayoutChecks.require(checked.count == 1 && checked[0].representedObject as? String == choice,
                                               "exactly the selected mode is checked")
                try NativeLayoutChecks.require(manager.runtime(id)?.state == .connected && manager.runtime(id)?.desiredConnected == true,
                                               "language switch preserves active SSH and connection intent")
            }
        }
        var failure: Error?
        func duringModal(_ action: @escaping () throws -> Void) {
            let timer = Timer(timeInterval: 0.05, repeats: false) { _ in
                MainActor.assumeIsolated {
                    do { try action() } catch { failure = error; controller.appModal?.cancel() }
                }
            }
            RunLoop.main.add(timer, forMode: .modalPanel)
        }
        var openedURLs: [URL] = []
        let about = AppModal(.about, openURL: { openedURLs.append($0) })
        duringModal {
            try NativeLayoutChecks.require(NSApp.modalWindow === about.window, "About is application modal")
            let blockedChoice = NSMenuItem(); blockedChoice.representedObject = originalLanguage == "en" ? "ko" : "en"
            controller.changeLanguage(blockedChoice)
            try NativeLayoutChecks.require(L10n.selection == originalLanguage, "modal blocks language selection")
            try NativeLayoutChecks.settle(about.window)
            let content = about.window.contentView!
            try NativeLayoutChecks.check(content.subviews.flatMap { NativeLayoutChecks.leaves($0) }, in: content, context: "About")
            try NativeLayoutChecks.require(about.labels.map(\.stringValue) == ["LocalForwarder", L10n.text("mac.version") + ProductVersion.current], "About product information without developer row")
            guard let license = about.licenseButton, let repository = about.linkButton else { throw AppError("About links missing") }
            try NativeLayoutChecks.require(license.title == "MIT License" && repository.title == AppModal.repositoryURL.absoluteString, "About link labels")
            let versionFrame = about.labels[1].convert(about.labels[1].bounds, to: content)
            let licenseFrame = license.convert(license.bounds, to: content)
            let repositoryFrame = repository.convert(repository.bounds, to: content)
            try NativeLayoutChecks.require(versionFrame.minY >= licenseFrame.maxY && licenseFrame.minY >= repositoryFrame.maxY, "About license follows version and precedes repository")
            try NativeLayoutChecks.require(openedURLs.isEmpty, "About opening must not open browser")
            license.performClick(nil)
            repository.performClick(nil)
            try NativeLayoutChecks.require(openedURLs == [AppModal.licenseURL, AppModal.repositoryURL], "About links open their respective URLs once")
            try NativeLayoutChecks.require(NSApp.modalWindow === about.window && about.window.isVisible, "About remains open after link clicks")
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
