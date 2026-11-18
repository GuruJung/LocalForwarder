import Foundation

public enum LoginItemStatus: String, Equatable {
    case notPresent, notRegistered, enabled, requiresApproval

    public var isRegistered: Bool { self == .enabled || self == .requiresApproval }
}

/// Registration belongs to this machine and user, never to the shared host configuration.
@MainActor
public protocol LoginItemService {
    func status() throws -> LoginItemStatus
    func register() async throws
    func unregister() async throws
}

public struct LoginItemError: LocalizedError {
    public let localizedMessage: LocalizedMessage
    public var message: String { localizedMessage.rendered }
    public init(_ message: String) { self.localizedMessage = .raw(message) }
    public init(_ message: LocalizedMessage) { self.localizedMessage = message }
    public var errorDescription: String? { message }
}

@MainActor
public final class LoginItemController {
    private let service: LoginItemService
    public private(set) var status: LoginItemStatus?
    public private(set) var isBusy = false
    private var errorMessageValue: LocalizedMessage?
    public var errorMessage: String? { errorMessageValue?.rendered }

    public init(service: LoginItemService) { self.service = service }

    public var menuTitle: String {
        if isBusy { return L10n.text("mac.launch.at.login.updating") }
        guard let status else { return L10n.text("mac.launch.at.login.status.unavailable") }
        if status == .notPresent { return L10n.text("mac.launch.at.login.registration.needed") }
        return status == .requiresApproval ? L10n.text("mac.launch.at.login.approval.needed") : L10n.text("mac.launch.at.login")
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
        let enable = status == .notRegistered || status == .notPresent
        var mutationError: Error?
        do {
            if enable { try await service.register() } else { try await service.unregister() }
        } catch { mutationError = error }

        // Even a failed API call can have changed the OS state. Never flip a cached check mark.
        do { try readStatus() }
        catch {
            if let mutationError {
                throw LoginItemError(.error(mutationError) + .key("mac.could.not.recheck.status") + .error(error))
            }
            throw error
        }
        if let mutationError { throw mutationError }
        guard status?.isRegistered == enable else {
            throw LoginItemError(.key("mac.macos.did.not.apply.the.requested.login.setting.the"))
        }
        return true
    }

    private func readStatus() throws {
        do {
            status = try service.status()
            errorMessageValue = nil
        } catch {
            status = nil
            errorMessageValue = .error(error)
            throw error
        }
    }
}
