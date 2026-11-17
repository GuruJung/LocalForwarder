import XCTest
@testable import LocalForwarderCore

final class ForwardDescriptionTests: XCTestCase {
    private func host() -> LocalForwarderCore.Host {
        Host(id: "host", name: "서버", target: "server", forwards: [
            Forward(id: "web", localPort: 18080, remoteHost: "localhost", remotePort: 80),
            Forward(id: "database", enabled: false, localPort: 15432, remoteHost: "database", remotePort: 5432)])
    }
    private func json(_ value: Forward) throws -> [String: Any] {
        try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(value)) as? [String: Any])
    }
    private func decode(_ object: [String: Any]) throws -> Forward {
        try JSONDecoder().decode(Forward.self, from: JSONSerialization.data(withJSONObject: object))
    }
    func testOptionalDescriptionRoundTripAndRequiredFields() throws {
        let original = host().forwards[0]
        let legacy = try json(original)
        XCTAssertNil(legacy["description"])
        XCTAssertEqual(try decode(legacy), original)
        for absent in [NSNull(), ""] as [Any] {
            var object = legacy; object["description"] = absent
            let rule = try decode(object)
            XCTAssertEqual(rule, original)
            XCTAssertNil(try json(rule)["description"])
        }
        var described = original; described.description = "사내 웹 대시보드"
        XCTAssertEqual(try decode(json(described)), described)
        XCTAssertEqual(try json(described)["description"] as? String, described.description)
        for invalid in [true, 123, ["wrong"], ["text": "wrong"]] as [Any] {
            var object = legacy; object["description"] = invalid
            XCTAssertThrowsError(try decode(object))
        }
        for key in ["id", "enabled", "localPort", "remoteHost", "remotePort"] {
            var object = legacy; object.removeValue(forKey: key)
            XCTAssertThrowsError(try decode(object), "Missing required field: \(key)")
            object[key] = NSNull()
            XCTAssertThrowsError(try decode(object), "Null required field: \(key)")
        }
    }
    func testV1AndV2LoadWithoutRewritingAndPreserveOriginalMigrationBackup() throws {
        for version in [1, 2] {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: directory) }
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let store = ConfigurationStore(directory: directory)
            var configuration = Configuration(hosts: [host()]); configuration.version = version
            let original = try JSONEncoder().encode(configuration)
            try original.write(to: store.file)
            let loaded = try store.load()
            XCTAssertEqual(loaded.version, version)
            XCTAssertEqual(loaded.hosts[0].forwards.map(\.description), ["", ""])
            XCTAssertEqual(try Data(contentsOf: store.file), original)
            var edited = loaded.hosts[0]; edited.forwards[0].description = "기존 웹 규칙"
            try store.save(edited)
            XCTAssertEqual(try store.load().version, 2)
            XCTAssertEqual(try store.host(edited.id), edited)
            let backups = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
                .filter { $0.lastPathComponent.hasPrefix("config-v1-") }
            XCTAssertEqual(backups.count, version == 1 ? 1 : 0)
            if version == 1 { XCTAssertEqual(try Data(contentsOf: XCTUnwrap(backups.first)), original) }
            try store.save(edited)
            XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path)
                .filter { $0.hasPrefix("config-v1-") }.count, version == 1 ? 1 : 0)
        }
    }
    func testInvalidDescriptionRejectsLoadAndSaveWithoutChangingFile() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(directory: directory)
        try store.save(host())
        let original = try Data(contentsOf: store.file)
        for scalar in [UInt32(0), 10, 13, 0x85, 0x2028, 0x2029] {
            var invalid = host()
            // Disabled rules have the same validation contract.
            invalid.forwards[1].description = "앞" + String(UnicodeScalar(scalar)!) + "뒤"
            XCTAssertThrowsError(try store.save(invalid))
            XCTAssertEqual(try Data(contentsOf: store.file), original)
            let invalidBytes = try JSONEncoder().encode(Configuration(hosts: [invalid]))
            try invalidBytes.write(to: store.file)
            XCTAssertThrowsError(try store.load())
            XCTAssertThrowsError(try store.save(host()))
            XCTAssertEqual(try Data(contentsOf: store.file), invalidBytes)
            try original.write(to: store.file)
        }
    }
    func testSelectedSaveOtherHostAndIdentityUpdatePreserveRuleDescriptions() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(directory: directory)
        var first = host(); first.forwards[0].description = "웹 화면"; first.forwards[1].description = "DB 읽기"
        var second = host(); second.id = "other"; second.forwards[0].description = "다른 호스트 웹"
        try store.save(first); try store.save(second)
        XCTAssertEqual(try store.host(first.id), first)
        try ConfigurationStore(directory: directory).setIdentity("/tmp/new-key", for: first.id)
        first.identityFile = "/tmp/new-key"
        XCTAssertEqual(try store.host(first.id), first)
        second.name = "다른 호스트 수정"; try store.save(second)
        XCTAssertEqual(try store.host(first.id), first)
        first.forwards[0].description = ""; try store.save(first)
        XCTAssertEqual(try store.host(first.id), first)
        XCTAssertEqual(try store.host(second.id), second)
        XCTAssertNil(try json(store.host(first.id).forwards[0])["description"])
    }
    func testDescriptionsAreDirtyMetadataOnlyAndDoNotChangeTrayOrSSH() {
        let original = host()
        var changed = original
        changed.description = "호스트 설명"
        changed.forwards[0].description = "웹 규칙 설명"
        changed.forwards[1].description = "비활성 규칙 설명"
        XCTAssertNotEqual(changed, original)
        XCTAssertTrue(changed.hasSameExecutionSettings(as: original))
        XCTAssertTrue(original.hasSameExecutionSettings(as: changed))
        XCTAssertEqual(SSHCommand.connect(changed), SSHCommand.connect(original))
        XCTAssertEqual(SSHCommand.inspect(changed), SSHCommand.inspect(original))
        XCTAssertEqual(changed.forwards.map(SSHCommand.forwarding), original.forwards.map(SSHCommand.forwarding))
        XCTAssertEqual(TrayPresentation.forwards(host: changed, runtime: nil).map(\.text),
                       TrayPresentation.forwards(host: original, runtime: nil).map(\.text))
        for index in changed.forwards.indices {
            var connectionEdit = changed; connectionEdit.forwards[index].remotePort += 1
            XCTAssertFalse(original.hasSameExecutionSettings(as: connectionEdit))
        }
        var reordered = changed; reordered.forwards.reverse()
        XCTAssertFalse(original.hasSameExecutionSettings(as: reordered))
        var removed = changed; removed.forwards.removeLast()
        XCTAssertFalse(original.hasSameExecutionSettings(as: removed))
    }
    func testRuleDraftDescriptionConfirmDiscardClearAndIdentityOrder() throws {
        let original = host().forwards
        var editor = RuleDraft(original[0]); editor.rule.description = "편집한 웹 설명"
        // A modal owns this value copy; cancelling without applying cannot change the parent.
        XCTAssertEqual(original[0].description, "")
        editor.rule.id = "replacement-id-must-not-win"
        let edited = try editor.applying(to: original, replacing: original[0].id)
        XCTAssertEqual(edited.map(\.id), original.map(\.id))
        XCTAssertEqual(edited[0].description, "편집한 웹 설명")
        XCTAssertEqual(edited[1], original[1])
        var clearing = RuleDraft(edited[0]); clearing.rule.description = ""
        XCTAssertEqual(try clearing.applying(to: edited, replacing: original[0].id), original)
        var addition = RuleDraft()
        XCTAssertEqual(addition.rule.description, "")
        addition.rule.description = "추가한 규칙 설명"
        let added = try addition.applying(to: edited)
        XCTAssertEqual(Array(added.prefix(edited.count)), edited)
        XCTAssertEqual(added.last?.id, addition.rule.id)
        XCTAssertEqual(added.last?.description, addition.rule.description)
        editor.rule.description = "잘못된\n설명"
        XCTAssertThrowsError(try editor.applying(to: edited, replacing: edited[0].id))
        XCTAssertEqual(edited[0].description, "편집한 웹 설명")
    }
}
