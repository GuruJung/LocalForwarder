import AppKit
import Foundation
import CoreServices
import ServiceManagement

/// Compiled with the production adapter into a disposable, uniquely identified CI .app.
@main
struct LoginItemFixture {
    @MainActor static func main() {
        let application = NSApplication.shared
        let delegate = LoginItemFixtureDelegate()
        application.delegate = delegate
        withExtendedLifetime(delegate) { application.run() }
    }

    @MainActor static func exercise() async {
        let env = ProcessInfo.processInfo.environment
        guard env["GITHUB_ACTIONS"] == "true", env["RUNNER_ENVIRONMENT"] == "github-hosted",
              let identifier = Bundle.main.bundleIdentifier,
              identifier.hasPrefix("com.gurujung.LocalForwarder.LoginItemTest."),
              CommandLine.arguments.count == 3 else {
            fputs("Login-item fixture is restricted to a unique bundle on disposable GitHub-hosted CI.\n", stderr)
            exit(2)
        }
        let mode = CommandLine.arguments[1]
        let evidenceURL = URL(fileURLWithPath: CommandLine.arguments[2])
        var evidence: [String: Any] = ["bundleIdentifier": identifier, "signing": "ad-hoc",
            "mode": mode, "actualLoginTested": false, "launchMethod": "LaunchServices open",
            "bundlePath": Bundle.main.bundlePath,
            "executablePath": Bundle.main.executablePath ?? "missing"]
        // Construct SMAppService only after LaunchServices knows this app and AppKit finishes launch.
        let launchServicesResult = LSRegisterURL(Bundle.main.bundleURL as CFURL, true)
        evidence["launchServicesRegistrationStatus"] = launchServicesResult
        let service = MacLoginItemService()
        evidence["initialRawServiceStatus"] = SMAppService.mainApp.status.rawValue
        var failure: Error?
        do {
            guard launchServicesResult == noErr else {
                throw LoginItemError("LaunchServices registration failed: \(launchServicesResult)")
            }
            let controller = LoginItemController(service: service)
            controller.refresh()
            guard let initial = controller.status else {
                throw LoginItemError(controller.errorMessage ?? "Initial status query failed")
            }
            evidence["initialStatus"] = initial.rawValue
            evidence["initialMenuTitle"] = controller.menuTitle
            evidence["initialChecked"] = controller.isChecked
            if mode == "exercise" {
                guard initial == .notRegistered || initial == .notPresent else {
                    throw LoginItemError("Unique fixture was unexpectedly registered")
                }
                try await controller.toggle()
                guard let registered = controller.status else { throw LoginItemError("Post-registration status unavailable") }
                evidence["registeredStatus"] = registered.rawValue
                evidence["registeredRawServiceStatus"] = SMAppService.mainApp.status.rawValue
                evidence["eligibleToRunAtLogin"] = registered == .enabled
                evidence["requiresUserApproval"] = registered == .requiresApproval
                guard registered.isRegistered else { throw LoginItemError("Registration did not persist") }
                try await controller.toggle()
                evidence["unregisteredStatus"] = controller.status?.rawValue ?? "unknown"
                evidence["unregisteredRawServiceStatus"] = SMAppService.mainApp.status.rawValue
                guard controller.status == .notRegistered else {
                    throw LoginItemError("Controller did not remove the login item")
                }
            } else if mode != "cleanup" { throw LoginItemError("Unknown fixture operation") }
        } catch {
            failure = error
            evidence["error"] = error.localizedDescription
            evidence["errorDetails"] = errorDetails(error)
        }

        // Always attempt cleanup, including after a partially successful register() throws.
        do {
            // Unknown is not proof of no registration; explicitly attempt unregister in that case.
            if SMAppService.mainApp.status != .notRegistered { try service.unregister() }
            let final = try service.status()
            evidence["cleanupStatus"] = final.rawValue
            evidence["cleanupSucceeded"] = final == .notRegistered
            guard final == .notRegistered else { throw LoginItemError("Fixture remains registered after cleanup") }
        } catch {
            evidence["cleanupSucceeded"] = false
            evidence["cleanupError"] = error.localizedDescription
            evidence["cleanupErrorDetails"] = errorDetails(error)
            if failure == nil { failure = error }
        }
        evidence["finalRawServiceStatus"] = SMAppService.mainApp.status.rawValue
        evidence["passed"] = failure == nil
        do {
            let data = try JSONSerialization.data(withJSONObject: evidence, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: evidenceURL, options: .atomic)
            FileHandle.standardOutput.write(data)
            print("")
        } catch { fputs("Cannot write login-item evidence: \(error)\n", stderr); exit(1) }
        exit(failure == nil ? 0 : 1)
    }

    private static func errorDetails(_ error: Error) -> [String: Any] {
        let error = error as NSError
        return ["domain": error.domain, "code": error.code, "description": error.localizedDescription,
                "userInfo": String(describing: error.userInfo)]
    }
}

@MainActor
private final class LoginItemFixtureDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Exercise on the first run-loop turn after the ordinary application launch callback.
        Task { @MainActor in await LoginItemFixture.exercise() }
    }
}
