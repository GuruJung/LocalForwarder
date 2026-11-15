import XCTest
@testable import LocalForwarderCore

typealias Host = LocalForwarderCore.Host

final class CoreTests: XCTestCase {
    func host() -> Host { Host(id: "fixture", name: "서버", target: "server", forwards: [Forward(id: "rule", localPort: 15432, remoteHost: "localhost", remotePort: 5432)]) }
    func testDisconnectedSessionNeverSchedulesRecoveryOrExit() {
        var machine = ConnectionMachine()
        XCTAssertEqual(machine.begin(now: 0), [.stop, .launch]); machine.ready()
        XCTAssertEqual(machine.failed(now: 5, retryable: true), [.stop])
        XCTAssertEqual(machine.state, .disconnected); XCTAssertNil(machine.deadline)
        XCTAssertEqual(machine.tick(now: 60), []); XCTAssertEqual(machine.tick(now: 3600), [])
        XCTAssertEqual(machine.manualReconnect(now: 3601), [.stop, .launch])
        XCTAssertEqual(machine.manualReconnect(now: 3602), [])
    }
    func testOnlyClassifiedPreConnectionFailureRetriesOnce() {
        var machine = ConnectionMachine(); _ = machine.begin(now: 0)
        XCTAssertEqual(machine.failed(now: 2, retryable: true), [.stop])
        XCTAssertEqual(machine.state, .connecting)
        XCTAssertEqual(machine.tick(now: 2.999), [])
        XCTAssertEqual(machine.tick(now: 3), [.launch])
        XCTAssertEqual(machine.failed(now: 4, retryable: true), [.stop])
        XCTAssertEqual(machine.state, .disconnected); XCTAssertNil(machine.deadline)
        XCTAssertEqual(machine.tick(now: 100), [])
        _ = machine.manualReconnect(now: 101)
        XCTAssertEqual(machine.failed(now: 102), [.stop])
        XCTAssertEqual(machine.state, .disconnected); XCTAssertNil(machine.deadline)
    }
    func testTimeoutDoesNotRetryAndQuitCancelsPendingAttempt() {
        var machine = ConnectionMachine(); _ = machine.begin(now: 0)
        XCTAssertEqual(machine.tick(now: 19.999), [])
        XCTAssertEqual(machine.tick(now: 20), [.stop])
        XCTAssertEqual(machine.state, .disconnected); XCTAssertEqual(machine.tick(now: 21), [])
        _ = machine.begin(now: 30); _ = machine.failed(now: 31, retryable: true)
        XCTAssertEqual(machine.quit(), [.stop]); XCTAssertNil(machine.deadline)
        XCTAssertEqual(machine.tick(now: 600), []); XCTAssertEqual(machine.failed(now: 601), [])
        machine.stopped(); XCTAssertEqual(machine.state, .disconnected)
    }
    func testV1ReadIsUnchangedAndFirstSavePreservesExactBackup() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let store = ConfigurationStore(directory: directory)
        let original = Data("{ \"version\": 1, \"hosts\": [{\"id\":\"old\",\"name\":\"old\",\"target\":\"server\",\"forwards\":[]}] }\n".utf8)
        try original.write(to: store.file)
        let loaded = try store.load()
        XCTAssertEqual(loaded.version, 1)
        XCTAssertTrue(loaded.hosts[0].autoConnect)
        XCTAssertTrue(loaded.hosts[0].exitOnForwardFailure)
        XCTAssertTrue(loaded.hosts[0].retryOnConnectionRefused)
        XCTAssertEqual(try Data(contentsOf: store.file), original)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path), ["config.json"])
        var changed = loaded.hosts[0]; changed.autoConnect = false
        try store.save(changed)
        XCTAssertEqual(try store.load().version, 2)
        XCTAssertFalse(try store.host("old").autoConnect)
        let backups = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
            .filter { $0.lastPathComponent.hasPrefix("config-v1-") }
        XCTAssertEqual(backups.count, 1)
        XCTAssertEqual(try Data(contentsOf: XCTUnwrap(backups.first)), original)
        try store.save(changed)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path).filter { $0.hasPrefix("config-v1-") }.count, 1)
        let permissions = try FileManager.default.attributesOfItem(atPath: store.file.path)[.posixPermissions] as? NSNumber
        XCTAssertEqual(permissions?.intValue, 0o600)
    }
    func testUnsupportedConfigurationAndInvalidSavePreserveOriginal() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let store = ConfigurationStore(directory: directory)
        let unsupported = Data("{\"version\":99,\"hosts\":[]}".utf8)
        try unsupported.write(to: store.file)
        XCTAssertThrowsError(try store.load()); XCTAssertThrowsError(try store.save(host()))
        XCTAssertEqual(try Data(contentsOf: store.file), unsupported)
        try FileManager.default.removeItem(at: store.file)
        try store.save(host()); let valid = try Data(contentsOf: store.file)
        var invalid = host(); invalid.target = "-oBad"
        XCTAssertThrowsError(try store.save(invalid))
        XCTAssertEqual(try Data(contentsOf: store.file), valid)
    }
    func testConcurrentDistinctHostsAndIdentityUpdatePreserveOtherFields() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(directory: directory)
        let lock = NSLock(); var failures: [String] = []
        DispatchQueue.concurrentPerform(iterations: 24) { index in
            do {
                var value = self.host(); value.id = "host-\(index)"; value.name = "name-\(index)"
                try ConfigurationStore(directory: directory).save(value)
            } catch { lock.lock(); failures.append(error.localizedDescription); lock.unlock() }
        }
        XCTAssertTrue(failures.isEmpty, failures.joined(separator: "\n"))
        XCTAssertEqual(try store.load().hosts.count, 24)
        var changed = try store.host("host-0"); changed.name = "latest edit"; changed.autoConnect = false
        try store.save(changed)
        try ConfigurationStore(directory: directory).setIdentity("/tmp/new-key", for: changed.id)
        let merged = try store.host(changed.id)
        XCTAssertEqual(merged.name, "latest edit"); XCTAssertFalse(merged.autoConnect)
        XCTAssertEqual(merged.identityFile, "/tmp/new-key")
        XCTAssertEqual(try store.load().hosts.count, 24)
    }
    func testDiskRereadPreservesOtherHostsAndDamagedFiles() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let one = ConfigurationStore(directory: directory); let two = ConfigurationStore(directory: directory)
        XCTAssertEqual(try one.load().hosts.count, 0)
        var first = host(); try one.save(first)
        var second = host(); second.id = "second"; second.name = "다른 서버"; try two.save(second)
        first.name = "새 이름"; try one.save(first)
        XCTAssertEqual(try two.host("fixture").name, "새 이름"); XCTAssertEqual(try one.load().hosts.count, 2)
        try two.delete("fixture"); XCTAssertThrowsError(try one.host("fixture"))
        let damaged = Data("{broken".utf8); try damaged.write(to: one.file)
        XCTAssertThrowsError(try one.save(first)); XCTAssertEqual(try Data(contentsOf: one.file), damaged)
    }
    func testConcurrentSavesRemainValidAtSpecialCharacterPath() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("Local Forwarder 한글 '\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(directory: directory)
        var initial = host()
        initial.identityFile = directory.appendingPathComponent("개인 키's identity").path
        try store.save(initial)
        let baseline = initial
        final class Failures {
            private let lock = NSLock()
            private var messages: [String] = []
            func add(_ message: String) { lock.lock(); defer { lock.unlock() }; messages.append(message) }
            func read() -> [String] { lock.lock(); defer { lock.unlock() }; return messages }
        }
        let failures = Failures()
        DispatchQueue.concurrentPerform(iterations: 32) { index in
            do {
                let writer = ConfigurationStore(directory: directory)
                var update = baseline; update.name = "writer-\(index)"
                try writer.save(update)
                let snapshot = try JSONDecoder().decode(Configuration.self, from: Data(contentsOf: writer.file))
                if snapshot.version != 2 || snapshot.hosts.count != 1 || snapshot.hosts[0].id != baseline.id {
                    failures.add("동시 저장 중 유효하지 않은 설정을 읽었습니다.")
                }
            } catch { failures.add(error.localizedDescription) }
        }
        XCTAssertTrue(failures.read().isEmpty, failures.read().joined(separator: "\n"))
        let concurrentResult = try store.host(baseline.id)
        XCTAssertTrue(concurrentResult.name.hasPrefix("writer-"))
        XCTAssertEqual(concurrentResult.identityFile, baseline.identityFile)
        var last = baseline; last.name = "마지막 저장"
        try ConfigurationStore(directory: directory).save(last)
        let fresh = try ConfigurationStore(directory: directory).host(baseline.id)
        XCTAssertEqual(fresh, last)
        XCTAssertTrue(SSHCommand.connect(fresh).contains(baseline.identityFile!))
    }
    func testCommandArgumentsAndDisabledForward() throws {
        var value = host(); value.user = "alice"; value.port = 2222; value.identityFile = "/tmp/한 글 key"
        value.forwards.append(Forward(id: "disabled", enabled: false, localPort: 1234, remoteHost: "::1", remotePort: 99))
        let args = SSHCommand.connect(value, configuration: "/tmp/test config")
        XCTAssertEqual(args.last, "server"); XCTAssertEqual(args.filter { $0 == "-L" }.count, 1)
        XCTAssertTrue(args.contains("/tmp/한 글 key")); XCTAssertTrue(args.contains("127.0.0.1:15432:localhost:5432"))
        XCTAssertTrue(args.contains("BatchMode=yes")); XCTAssertTrue(args.contains("ControlPath=none"))
        XCTAssertEqual(SSHCommand.forwarding(value.forwards[1]), "127.0.0.1:1234:[::1]:99")
        XCTAssertEqual(SSHCommand.shellQuote("a'b"), "'a'\\''b'")
    }
    func testValidationRejectsInjectionAndDuplicateActivePorts() throws {
        var value = host(); try value.validate()
        for target in ["-oProxyCommand=x", "a\nb", "a b", "a;touch", "$(id)", "a`id`", "a'quoted"] {
            value.target = target; XCTAssertThrowsError(try value.validate())
        }
        value = host(); value.forwards.append(Forward(id: "two", localPort: 15432)); XCTAssertThrowsError(try value.validate())
        value.forwards[1].enabled = false; try value.validate()
        value.id = "../escape"; XCTAssertThrowsError(try value.validate())
    }
    func testReadinessWaitsForRemoteAcknowledgementsAndRejectsFailure() {
        var readiness = Readiness(remoteExpected: 2)
        readiness.consume("Authenticated to server ([127.0.0.1]:22) using publickey.")
        readiness.consume("debug1: Entering interactive session."); XCTAssertFalse(readiness.ready)
        readiness.consume("debug1: remote forward success for: listen 9090, connect localhost:9090"); XCTAssertFalse(readiness.ready)
        readiness.consume("debug1: remote forward success for: listen 9091, connect localhost:9091"); XCTAssertTrue(readiness.ready)
        var failed = Readiness(); failed.consume("Could not request local forwarding.")
        failed.consume("Authenticated to server"); failed.consume("Local forwarding listening on 127.0.0.1 port 9090.")
        failed.consume("Entering interactive session"); XCTAssertFalse(failed.ready)
    }
    func testJumpSessionDoesNotEstablishTargetReadiness() {
        var readiness = Readiness()
        readiness.consume("Authenticated to jump ([127.0.0.1]:22) using publickey.")
        readiness.consume("debug1: channel_connect_stdio_fwd: target:22")
        readiness.consume("debug1: Entering interactive session.")
        XCTAssertFalse(readiness.ready, "The ProxyJump -W child has no forwarding listeners")
        readiness.consume("Authenticated to target ([127.0.0.1]:23) using publickey.")
        readiness.consume("debug1: Local forwarding listening on 127.0.0.1 port 15432.")
        XCTAssertFalse(readiness.ready, "The jump child's earlier session entry must be reset")
        readiness.consume("debug1: Entering interactive session.")
        XCTAssertTrue(readiness.ready)
    }
    func testAuthenticationResetsEarlierRemoteAcknowledgements() {
        var readiness = Readiness(remoteExpected: 1)
        readiness.consume("Authenticated to jump")
        readiness.consume("debug1: Entering interactive session.")
        readiness.consume("Authenticated to target")
        readiness.consume("debug1: Entering interactive session.")
        XCTAssertFalse(readiness.ready)
        readiness.consume("debug1: remote forward success for: listen 9090, connect localhost:9090")
        XCTAssertTrue(readiness.ready)
        readiness.consume("Authenticated to another-target")
        XCTAssertFalse(readiness.ready)
        XCTAssertEqual(readiness.remoteSucceeded, 0)
        XCTAssertFalse(readiness.enteredSession)
        XCTAssertFalse(readiness.localListener)
    }
    func testSSHReadinessSeparatesFailedForwardsAndFiltersJumpAuthentication() {
        var readiness = Readiness(requireForwards: false, expectedTarget: "127.0.0.1", expectedPort: 2222)
        readiness.consume("Authenticated to 127.0.0.1 ([127.0.0.1]:22) using publickey.")
        readiness.consume("debug1: Entering interactive session.")
        XCTAssertFalse(readiness.ready, "The jump child can have the same hostname but a different port")
        readiness.consume("Authenticated to 127.0.0.1 ([127.0.0.1]:2222) using publickey.")
        readiness.consume("Could not request local forwarding.")
        XCTAssertFalse(readiness.ready)
        readiness.consume("debug1: Entering interactive session.")
        XCTAssertTrue(readiness.ready, "Live SSH with ExitOnForwardFailure OFF is independent of forward results")
    }
    func testOnlyTransportRefusalClassificationPermitsRetry() {
        for log in ["ssh: connect to host server port 22: Connection refused",
                    "kex_exchange_identification: Connection closed by remote host",
                    "Connection reset by 127.0.0.1 port 22"] {
            XCTAssertTrue(SSHCommand.isConnectionRefusal(log), log)
        }
        for log in ["Permission denied (publickey).", "Host key verification failed.",
                    "bind [127.0.0.1]:15432: Address already in use", "Connection timed out",
                    "channel 2: open failed: connect failed: Connection refused",
                    "Authenticated to server\nConnection closed by server", "unclassified failure"] {
            XCTAssertFalse(SSHCommand.isConnectionRefusal(log), log)
        }
        var value = host(); value.exitOnForwardFailure = false
        XCTAssertTrue(SSHCommand.connect(value).contains("ExitOnForwardFailure=no"))
        XCTAssertTrue(SSHCommand.inspect(value).contains("ExitOnForwardFailure=no"))
    }
    func testForwardRuntimePreservesPartialFailureSourceAndObservedDestinationError() {
        var value = host()
        value.forwards.append(Forward(id: "bad", localPort: 15433))
        value.forwards.append(Forward(id: "disabled", enabled: false, localPort: 15434))
        var tracker = ForwardTracker(host: value, inherited: ["remoteforward 19090 [localhost]:9090", "dynamicforward /tmp/socket"])
        tracker.consume("debug1: Local forwarding listening on 127.0.0.1 port 15432.")
        tracker.consume("bind [127.0.0.1]:15433: Address already in use")
        tracker.consume("debug1: remote forward success for: listen 19090, connect localhost:9090")
        tracker.sessionReady()
        XCTAssertEqual(tracker.forwards.map(\.state), [.ready, .failed, .ready, .unknown])
        XCTAssertEqual(tracker.forwards[0].source, "앱 설정")
        XCTAssertEqual(tracker.forwards[2].source, "SSH 설정")
        let observed = Date(timeIntervalSince1970: 123)
        tracker.consume("channel 1: open failed: connect failed: Connection refused", now: observed)
        XCTAssertEqual(tracker.recentErrors.first?.time, observed)
        XCTAssertEqual(tracker.forwards[0].state, .ready, "Destination failure does not undo listener readiness")
        tracker.disconnected()
        XCTAssertTrue(tracker.forwards.allSatisfy { $0.state == .disconnected })
        XCTAssertEqual(tracker.recentErrors.count, 1)
    }
    func testInheritedRulesDoNotIncludeOrdinarySettings() {
        XCTAssertEqual(SSHCommand.inheritedForwards("hostname a\nlocalforward 1234 localhost:22\nremoteforward 4567 localhost:33\ndynamicforward 1080\n" ).count, 3)
    }
    func testCapturedCommandTimeoutHasBoundedCleanup() throws {
        let start = ContinuousTime.now()
        let result = try ProcessTools.capture("/bin/sleep", ["120"], timeout: 0.05)
        XCTAssertNotEqual(result.status, 0)
        XCTAssertLessThan(ContinuousTime.now() - start, 3)
    }
    func testPublicKeyInstallerPreservesRestrictedExistingEntry() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let ssh = directory.appendingPathComponent(".ssh"); try FileManager.default.createDirectory(at: ssh, withIntermediateDirectories: true)
        let file = ssh.appendingPathComponent("authorized_keys")
        let original = "# retained\nrestrict ssh-ed25519 AAAATEST test-comment\n"
        try original.write(to: file, atomically: true, encoding: .utf8)
        let child = Process(); child.executableURL = URL(fileURLWithPath: "/bin/sh")
        child.arguments = ["-c", AuthenticationSetup.installCommand]
        var environment = ProcessInfo.processInfo.environment; environment["HOME"] = directory.path; child.environment = environment
        let input = Pipe(); child.standardInput = input; child.standardOutput = FileHandle.nullDevice; child.standardError = FileHandle.nullDevice
        try child.run(); input.fileHandleForWriting.write(Data("ssh-ed25519 AAAATEST new-comment\n".utf8)); try input.fileHandleForWriting.close(); child.waitUntilExit()
        XCTAssertEqual(child.terminationStatus, 0)
        XCTAssertEqual(try String(contentsOf: file), original)
    }
}
