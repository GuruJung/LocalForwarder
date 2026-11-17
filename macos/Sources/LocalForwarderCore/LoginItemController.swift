import Foundation

public enum LoginItemStatus: String, Equatable {
    case notRegistered, enabled, requiresApproval

    public var isRegistered: Bool { self != .notRegistered }
}

/// Registration belongs to this machine and user, never to the shared host configuration.
@MainActor
public protocol LoginItemService {
    func status() throws -> LoginItemStatus
    func register() async throws
    func unregister() async throws
}

public struct LoginItemError: LocalizedError {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

@MainActor
public final class LoginItemController {
    private let service: LoginItemService
    public private(set) var status: LoginItemStatus?
    public private(set) var isBusy = false
    public private(set) var errorMessage: String?

    public init(service: LoginItemService) { self.service = service }

    public var menuTitle: String {
        if isBusy { return "로그인 자동 시작 (변경 중…)" }
        guard let status else { return "로그인 자동 시작 (상태 확인 실패)" }
        return status == .requiresApproval ? "로그인 자동 시작 (승인 필요)" : "로그인 자동 시작"
    }
    public var isChecked: Bool { status?.isRegistered == true }
    public var canToggle: Bool { status != nil && !isBusy }

    /// A refresh only reads; app launch and menu opening never register or repair an item.
    public func refresh() {
        guard !isBusy else { return }
        do { try readStatus() } catch { /* Exposed as an unknown, disabled menu item. */ }
    }

    /// Re-read before deciding which action to take: System Settings may have changed it.
    /// Return false for a duplicate click while an operation is already pending.
    @discardableResult
    public func toggle() async throws -> Bool {
        guard !isBusy else { return false }
        isBusy = true
        defer { isBusy = false }
        await Task.yield()
        try readStatus()
        let enable = status == .notRegistered
        var mutationError: Error?
        do {
            if enable { try await service.register() } else { try await service.unregister() }
        } catch { mutationError = error }

        // Even a failed API call can have changed the OS state. Never flip a cached check mark.
        do { try readStatus() }
        catch {
            if let mutationError {
                throw LoginItemError(mutationError.localizedDescription + "\n상태 재확인 실패: " + error.localizedDescription)
            }
            throw error
        }
        if let mutationError { throw mutationError }
        guard status?.isRegistered == enable else {
            throw LoginItemError("macOS가 요청한 로그인 자동 시작 설정을 반영하지 않았습니다. 실제 상태를 다시 확인했습니다.")
        }
        return true
    }

    private func readStatus() throws {
        do {
            status = try service.status()
            errorMessage = nil
        } catch {
            status = nil
            errorMessage = error.localizedDescription
            throw error
        }
    }
}
