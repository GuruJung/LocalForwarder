import XCTest
@testable import LocalForwarderCore

private final class FakeLoginItemService: LoginItemService {
    var current: LoginItemStatus = .notRegistered
    var registeredState: LoginItemStatus = .enabled
    var reads = 0
    var registrations = 0
    var removals = 0
    var failRead: Int?
    var mutationFails = false
    var applyMutation = true
    var beforeRegister: (() async -> Void)?
    func status() throws -> LoginItemStatus {
        reads += 1
        if reads == failRead { throw LoginItemError("read failed") }
        return current
    }
    func register() async throws {
        registrations += 1
        await beforeRegister?()
        if applyMutation { current = registeredState }
        if mutationFails { throw LoginItemError("register failed") }
    }
    func unregister() throws {
        removals += 1
        if applyMutation { current = .notRegistered }
        if mutationFails { throw LoginItemError("unregister failed") }
    }
}

final class LoginItemTests: XCTestCase {
    @MainActor func testStartupAndRefreshOnlyReadRegistration() async {
        let service = FakeLoginItemService()
        let model = LoginItemController(service: service)
        XCTAssertEqual(service.reads, 0)
        model.refresh(); model.refresh()
        XCTAssertEqual(model.status, .notRegistered)
        XCTAssertFalse(model.isChecked)
        XCTAssertTrue(model.canToggle)
        XCTAssertEqual(service.registrations + service.removals, 0)
        service.current = .enabled
        model.refresh()
        XCTAssertTrue(model.isChecked)
    }

    @MainActor func testRegisterAndUnregisterRequeryActualState() async throws {
        let service = FakeLoginItemService()
        let model = LoginItemController(service: service)
        try await model.toggle()
        XCTAssertEqual(model.status, .enabled)
        XCTAssertEqual(service.reads, 2)
        try await model.toggle()
        XCTAssertEqual(model.status, .notRegistered)
        XCTAssertEqual(service.reads, 4)
        XCTAssertEqual(service.registrations, 1)
        XCTAssertEqual(service.removals, 1)
    }

    @MainActor func testColdAbsentRecordOnlyRegistersAfterExplicitToggle() async throws {
        for registeredState in [LoginItemStatus.enabled, .requiresApproval] {
            let service = FakeLoginItemService()
            service.current = .notPresent
            service.registeredState = registeredState
            let model = LoginItemController(service: service)
            model.refresh()
            XCTAssertEqual(model.status, .notPresent)
            XCTAssertEqual(model.menuTitle, L10n.text("mac.launch.at.login.registration.needed"))
            XCTAssertFalse(model.isChecked)
            XCTAssertTrue(model.canToggle)
            XCTAssertEqual(service.registrations, 0)
            try await model.toggle()
            XCTAssertEqual(model.status, registeredState)
            XCTAssertTrue(model.isChecked)
            XCTAssertEqual(service.registrations, 1)
            try await model.toggle()
            XCTAssertEqual(model.status, .notRegistered)
            XCTAssertFalse(model.isChecked)
            XCTAssertEqual(service.removals, 1)
        }
    }

    @MainActor func testFailedColdRegistrationPreservesAbsentRecord() async {
        let service = FakeLoginItemService()
        service.current = .notPresent
        service.mutationFails = true
        service.applyMutation = false
        let model = LoginItemController(service: service)
        do { try await model.toggle(); XCTFail("Expected registration failure") } catch {}
        XCTAssertEqual(model.status, .notPresent)
        XCTAssertFalse(model.isChecked)
        XCTAssertTrue(model.canToggle)
        XCTAssertEqual(service.registrations, 1)
    }

