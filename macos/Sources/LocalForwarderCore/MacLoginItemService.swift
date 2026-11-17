import Foundation
import ServiceManagement

/// The app itself is the login item. No helper, launch agent, or shared config flag is installed.
public final class MacLoginItemService: LoginItemService {
    private let service = SMAppService.mainApp
    public init() {}

    public func status() throws -> LoginItemStatus {
        switch service.status {
        case .notRegistered: return .notRegistered
        case .enabled: return .enabled
        case .requiresApproval: return .requiresApproval
        case .notFound:
            throw LoginItemError("macOS에서 이 앱의 로그인 항목을 찾을 수 없습니다. 앱을 보관할 위치에서 다시 실행해 주세요.")
        @unknown default:
            throw LoginItemError("macOS가 알 수 없는 로그인 항목 상태를 반환했습니다.")
        }
    }

    public func register() throws { try service.register() }
    public func unregister() throws { try service.unregister() }
    public static func openSystemSettings() { SMAppService.openSystemSettingsLoginItems() }
}
