import XCTest
@testable import LocalForwarderCore

private final class TestSession: SSHSessionProtocol {
    var state: ConnectionState = .disconnected
    var lastError = ""
    var forwards: [ForwardRuntime] = []
    var onChange: (() -> Void)?
    var onError: ((String) -> Void)?
    var starts: [Host] = []
    var stopCount = 0
    var holdStops = false
    var completions: [() -> Void] = []
    func start(_ host: Host) { starts.append(host); state = .connecting; onChange?() }
    func stop(completion: @escaping () -> Void) {
        stopCount += 1
        if holdStops { state = .stopping; onChange?(); completions.append(completion) }
        else { state = .disconnected; onChange?(); completion() }
    }
    func finishStops() {
        let pending = completions; completions = []
        state = .disconnected; onChange?()
        for completion in pending { completion() }
    }
    func connected() { state = .connected; onChange?() }
    func lost() { state = .disconnected; lastError = "connection lost"; onChange?() }
}

private final class TestClock {
    final class Job {
        let time: TimeInterval
        let action: () -> Void
        var cancelled = false
        init(time: TimeInterval, action: @escaping () -> Void) { self.time = time; self.action = action }
    }
    var now: TimeInterval = 0
    var jobs: [Job] = []
    func schedule(_ delay: TimeInterval, _ action: @escaping () -> Void) -> ConnectionCancellation {
        let job = Job(time: now + delay, action: action); jobs.append(job)
        return ConnectionCancellation { job.cancelled = true }
    }
    func advance(to target: TimeInterval) {
        while let next = jobs.filter({ !$0.cancelled && $0.time <= target }).min(by: { $0.time < $1.time }) {
            next.cancelled = true; now = next.time; next.action()
        }
        now = target
    }
}

private final class ManagerFixture {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    let clock = TestClock()
    var sessions: [TestSession] = []
    let store: ConfigurationStore
    var manager: ConnectionManager!
    init(count: Int = 3, disabledLast: Bool = false) throws {
        store = ConfigurationStore(directory: directory)
        for index in 0..<count {
            let host = Host(id: "host-\(index)", name: "Host \(index)", target: "server-\(index)",
                            forwards: [Forward(id: "rule", localPort: 15000 + index)],
                            autoConnect: !(disabledLast && index == count - 1))
            try store.save(host)
        }
        manager = ConnectionManager(store: store, sessionFactory: { [unowned self] in
            let session = TestSession(); self.sessions.append(session); return session
        }, schedule: { [unowned self] delay, action in self.clock.schedule(delay, action) })
    }
    deinit { try? FileManager.default.removeItem(at: directory) }
}

