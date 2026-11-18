import XCTest
@testable import LocalForwarderCore

final class LocalizationTests: XCTestCase {
    override func setUp() { super.setUp(); L10n.configure(selection: "en") }
    override func tearDown() { L10n.configure(selection: "en"); super.tearDown() }

    func testPreferredLanguagesAndChineseScripts() {
        XCTAssertEqual(L10n.resolve(nil, preferred: ["fr-FR", "de-AT", "en-US"]), "de")
        XCTAssertEqual(L10n.resolve("unknown", preferred: ["fr", "it"]), "en")
        XCTAssertEqual(L10n.resolve("ko", preferred: ["ja"]), "ko")
        for (tag, expected) in [("zh", "zh-Hans"), ("zh-SG", "zh-Hans"), ("zh_CN", "zh-Hans"),
                                ("zh-TW", "zh-Hant"), ("zh-HK", "zh-Hant"), ("zh-MO", "zh-Hant"),
                                ("zh-Hans-TW", "zh-Hans"), ("zh-Hant-CN", "zh-Hant"),
                                ("ja-JP", "ja"), ("es-MX", "es"), ("en-GB", "en")] {
            XCTAssertEqual(L10n.match(tag), expected, tag)
        }
        XCTAssertNil(L10n.match("fr-CA"))
    }
    func testLiteralArgumentsAndEnglishFallback() {
        XCTAssertEqual(L10n.text("mac.settings"), "Settings")
        XCTAssertEqual(L10n.text("mac.settings", language: "ko"), "설정")
        XCTAssertEqual(L10n.text("mac.settings", language: "unsupported"), "Settings")
        XCTAssertEqual(L10n.format("{host} / {count}", ["host": "{count} & 100% $()", "count": "7"]), "{count} & 100% $() / 7")
    }
    func testSelectionsPersistAndFailedSavesPreserveContext() throws {
        var saved: String?
        var fail = false
        let preferences = LanguagePreferences(load: { saved }, save: { value in
            if fail { throw AppError("disk failure") }; saved = value
        }, preferred: { ["ja-JP"] })
        preferences.start()
        XCTAssertEqual(L10n.selection, "auto"); XCTAssertEqual(L10n.language, "ja")
        try preferences.select("de")
        L10n.configure(selection: "en"); preferences.start()
        XCTAssertEqual(L10n.selection, "de"); XCTAssertEqual(L10n.language, "de")
        fail = true
        XCTAssertThrowsError(try preferences.select("ko"))
        XCTAssertEqual(saved, "de"); XCTAssertEqual(L10n.selection, "de"); XCTAssertEqual(L10n.language, "de")
        fail = false; try preferences.select("auto")
        XCTAssertEqual(saved, "auto"); XCTAssertEqual(L10n.language, "ja")
    }
    func testRetainedMessagesRerenderWithoutChangingRawSSH() throws {
        let raw = "bind [127.0.0.1]:8080: Address already in use {host} 한글"
        let message = SSHCommand.userMessageValue(.raw(raw))
        let error = AppError(.key("mac.enter.a.host.name"))
        let english = message.rendered
        XCTAssertTrue(english.hasPrefix("The local port is already in use."))
        XCTAssertEqual(error.localizedDescription, "Enter a host name.")
        L10n.configure(selection: "ko")
        XCTAssertTrue(message.rendered.hasPrefix("누군가 해당 로컬 포트를 사용하고 있습니다."))
        XCTAssertTrue(message.rendered.hasSuffix(raw))
        XCTAssertEqual(error.localizedDescription, "호스트 이름을 입력하세요.")
        XCTAssertEqual(LocalizedMessage.raw(raw).rendered, raw)
        XCTAssertEqual(ProcessTools.environment()["LC_ALL"], "C")
    }
    func testForwardDescriptionsRetainMessageKeys() {
        let host = Host(id: "host", name: "Custom {name}", target: "example.invalid", forwards: [Forward(id: "one")])
        var tracker = ForwardTracker(host: host)
        tracker.sessionReady()
        XCTAssertEqual(tracker.forwards[0].source, "App configuration")
        let original = tracker.forwards[0].description
        L10n.configure(selection: "ko")
        XCTAssertEqual(tracker.forwards[0].source, "앱 설정")
        XCTAssertTrue(tracker.forwards[0].detail.hasPrefix("SSH 연결은 성립"))
        XCTAssertEqual(tracker.forwards[0].description, original)
        XCTAssertEqual(host.name, "Custom {name}")
    }
}
