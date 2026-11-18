import XCTest
@testable import LocalForwarderCore

final class SettingsPresentationTests: XCTestCase {
    private func host() -> LocalForwarderCore.Host {
        Host(id: "host", name: "서버", target: "server", forwards: [
            Forward(id: "one", localPort: 18080, remoteHost: "localhost", remotePort: 80),
            Forward(id: "off", enabled: false, localPort: 18081, remoteHost: "database", remotePort: 5432)])
    }
    func testHostConnectivityPresentationKeepsForwardFailuresInChildRows() {
        var host = host(); host.autoConnect = false
        var runtime = HostRuntime(); runtime.executionHost = host; runtime.state = .connected
        runtime.forwards = [ForwardRuntime(id: "one", description: "failed forward", sourceMessage: .key("mac.app.configuration"), state: .failed)]
        XCTAssertEqual(TrayPresentation.host(host, runtime: runtime).state, .connected)
        XCTAssertEqual(TrayPresentation.host(host, runtime: runtime).title, host.name)
        XCTAssertEqual(TrayPresentation.forwards(host: host, runtime: runtime)[0].state, .failed)
        runtime.state = .disconnected
        XCTAssertEqual(TrayPresentation.host(host, runtime: runtime).state, .disabled)
        host.autoConnect = true
        XCTAssertEqual(TrayPresentation.host(host, runtime: runtime).state, .disconnected)
        runtime.temporary = true
        for state in [ConnectionState.connecting, .stopping] {
            runtime.state = state
            XCTAssertEqual(TrayPresentation.host(host, runtime: runtime).state, .disconnected)
            XCTAssertEqual(TrayPresentation.host(host, runtime: runtime).title, host.name + L10n.text("mac.temporary") + L10n.text("mac.working"))
            host.autoConnect = false
            XCTAssertEqual(TrayPresentation.host(host, runtime: runtime).state, .disabled)
            host.autoConnect = true
        }
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
        runtime.forwards = [ForwardRuntime(id: "one", description: "execution", sourceMessage: .key("mac.app.configuration"), state: .ready),
                            ForwardRuntime(id: "ssh-config-0", description: "dynamicforward 1080", sourceMessage: .key("mac.ssh.configuration"), state: .unknown)]
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
    func testForwardOriginIdentitySurvivesEveryLanguageSwitch() {
        let previous = L10n.selection
        defer { L10n.configure(selection: previous) }
        var original = host(); original.forwards = [original.forwards[0]]
        var runtime = HostRuntime(); runtime.executionHost = original; runtime.state = .connected
        runtime.forwards = [
            ForwardRuntime(id: "one", description: "app", sourceMessage: .key("mac.app.configuration"), state: .ready),
            ForwardRuntime(id: "one", description: "inherited", sourceMessage: .key("mac.ssh.configuration"), state: .failed)]
        for language in L10n.languages {
            L10n.configure(selection: language)
            let rows = TrayPresentation.forwards(host: original, runtime: runtime)
            XCTAssertEqual(rows.map(\.id), ["app:one", "inherited:one"], language)
            XCTAssertEqual(rows.map(\.state), [.ready, .failed], language)
            // Display text equal to an app label must never identify an app-owned rule.
            let raw = ForwardRuntime(id: "one", description: "raw", source: L10n.text("mac.app.configuration"), state: .failed)
            var rawRuntime = runtime; rawRuntime.forwards = [raw]
            let rawRows = TrayPresentation.forwards(host: original, runtime: rawRuntime)
            XCTAssertEqual(rawRows.map(\.id), ["app:one", "inherited:one"], language)
            XCTAssertEqual(rawRows.map(\.state), [.unknown, .failed], language)
        }
    }
    func testMenuSeparatesCollidingAppAndInheritedIDsAndAllStates() {
        var original = host(); original.forwards[0].id = "ssh-config-0"
        var runtime = HostRuntime(); runtime.executionHost = original; runtime.state = .connected
        for state in [ForwardState.preparing, .ready, .failed, .disconnected, .unknown] {
            runtime.forwards = [ForwardRuntime(id: "ssh-config-0", description: "app", sourceMessage: .key("mac.app.configuration"), state: state),
                                ForwardRuntime(id: "ssh-config-0", description: "remote", sourceMessage: .key("mac.ssh.configuration"), state: .unknown)]
            let rows = TrayPresentation.forwards(host: original, runtime: runtime)
            XCTAssertEqual(rows.count, 3)
            XCTAssertEqual(rows[0].state.rawValue, state.rawValue)
            XCTAssertEqual(rows[2].id, "inherited:ssh-config-0")
        }
        XCTAssertEqual(Set(ForwardMenuState.allCases.map(\.symbol)).count, 6)
    }
}
