import XCTest
@testable import LocalForwarderCore

final class ForwardOrderTests: XCTestCase {
    private var rules: [Forward] {
        [Forward(id: "high", localPort: 10000, remoteHost: "a", remotePort: 1, description: "high"),
         Forward(id: "tie-first", enabled: false, localPort: 1000, remoteHost: "b", remotePort: 2, description: "disabled first"),
         Forward(id: "low", localPort: 80, remoteHost: "c", remotePort: 3, description: "low"),
         Forward(id: "tie-second", enabled: false, localPort: 1000, remoteHost: "d", remotePort: 4, description: "disabled second"),
         Forward(id: "middle", localPort: 443, remoteHost: "e", remotePort: 5, description: "middle")]
    }
    func testNumericAscendingStableTiesAndAllRuleFields() {
        let original = rules
        let sorted = ForwardOrder.byLocalPort(original)
        XCTAssertEqual(sorted.map(\.localPort), [80, 443, 1000, 1000, 10000])
        XCTAssertEqual(sorted.map(\.id), ["low", "middle", "tie-first", "tie-second", "high"])
        XCTAssertEqual(sorted, [original[2], original[4], original[1], original[3], original[0]])
        XCTAssertEqual(ForwardOrder.byLocalPort(sorted), sorted)
        XCTAssertEqual(original, rules)
        XCTAssertEqual(ForwardOrder.byLocalPort([]), [])
        XCTAssertEqual(ForwardOrder.byLocalPort([original[0]]), [original[0]])
    }
    func testOnlyExplicitSavePersistsReorderAndPreservesOtherHost() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(directory: directory)
        let original = Host(id: "first", name: "First", target: "first.invalid", forwards: rules)
        let other = Host(id: "other", name: "Other", target: "other.invalid", forwards: [Forward(id: "other-rule")])
        try store.save(original); try store.save(other)
        let before = try Data(contentsOf: store.file)
        var draft = original; draft.forwards = ForwardOrder.byLocalPort(draft.forwards)
        XCTAssertNotEqual(draft, original)
        XCTAssertEqual(try Data(contentsOf: store.file), before)
        XCTAssertEqual(try ConfigurationStore(directory: directory).host(original.id), original)
        try store.save(draft)
        let reopened = ConfigurationStore(directory: directory)
        XCTAssertEqual(try reopened.host(original.id), draft)
        XCTAssertEqual(try reopened.host(other.id), other)
    }
}
