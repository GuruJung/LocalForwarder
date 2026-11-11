import XCTest
@testable import LocalForwarderCore

final class CoreTests: XCTestCase {
    func host() -> Host { Host(id: "fixture", name: "서버", target: "server", forwards: [Forward(id: "rule", localPort: 15432, remoteHost: "localhost", remotePort: 5432)]) }
    func testAutomaticRecoveryAndTerminalFailure() {
        var machine = ConnectionMachine()
        XCTAssertEqual(machine.begin(now: 0), [.stop, .launch]); machine.ready()
        XCTAssertEqual(machine.failed(now: 5), [.stop]); XCTAssertEqual(machine.state, .waitingReconnect)
        XCTAssertEqual(machine.tick(now: 64.9), [])
        XCTAssertEqual(machine.tick(now: 65), [.stop, .launch]); XCTAssertEqual(machine.state, .reconnecting)
        XCTAssertEqual(machine.failed(now: 66), [.stop]); XCTAssertEqual(machine.tick(now: 66.9), [])
        XCTAssertEqual(machine.tick(now: 67), [.launch])
        XCTAssertEqual(machine.failed(now: 68), [.stop]); XCTAssertEqual(machine.state, .waitingExit)
        XCTAssertEqual(machine.tick(now: 127.9), [])
        XCTAssertEqual(machine.tick(now: 128), [.stop, .exit]); XCTAssertEqual(machine.state, .exiting)
        XCTAssertEqual(machine.tick(now: 200), []); XCTAssertEqual(machine.manualReconnect(now: 201), [])
    }
    func testManualReconnectionReplacesAutomaticAndResetsAfterSuccess() {
        var machine = ConnectionMachine(); _ = machine.begin(now: 0); machine.ready(); _ = machine.failed(now: 2)
        XCTAssertEqual(machine.manualReconnect(now: 10), [.stop, .launch]); XCTAssertEqual(machine.state, .reconnecting)
        XCTAssertEqual(machine.manualReconnect(now: 11), [])
        machine.ready(); XCTAssertNil(machine.deadline)
        XCTAssertEqual(machine.tick(now: 62), []); _ = machine.failed(now: 100)
        XCTAssertEqual(machine.deadline?.time, 160)
        XCTAssertEqual(machine.quit(), [.stop, .exit]); XCTAssertNil(machine.deadline)
        XCTAssertEqual(machine.failed(now: 101), []); XCTAssertEqual(machine.tick(now: 200), [])
    }
    func testInitialFailureIsFatalAndTimeoutRetriesOnce() {
        var machine = ConnectionMachine(); _ = machine.begin(now: 0)
        XCTAssertEqual(machine.tick(now: 20), [.stop]); XCTAssertEqual(machine.tick(now: 21), [.launch])
        XCTAssertEqual(machine.tick(now: 41), [.stop, .fatal]); XCTAssertEqual(machine.state, .exiting)
    }
    func testWakeUsesElapsedDeadlineAndFailedManualStartsNewExitGrace() {
        var machine = ConnectionMachine(); _ = machine.begin(now: 0); machine.ready(); _ = machine.failed(now: 1)
        XCTAssertEqual(machine.tick(now: 600), [.stop, .launch]); XCTAssertEqual(machine.state, .reconnecting)
        _ = machine.failed(now: 601); _ = machine.tick(now: 602); _ = machine.failed(now: 603)
        _ = machine.manualReconnect(now: 650); _ = machine.failed(now: 651); _ = machine.tick(now: 652); _ = machine.failed(now: 653)
        XCTAssertEqual(machine.deadline?.time, 713); XCTAssertEqual(machine.tick(now: 714), [.stop, .exit])
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
        readiness.consume("debug1: Entering interactive session."); XCTAssertFalse(readiness.ready)
        readiness.consume("Authenticated to server ([127.0.0.1]:22) using publickey.")
        readiness.consume("debug1: remote forward success for: listen 9090, connect localhost:9090"); XCTAssertFalse(readiness.ready)
        readiness.consume("debug1: remote forward success for: listen 9091, connect localhost:9091"); XCTAssertTrue(readiness.ready)
        var failed = Readiness(); failed.consume("Could not request local forwarding.")
        failed.consume("Authenticated to server"); failed.consume("Entering interactive session"); XCTAssertFalse(failed.ready)
    }
    func testInheritedRulesDoNotIncludeOrdinarySettings() {
        XCTAssertEqual(SSHCommand.inheritedForwards("hostname a\nlocalforward 1234 localhost:22\nremoteforward 4567 localhost:33\ndynamicforward 1080\n" ).count, 3)
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
