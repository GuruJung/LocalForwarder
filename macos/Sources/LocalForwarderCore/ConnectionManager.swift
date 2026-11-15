import Foundation

public final class ConnectionCancellation {
    private var action: (() -> Void)?
    public init(_ cancel: @escaping () -> Void) { action = cancel }
    public func cancel() { action?(); action = nil }
}
public struct HostRuntime {
    public var state: ConnectionState = .disconnected
    public var executionHost: Host?
    public var desiredConnected = false
    public var temporary = false
    public var forwards: [ForwardRuntime] = []
    public var lastError = ""
    public var recentErrors: [DestinationError] = []
    public var summary: String {
        let title: String
        switch state { case .connected: title = "SSH 연결됨"; case .connecting: title = "연결 중"; case .stopping: title = "정리 중"; case .disconnected: title = "연결 끊김" }
        let ready = forwards.filter { $0.state == .ready }.count
        let failed = forwards.filter { $0.state == .failed }.count
        let unknown = forwards.filter { $0.state == .unknown }.count
        return "\(title) · 포워딩 준비 \(ready) / 실패 \(failed)" + (unknown > 0 ? " / 확인 불가 \(unknown)" : "") + (temporary ? " · 임시 실행 설정" : "")
    }
}
/// All API calls and callbacks execute on the main queue. Each entry has its own operation generation.
public final class ConnectionManager {
    private final class Entry {
        var host: Host
        var runtime = HostRuntime()
        var override: Host?
        var generation = 0
        var pending: Host?
        let session: SSHSessionProtocol
        init(host: Host, session: SSHSessionProtocol) { self.host = host; self.session = session }
    }
    public let store: ConfigurationStore
    public var onChange: (() -> Void)?
    public var onError: ((String, String) -> Void)?
    public var onExit: (() -> Void)?
    private var savedHosts: [Host] = []
    private var entries: [String: Entry] = [:]
    private var order: [String] = []
    private let factory: () -> SSHSessionProtocol
    private let schedule: (TimeInterval, @escaping () -> Void) -> ConnectionCancellation
    private var recovery: ConnectionCancellation?
    private var recoveryGeneration = 0
    private var available = true
    private var sleeping = false
    private var quitting = false
    private var barrierCount = 0
    private let outstandingStops = DispatchGroup()
    public init(store: ConfigurationStore, sshConfiguration: String? = nil,
                sessionFactory: (() -> SSHSessionProtocol)? = nil,
                schedule: @escaping (TimeInterval, @escaping () -> Void) -> ConnectionCancellation = ConnectionManager.scheduleOnMain) {
        self.store = store
        self.factory = sessionFactory ?? { SSHSession(sshConfiguration: sshConfiguration) }
        self.schedule = schedule
        try? reloadConfiguration()
    }
    public static func scheduleOnMain(_ delay: TimeInterval, _ action: @escaping () -> Void) -> ConnectionCancellation {
        let work = DispatchWorkItem(block: action)
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
        return ConnectionCancellation { work.cancel() }
    }
    public var hosts: [Host] {
        let savedIDs = Set(savedHosts.map(\.id))
        return savedHosts + order.filter { !savedIDs.contains($0) }.compactMap { entries[$0]?.host }
    }
    public var connectedCount: Int { entries.values.filter { $0.runtime.state == .connected }.count }
    public var disconnectedCount: Int { hosts.filter { $0.autoConnect && entries[$0.id]?.runtime.state != .connected }.count }
    public func runtime(_ id: String) -> HostRuntime? { entries[id]?.runtime }
    private func entry(_ host: Host) -> Entry {
        if let existing = entries[host.id] { return existing }
        let value = Entry(host: host, session: factory()); entries[host.id] = value; order.append(host.id)
        value.session.onChange = { [weak self, weak value] in
            guard let self, let value, self.entries[host.id] === value else { return }
            value.runtime.state = value.session.state
            value.runtime.forwards = value.session.forwards
            value.runtime.lastError = value.session.lastError
            value.runtime.recentErrors = value.session.recentErrors
            self.onChange?()
        }
        value.session.onError = { [weak self, weak value] message in
            guard let self, let value, self.entries[host.id] === value else { return }
            value.runtime.lastError = message; self.onError?(host.id, message)
        }
        return value
    }
    public func reloadConfiguration() throws {
        let configuration = try store.load()
        savedHosts = configuration.hosts
        for host in savedHosts { entry(host).host = host }
        onChange?()
    }
    public func startAll() throws { try reconnectAll() }
    public func reconnectAll() throws {
        guard !quitting else { return }
        // Validate all saved input before disrupting any healthy connection.
        let configuration = try store.load()
        cancelRecovery()
        savedHosts = configuration.hosts
        for host in savedHosts { entry(host).host = host }
        let saved = Dictionary(uniqueKeysWithValues: savedHosts.map { ($0.id, $0) })
        let current = Array(entries.values)
        barrierCount += 1
        let group = DispatchGroup()
        for value in current {
            value.generation += 1; value.override = nil; value.runtime.temporary = saved[value.host.id] == nil
            value.runtime.desiredConnected = saved[value.host.id]?.autoConnect == true
            value.pending = value.runtime.desiredConnected ? saved[value.host.id] : nil
            value.runtime.state = .stopping
            group.enter(); stop(value) { group.leave() }
        }
        onChange?()
        group.notify(queue: .main) { [weak self] in
            guard let self else { return }
            self.barrierCount -= 1
            self.launchPending(); self.onChange?()
        }
    }
    public func connect(_ id: String) throws {
        guard !quitting else { return }
        let host: Host
        if let temporary = entries[id]?.override { host = temporary }
        else if !savedHosts.contains(where: { $0.id == id }), let transient = entries[id] { host = transient.host }
        else { host = try store.host(id); updateSaved(host) }
        try connect(host, temporary: entries[id]?.override != nil || !savedHosts.contains(where: { $0.id == id }))
    }
    public func connect(_ host: Host, temporary: Bool) throws {
        guard !quitting else { return }
        try host.validate()
        let value = entry(host)
        value.generation += 1; let operation = value.generation
        value.host = host; value.override = temporary ? host : nil
        value.runtime.temporary = temporary; value.runtime.desiredConnected = true
        value.pending = host; value.runtime.state = .stopping
        stop(value) { [weak self, weak value] in
            guard let self, let value, self.entries[host.id] === value, operation == value.generation, !self.quitting else { return }
            self.launch(value)
        }
        onChange?()
    }
    /// Call only after a successful explicit save. The active process keeps its original snapshot.
    public func saved(_ host: Host) {
        updateSaved(host)
        let value = entry(host); value.host = host; value.override = nil; value.runtime.temporary = false
        onChange?()
    }
    private func updateSaved(_ host: Host) {
        if let index = savedHosts.firstIndex(where: { $0.id == host.id }) { savedHosts[index] = host }
        else { savedHosts.append(host) }
    }
    private func launch(_ value: Entry) {
        guard !quitting, barrierCount == 0, value.runtime.desiredConnected, let host = value.pending else { return }
        value.pending = nil; value.runtime.executionHost = host; value.runtime.state = .connecting
        value.runtime.lastError = ""; value.runtime.recentErrors = []; value.runtime.forwards = ForwardTracker(host: host).forwards
        value.session.start(host); onChange?()
    }
    private func launchPending() {
        guard barrierCount == 0 else { return }
        for value in Array(entries.values) { launch(value) }
    }
    private func stop(_ value: Entry, completion: @escaping () -> Void) {
        // The group outlives dictionary membership, so deleting a host cannot bypass final cleanup.
        let group = outstandingStops
        group.enter()
        value.session.stop { completion(); group.leave() }
    }
    public func disconnect(_ id: String) {
        guard !quitting, let value = entries[id] else { return }
        value.generation += 1; value.pending = nil; value.runtime.desiredConnected = false; value.runtime.state = .stopping
        stop(value) {}; onChange?()
    }
    public func delete(_ id: String) throws {
        guard !quitting else { return }
        // A damaged or unwritable saved file must not destroy an existing running connection.
        if savedHosts.contains(where: { $0.id == id }) { try store.delete(id) }
        disconnect(id)
        entries.removeValue(forKey: id); order.removeAll { $0 == id }; savedHosts.removeAll { $0.id == id }
        onChange?()
    }
    public func networkChanged(available: Bool) {
        let wasAvailable = self.available; self.available = available
        if !available { cancelRecovery() }
        else if !wasAvailable { scheduleRecovery() }
    }
    public func sleep() { sleeping = true; cancelRecovery() }
    public func wake() { sleeping = false; scheduleRecovery() }
    private func cancelRecovery() { recovery?.cancel(); recovery = nil; recoveryGeneration += 1 }
    private func scheduleRecovery() {
        cancelRecovery()
        guard !quitting, available, !sleeping else { return }
        let operation = recoveryGeneration
        let generations = entries.mapValues(\.generation)
        recovery = schedule(5) { [weak self] in
            guard let self, !self.quitting, self.available, !self.sleeping, operation == self.recoveryGeneration else { return }
            self.recovery = nil
            for (id, generation) in generations {
                guard let value = self.entries[id], value.generation == generation, value.runtime.desiredConnected, value.runtime.state == .disconnected else { continue }
                do {
                    guard let host = value.override ?? self.savedHosts.first(where: { $0.id == id }) else { continue }
                    try self.connect(host, temporary: value.override != nil)
                }
                catch { value.runtime.lastError = error.localizedDescription; self.onError?(id, error.localizedDescription) }
            }
            self.onChange?()
        }
    }
    public func quit() {
        guard !quitting else { return }
        quitting = true; cancelRecovery()
        for value in entries.values {
            value.generation += 1; value.pending = nil; value.runtime.desiredConnected = false
            stop(value) {}
        }
        outstandingStops.notify(queue: .main) { [self] in onExit?() }
        onChange?()
    }
}