    @MainActor func testApprovalRequiredIsCheckedAndCanBeRemoved() async throws {
        let service = FakeLoginItemService(); service.registeredState = .requiresApproval
        let model = LoginItemController(service: service)
        try await model.toggle()
        XCTAssertEqual(model.status, .requiresApproval)
        XCTAssertEqual(model.menuTitle, L10n.text("mac.launch.at.login.approval.needed"))
        XCTAssertTrue(model.isChecked)
        XCTAssertTrue(model.canToggle)
        try await model.toggle()
        XCTAssertFalse(model.isChecked)
        XCTAssertEqual(service.removals, 1)
    }

    @MainActor func testExternalChangeIsReadBeforeToggle() async throws {
        let service = FakeLoginItemService()
        let model = LoginItemController(service: service); model.refresh()
        service.current = .requiresApproval
        try await model.toggle()
        XCTAssertEqual(service.registrations, 0)
        XCTAssertEqual(service.removals, 1)
    }

    @MainActor func testReadFailureDisablesUnknownStateAndNeverMutates() async {
        let service = FakeLoginItemService(); service.failRead = 1
        let model = LoginItemController(service: service); model.refresh()
        XCTAssertNil(model.status)
        XCTAssertFalse(model.canToggle)
        XCTAssertNotNil(model.errorMessage)
        service.failRead = 2
        do { try await model.toggle(); XCTFail("Expected read failure") } catch {}
        XCTAssertEqual(service.registrations + service.removals, 0)
        XCTAssertFalse(model.isBusy)
        model.refresh()
        XCTAssertTrue(model.canToggle)
    }

    @MainActor func testFailedMutationUsesActualStateWithoutOptimisticFlip() async {
        let service = FakeLoginItemService(); service.mutationFails = true; service.applyMutation = false
        let model = LoginItemController(service: service)
        do { try await model.toggle(); XCTFail("Expected mutation failure") } catch {}
        XCTAssertEqual(model.status, .notRegistered)
        XCTAssertFalse(model.isChecked)
        XCTAssertEqual(service.reads, 2)
        // A failed API call can still take effect; the resulting check must reflect the OS.
        service.applyMutation = true
        do { try await model.toggle(); XCTFail("Expected mutation failure") } catch {}
        XCTAssertTrue(model.isChecked)
        XCTAssertFalse(model.isBusy)
        do { try await model.toggle(); XCTFail("Expected unregister failure") } catch {}
        XCTAssertFalse(model.isChecked)
    }

    @MainActor func testPostMutationReadFailureDoesNotClaimSuccess() async {
        let service = FakeLoginItemService(); service.failRead = 2
        let model = LoginItemController(service: service)
        do { try await model.toggle(); XCTFail("Expected post-operation read failure") } catch {}
        XCTAssertEqual(service.current, .enabled)
        XCTAssertNil(model.status)
        XCTAssertFalse(model.canToggle)
        model.refresh()
        XCTAssertTrue(model.isChecked)
    }

    @MainActor func testUnchangedStateAfterMutationIsAnError() async {
        let service = FakeLoginItemService(); service.applyMutation = false
        let model = LoginItemController(service: service)
        do { try await model.toggle(); XCTFail("Expected unchanged state error") } catch {}
        XCTAssertFalse(model.isChecked)
    }

    @MainActor func testConcurrentClicksAreSuppressed() async throws {
        let service = FakeLoginItemService()
        let model = LoginItemController(service: service)
        var resumeRegistration: CheckedContinuation<Void, Never>?
        service.beforeRegister = {
            await withCheckedContinuation { resumeRegistration = $0 }
        }
        let first = Task { try await model.toggle() }
        while resumeRegistration == nil { await Task.yield() }
        XCTAssertTrue(model.isBusy)
        XCTAssertFalse(model.canToggle)
        let duplicate = try await model.toggle()
        XCTAssertFalse(duplicate)
        resumeRegistration?.resume()
        let applied = try await first.value
        XCTAssertTrue(applied)
        XCTAssertEqual(service.registrations, 1)
        XCTAssertEqual(service.removals, 0)
        XCTAssertFalse(model.isBusy)
    }
}
