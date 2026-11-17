import XCTest
@testable import LocalForwarderCore

final class SettingsPresentationTests: XCTestCase {
    private func host() -> LocalForwarderCore.Host {
        Host(id: "host", name: "서버", target: "server", forwards: [
            Forward(id: "one", localPort: 18080, remoteHost: "localhost", remotePort: 80),
            Forward(id: "off", enabled: false, localPort: 18081, remoteHost: "database", remotePort: 5432)])
    }
    func testDescriptionCompatibilityAndExecutionIdentity() throws {
        let original = host()
        let data = try JSONEncoder().encode(original)
        var json = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertNil(json["description"])
        for value in [NSNull(), ""] as [Any] {
            json["description"] = value
            let decoded = try JSONDecoder().decode(Host.self, from: JSONSerialization.data(withJSONObject: json))
            XCTAssertEqual(decoded.description, "")
        }
        json["description"] = 123
        XCTAssertThrowsError(try JSONDecoder().decode(Host.self, from: JSONSerialization.data(withJSONObject: json)))
        var described = original; described.description = "사내 웹 및 DB 포워딩"
        XCTAssertEqual(try JSONDecoder().decode(Host.self, from: JSONEncoder().encode(described)), described)
        XCTAssertNotEqual(described, original)
        XCTAssertTrue(described.hasSameExecutionSettings(as: original))
        XCTAssertEqual(SSHCommand.connect(described), SSHCommand.connect(original))
        described.target = "different-server"
        XCTAssertFalse(described.hasSameExecutionSettings(as: original))
        for scalar in [UInt32(0), 10, 13, 0x85, 0x2028, 0x2029] {
            described.description = "앞" + String(UnicodeScalar(scalar)!) + "뒤"
            XCTAssertThrowsError(try described.validate())
        }
    }
    func testDescriptionSurvivesSelectedSaveAndIdentityMerge() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(directory: directory)
        var first = host(); first.description = "첫 번째 설명"
        var second = host(); second.id = "second"; second.description = "두 번째 설명"
        try store.save(first); try store.save(second)
        try store.setIdentity("/tmp/key", for: first.id)
        XCTAssertEqual(try store.host(first.id).description, first.description)
        XCTAssertEqual(try store.host(second.id), second)
        first.description = "변경한 설명"; try store.save(first)
        XCTAssertEqual(try store.host(second.id), second)
    }
    func testRuleDraftAppliesAtomicallyWithoutHostIdentity() throws {
        let before = host().forwards
        var editor = RuleDraft(before[0]); editor.rule.remoteHost = "edited"; editor.localPort = "invalid"
        XCTAssertThrowsError(try editor.applying(to: before, replacing: before[0].id))
        XCTAssertEqual(before[0].remoteHost, "localhost")
        editor.localPort = "18082"
        let edited = try editor.applying(to: before, replacing: before[0].id)
        XCTAssertEqual(edited.map(\.id), before.map(\.id))
        XCTAssertEqual(edited[0].remoteHost, "edited"); XCTAssertEqual(edited[1], before[1])
        var addition = RuleDraft(); addition.localPort = "18080"
        XCTAssertThrowsError(try addition.applying(to: before))
        addition.rule.enabled = false
        XCTAssertEqual(try addition.applying(to: before).count, 3)
        addition.localPort = "0"; XCTAssertThrowsError(try addition.applying(to: []))
        addition.localPort = "65536"; XCTAssertThrowsError(try addition.applying(to: []))
        addition.localPort = "12345"; addition.remotePort = "443"
        let unfinishedHost = Host()
        XCTAssertNoThrow(try addition.applying(to: unfinishedHost.forwards))
        addition.rule.remoteHost = "bad host"; XCTAssertThrowsError(try addition.applying(to: []))
    }
    func testMenuUsesExecutionSnapshotAndKeepsDisabledRules() {
        let original = host()
        XCTAssertEqual(TrayPresentation.forwards(host: original, runtime: nil).map(\.state), [.disconnected, .disabled])
        var runtime = HostRuntime(); runtime.executionHost = original; runtime.state = .connected
        runtime.forwards = [ForwardRuntime(id: "one", description: "execution", source: "앱 설정", state: .ready),
                            ForwardRuntime(id: "ssh-config-0", description: "dynamicforward 1080", source: "SSH 설정", state: .unknown)]
        var saved = original; saved.forwards[0].remoteHost = "new-destination"; saved.forwards.removeLast()
        let rows = TrayPresentation.forwards(host: saved, runtime: runtime)
        XCTAssertEqual(rows.map(\.id), ["app:one", "app:off", "inherited:ssh-config-0"])
        XCTAssertTrue(rows[0].text.contains("localhost")); XCTAssertFalse(rows[0].text.contains("new-destination"))
        XCTAssertEqual(rows.map(\.state), [.ready, .disabled, .unknown])
        runtime.state = .disconnected; runtime.forwards[0].state = .disconnected
        XCTAssertEqual(TrayPresentation.forwards(host: saved, runtime: runtime)[0].state, .disconnected)
        var empty = original; empty.forwards = []
        XCTAssertTrue(TrayPresentation.forwards(host: empty, runtime: nil).isEmpty)
    }
    func testMenuSeparatesCollidingAppAndInheritedIDsAndAllStates() {
        var original = host(); original.forwards[0].id = "ssh-config-0"
        var runtime = HostRuntime(); runtime.executionHost = original; runtime.state = .connected
        for state in [ForwardState.preparing, .ready, .failed, .disconnected, .unknown] {
            runtime.forwards = [ForwardRuntime(id: "ssh-config-0", description: "app", source: "앱 설정", state: state),
                                ForwardRuntime(id: "ssh-config-0", description: "remote", source: "SSH 설정", state: .unknown)]
            let rows = TrayPresentation.forwards(host: original, runtime: runtime)
            XCTAssertEqual(rows.count, 3)
            XCTAssertEqual(rows[0].state.rawValue, state.rawValue)
            XCTAssertEqual(rows[2].id, "inherited:ssh-config-0")
        }
        XCTAssertEqual(Set(ForwardMenuState.allCases.map(\.symbol)).count, 6)
    }
}