final class ManagerTests: XCTestCase {
    private func drainMainQueue() {
        let done = expectation(description: "main queue barrier")
        DispatchQueue.main.async { done.fulfill() }
        wait(for: [done], timeout: 2)
    }
    func testStartIntentDisabledManualUseAndDisconnectSurviveRecovery() throws {
        let fixture = try ManagerFixture(disabledLast: true)
        let manager = fixture.manager!
        try manager.startAll(); drainMainQueue()
        XCTAssertEqual(fixture.sessions.map { $0.starts.count }, [1, 1, 0])
        XCTAssertEqual(manager.runtime("host-2")?.desiredConnected, false)
        try manager.connect("host-2"); fixture.sessions[2].connected()
        XCTAssertEqual(manager.connectedCount, 1)
        XCTAssertEqual(try fixture.store.host("host-2").autoConnect, false)
        XCTAssertEqual(manager.runtime("host-2")?.desiredConnected, true)
        manager.disconnect("host-0")
        fixture.sessions[1].lost(); manager.wake(); fixture.clock.advance(to: 5)
        XCTAssertEqual(fixture.sessions.map { $0.starts.count }, [1, 2, 1])
        XCTAssertEqual(manager.runtime("host-0")?.desiredConnected, false)
        XCTAssertTrue(try fixture.store.host("host-0").autoConnect)
    }
    func testRecoveryCoalescesAtFiveSecondsAndOnlyTouchesDisconnected() throws {
        let fixture = try ManagerFixture()
        let manager = fixture.manager!
        try manager.startAll(); drainMainQueue()
        fixture.sessions[0].connected(); fixture.sessions[1].lost()
        // host-2 remains connecting and must not be replaced.
        manager.networkChanged(available: true)
        XCTAssertTrue(fixture.clock.jobs.isEmpty, "Initial online observation is not a recovery event")
        manager.networkChanged(available: false); manager.networkChanged(available: true)
        fixture.clock.advance(to: 2); manager.wake()
        fixture.clock.advance(to: 6.999)
        XCTAssertEqual(fixture.sessions.map { $0.starts.count }, [1, 1, 1])
        fixture.clock.advance(to: 7)
        XCTAssertEqual(fixture.sessions.map { $0.starts.count }, [1, 2, 1])
        fixture.sessions[0].lost(); fixture.clock.advance(to: 500)
        XCTAssertEqual(fixture.sessions[0].starts.count, 1, "Late disconnection waits for another event")
    }
    func testOfflineSleepAndManualWorkInvalidatePendingRecovery() throws {
        let fixture = try ManagerFixture(count: 1)
        let manager = fixture.manager!
        try manager.startAll(); drainMainQueue(); fixture.sessions[0].lost()
        manager.wake(); let stale = try XCTUnwrap(fixture.clock.jobs.last)
        fixture.clock.advance(to: 2); manager.networkChanged(available: false)
        stale.action(); fixture.clock.advance(to: 10)
        XCTAssertEqual(fixture.sessions[0].starts.count, 1)
        manager.networkChanged(available: true); manager.sleep(); fixture.clock.advance(to: 20)
        XCTAssertEqual(fixture.sessions[0].starts.count, 1)
        manager.wake(); manager.disconnect("host-0"); fixture.clock.advance(to: 25)
        XCTAssertEqual(fixture.sessions[0].starts.count, 1)
        try manager.connect("host-0"); fixture.sessions[0].lost(); manager.wake()
        fixture.clock.advance(to: 27); try manager.connect("host-0"); fixture.sessions[0].lost()
        fixture.clock.advance(to: 30)
        XCTAssertEqual(fixture.sessions[0].starts.count, 3, "A failed manual attempt during the wait also wins")
    }
    func testCancelledStartupHasNoRecoveryIntent() throws {
        let fixture = try ManagerFixture()
        fixture.manager.wake(); fixture.clock.advance(to: 5)
        XCTAssertTrue(fixture.sessions.allSatisfy { $0.starts.isEmpty })
        XCTAssertTrue(fixture.manager.hosts.allSatisfy { fixture.manager.runtime($0.id)?.desiredConnected == false })
    }
    func testTemporaryExecutionSurvivesReconnectAndSavedDoesNotDisruptActiveSession() throws {
        let fixture = try ManagerFixture(count: 1)
        let manager = fixture.manager!
        let saved = try fixture.store.host("host-0")
        var temporary = saved; temporary.target = "temporary-server"
        try manager.connect(temporary, temporary: true); fixture.sessions[0].connected()
        try manager.connect("host-0")
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "temporary-server")
        fixture.sessions[0].lost(); manager.wake(); fixture.clock.advance(to: 5)
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "temporary-server")
        XCTAssertEqual(try fixture.store.host(saved.id), saved)
        fixture.sessions[0].connected()
        let stops = fixture.sessions[0].stopCount
        var updated = saved; updated.target = "saved-new-server"
        try fixture.store.save(updated); manager.saved(updated)
        XCTAssertEqual(fixture.sessions[0].stopCount, stops)
        XCTAssertEqual(manager.runtime(saved.id)?.executionHost?.target, "temporary-server")
        XCTAssertEqual(manager.runtime(saved.id)?.temporary, false)
        fixture.sessions[0].lost(); manager.wake(); fixture.clock.advance(to: 10)
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "saved-new-server")
    }
    func testRecoveryUsesRememberedConfigurationAndManualConnectReloadsDisk() throws {
        let fixture = try ManagerFixture(count: 1)
        let manager = fixture.manager!
        try manager.startAll(); drainMainQueue()
        var external = try fixture.store.host("host-0"); external.target = "external-change"
        try fixture.store.save(external)
        fixture.sessions[0].lost(); manager.wake(); fixture.clock.advance(to: 5)
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "server-0")
        try manager.connect("host-0")
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "external-change")
    }
    func testFullReconnectResetsTemporaryAndDisabledIntent() throws {
        let fixture = try ManagerFixture(count: 2, disabledLast: true)
        let manager = fixture.manager!
        var changed = try fixture.store.host("host-0"); changed.target = "temporary"
        try manager.connect(changed, temporary: true)
        try manager.connect("host-1")
        let unsaved = Host(id: "unsaved", name: "Unsaved", target: "server")
        try manager.connect(unsaved, temporary: true)
        XCTAssertEqual(manager.hosts.count, 3)
        try manager.reconnectAll(); drainMainQueue()
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "server-0")
        XCTAssertEqual(manager.runtime("host-0")?.temporary, false)
        XCTAssertEqual(manager.runtime("host-1")?.desiredConnected, false)
        XCTAssertEqual(fixture.sessions[1].starts.count, 1)
        XCTAssertEqual(fixture.sessions[2].starts.count, 1)
        XCTAssertTrue(manager.hosts.contains { $0.id == "unsaved" })
        XCTAssertEqual(manager.runtime("unsaved")?.desiredConnected, false)
    }
    func testInvalidConnectionAndDamagedDeleteKeepExistingSession() throws {
        let fixture = try ManagerFixture(count: 1)
        let manager = fixture.manager!
        try manager.connect("host-0"); fixture.sessions[0].connected()
        let stops = fixture.sessions[0].stopCount
        var invalid = try fixture.store.host("host-0"); invalid.target = "-oBad"
        XCTAssertThrowsError(try manager.connect(invalid, temporary: true))
        XCTAssertEqual(fixture.sessions[0].stopCount, stops)
        let damaged = Data("{broken".utf8); try damaged.write(to: fixture.store.file)
        XCTAssertThrowsError(try manager.delete("host-0"))
        XCTAssertThrowsError(try manager.reconnectAll())
        XCTAssertEqual(fixture.sessions[0].stopCount, stops)
        XCTAssertEqual(manager.runtime("host-0")?.state, .connected)
        XCTAssertEqual(try Data(contentsOf: fixture.store.file), damaged)
    }
    func testDisconnectAndDeleteInvalidateDelayedStopCompletions() throws {
        let fixture = try ManagerFixture(count: 1)
        let manager = fixture.manager!
        fixture.sessions[0].holdStops = true
        try manager.connect("host-0")
        manager.disconnect("host-0")
        fixture.sessions[0].finishStops()
        XCTAssertTrue(fixture.sessions[0].starts.isEmpty)
        try manager.connect("host-0"); try manager.delete("host-0")
        fixture.sessions[0].finishStops()
        XCTAssertTrue(fixture.sessions[0].starts.isEmpty)
        XCTAssertNil(manager.runtime("host-0")); XCTAssertTrue(try fixture.store.load().hosts.isEmpty)
    }
    func testDisconnectAllPreservesSettingsAndTemporaryExecution() throws {
        let fixture = try ManagerFixture(disabledLast: true)
        let manager = fixture.manager!
        XCTAssertFalse(manager.canDisconnectAll)
        try manager.startAll(); drainMainQueue()
        try manager.connect("host-2")
        var temporary = try fixture.store.host("host-0"); temporary.target = "temporary"
        try manager.connect(temporary, temporary: true)
        try manager.connect(Host(id: "unsaved", name: "Unsaved", target: "transient"), temporary: true)
        fixture.sessions[0].connected(); fixture.sessions[1].lost()
        manager.wake(); let stale = try XCTUnwrap(fixture.clock.jobs.last)
        let bytes = try Data(contentsOf: fixture.store.file)
        let starts = fixture.sessions.map { $0.starts.count }
        XCTAssertTrue(manager.canDisconnectAll)
        manager.disconnectAll(); manager.disconnectAll(); drainMainQueue()
        stale.action(); manager.wake(); manager.networkChanged(available: false)
        manager.networkChanged(available: true); fixture.clock.advance(to: 100)
        XCTAssertEqual(fixture.sessions.map { $0.starts.count }, starts)
        XCTAssertTrue(manager.hosts.allSatisfy { manager.runtime($0.id)?.state == .disconnected && manager.runtime($0.id)?.desiredConnected == false })
        XCTAssertFalse(manager.canDisconnectAll)
        XCTAssertEqual(try Data(contentsOf: fixture.store.file), bytes)
        XCTAssertEqual(manager.hosts.count, 4)
        XCTAssertEqual(manager.runtime("host-0")?.temporary, true)
        try manager.connect("host-0")
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "temporary")
        try manager.connect("unsaved")
        XCTAssertEqual(fixture.sessions[3].starts.last?.target, "transient")
        manager.disconnectAll(); drainMainQueue()
        try manager.reconnectAll(); drainMainQueue()
        XCTAssertEqual(fixture.sessions[0].starts.last?.target, "server-0")
        XCTAssertFalse(manager.runtime("host-2")!.desiredConnected)
        XCTAssertFalse(manager.runtime("unsaved")!.desiredConnected)
    }
    func testDisconnectAllInvalidatesFullReconnectAndPendingConnect() throws {
        let fixture = try ManagerFixture(count: 2)
        let manager = fixture.manager!
        try manager.startAll(); drainMainQueue()
        for session in fixture.sessions { session.holdStops = true }
        try manager.reconnectAll()
        try manager.connect("host-0")
        manager.disconnectAll()
        XCTAssertTrue(manager.isDisconnectingAll)
        XCTAssertFalse(manager.canDisconnectAll)
        fixture.sessions[0].finishStops(); drainMainQueue()
        XCTAssertTrue(manager.isDisconnectingAll)
        fixture.sessions[1].finishStops(); drainMainQueue()
        XCTAssertFalse(manager.isDisconnectingAll)
        XCTAssertEqual(fixture.sessions.map { $0.starts.count }, [1, 1])
        var exited = false; manager.onExit = { exited = true }
        manager.quit()
        for session in fixture.sessions { session.finishStops() }
        drainMainQueue(); XCTAssertTrue(exited)
    }
    func testDisconnectAllDoesNotReadDamagedConfigurationAndHandlesEmptyHosts() throws {
        let empty = try ManagerFixture(count: 0)
        empty.manager.disconnectAll()
        XCTAssertFalse(empty.manager.isDisconnectingAll)
        let fixture = try ManagerFixture(count: 1)
        try fixture.manager.connect("host-0")
        let damaged = Data("{broken".utf8); try damaged.write(to: fixture.store.file)
        fixture.manager.disconnectAll(); drainMainQueue()
        XCTAssertEqual(fixture.manager.runtime("host-0")?.state, .disconnected)
        XCTAssertEqual(try Data(contentsOf: fixture.store.file), damaged)
    }
    func testQuitAlsoWaitsForDeletedHostCleanup() throws {
        let fixture = try ManagerFixture(count: 1)
        let manager = fixture.manager!
        try manager.connect("host-0"); fixture.sessions[0].connected()
        fixture.sessions[0].holdStops = true
        try manager.delete("host-0")
        XCTAssertNil(manager.runtime("host-0"))
        var didExit = false; manager.onExit = { didExit = true }
        manager.quit(); drainMainQueue(); XCTAssertFalse(didExit)
        fixture.sessions[0].finishStops(); drainMainQueue(); XCTAssertTrue(didExit)
    }
    func testQuitWaitsForEverySessionAndCancelsRecovery() throws {
        let fixture = try ManagerFixture(count: 2)
        let manager = fixture.manager!
        try manager.startAll(); drainMainQueue()
        fixture.sessions[0].lost(); manager.wake()
        for session in fixture.sessions { session.holdStops = true }
        var didExit = false; manager.onExit = { didExit = true }
        manager.quit(); fixture.clock.advance(to: 100); drainMainQueue()
        XCTAssertFalse(didExit)
        fixture.sessions[0].finishStops(); drainMainQueue(); XCTAssertFalse(didExit)
        fixture.sessions[1].finishStops(); drainMainQueue(); XCTAssertTrue(didExit)
        XCTAssertEqual(fixture.sessions.map { $0.starts.count }, [1, 1])
    }
}
