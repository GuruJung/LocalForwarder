import AppKit
import Foundation
import CoreServices
import ServiceManagement

/// Compiled with the production adapter into a disposable, uniquely identified CI .app.
@main
struct LoginItemFixture {
    @MainActor static func main() {
        let env = ProcessInfo.processInfo.environment
        guard env["GITHUB_ACTIONS"] == "true", env["RUNNER_ENVIRONMENT"] == "github-hosted",
              let identifier = Bundle.main.bundleIdentifier,
              identifier.hasPrefix("com.gurujung.LocalForwarder.LoginItemTest."),
              CommandLine.arguments.count == 3 else {
            fputs("Login-item fixture is restricted to a unique bundle on disposable GitHub-hosted CI.\n", stderr)
            exit(2)
        }
        NSApplication.shared.finishLaunching()
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
            let initial = try service.status()
            evidence["initialStatus"] = initial.rawValue
            if mode == "exercise" {
                guard initial == .notRegistered else { throw LoginItemError("Unique fixture was unexpectedly registered") }
                try service.register()
                let registered = try service.status()
                evidence["registeredStatus"] = registered.rawValue
                evidence["eligibleToRunAtLogin"] = registered == .enabled
                evidence["requiresUserApproval"] = registered == .requiresApproval
                guard registered.isRegistered else { throw LoginItemError("Registration did not persist") }
            } else if mode != "cleanup" { throw LoginItemError("Unknown fixture operation") }
        } catch { failure = error; evidence["error"] = error.localizedDescription }

        // Always attempt cleanup, including after a partially successful register() throws.
        do {
            if try service.status().isRegistered { try service.unregister() }
            let final = try service.status()
            evidence["cleanupStatus"] = final.rawValue
            evidence["cleanupSucceeded"] = final == .notRegistered
            guard final == .notRegistered else { throw LoginItemError("Fixture remains registered after cleanup") }
        } catch {
            evidence["cleanupSucceeded"] = false
            evidence["cleanupError"] = error.localizedDescription
            if failure == nil { failure = error }
        }
        evidence["passed"] = failure == nil
        do {
            let data = try JSONSerialization.data(withJSONObject: evidence, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: evidenceURL, options: .atomic)
            FileHandle.standardOutput.write(data)
            print("")
        } catch { fputs("Cannot write login-item evidence: \(error)\n", stderr); exit(1) }
        if failure != nil { exit(1) }
    }
}
