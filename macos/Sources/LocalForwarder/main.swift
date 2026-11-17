import AppKit
import Foundation
import LocalForwarderCore

func writeLine(_ message: String) {
    try? FileHandle.standardOutput.write(contentsOf: Data((message + "\n").utf8))
}

/// GUI-independent entry points share the same manager used by the menu bar application.
func runTestManager(directory: String, configuration: String, start: Bool, singleID: String? = nil) {
    let store = ConfigurationStore(directory: URL(fileURLWithPath: directory))
    let manager = ConnectionManager(store: store, sshConfiguration: configuration)
    var previous: ConnectionState?
    func snapshot(requestID: String? = nil) {
        for host in manager.hosts {
            guard let runtime = manager.runtime(host.id) else { continue }
            if let singleID, singleID == host.id, previous != runtime.state {
                previous = runtime.state; writeLine("LF_STATE " + runtime.state.rawValue)
            }
            var value: [String: Any] = ["id": host.id, "state": runtime.state.rawValue, "intent": runtime.desiredConnected,
                                        "temporary": runtime.temporary, "error": runtime.lastError,
                                        "recentErrors": runtime.recentErrors.map { ["time": ISO8601DateFormatter().string(from: $0.time), "message": $0.message] },
                                        "forwards": runtime.forwards.map { ["id": $0.id, "state": $0.state.rawValue,
                                                                          "source": $0.source, "description": $0.description, "detail": $0.detail] }]
            if let requestID { value["requestID"] = requestID }
            if let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]),
               let json = String(data: data, encoding: .utf8) { writeLine("LF_HOST " + json) }
        }
    }
    manager.onChange = { snapshot() }
    manager.onError = { _, message in writeLine("LF_ERROR " + message.replacingOccurrences(of: "\n", with: " | ")) }
    manager.onExit = { exit(0) }
    signal(SIGTERM, SIG_IGN); signal(SIGINT, SIG_IGN)
    let terminate = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
    let interrupt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
    terminate.setEventHandler { manager.quit() }; interrupt.setEventHandler { manager.quit() }
    terminate.resume(); interrupt.resume()
    do {
        try manager.reloadConfiguration()
        if let singleID { try manager.connect(singleID) }
        else if start { try manager.startAll() }
    } catch { writeLine("LF_ERROR " + error.localizedDescription.replacingOccurrences(of: "\n", with: " | ")) }
    snapshot(); writeLine("LF_READY")
    DispatchQueue.global().async {
        while let line = readLine() {
            DispatchQueue.main.async {
                do {
                    if singleID != nil && !line.hasPrefix("{") {
                        if line == "quit" { manager.quit() }
                        else if line == "reconnect", let singleID { try manager.connect(singleID) }
                        else if line == "disconnect", let singleID { manager.disconnect(singleID) }
                        else if line == "wake" { manager.wake() }
                        else if line == "sleep" { manager.sleep() }
                        return
                    }
                    guard let data = line.data(using: .utf8),
                          let value = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                          let command = value["command"] as? String else { throw AppError("명령 JSON이 올바르지 않습니다.") }
                    func id() throws -> String {
                        guard let id = value["id"] as? String else { throw AppError("호스트 ID가 필요합니다.") }; return id
                    }
                    switch command {
                    case "connect": try manager.connect(id())
                    case "disconnect": manager.disconnect(try id())
                    case "delete": try manager.delete(id())
                    case "temporary":
                        guard let object = value["host"] else { throw AppError("호스트 설정이 필요합니다.") }
                        let encoded = try JSONSerialization.data(withJSONObject: object)
                        try manager.connect(JSONDecoder().decode(Host.self, from: encoded), temporary: true)
                    case "saved": manager.saved(try store.host(id()))
                    case "reconnect-all": try manager.reconnectAll()
                    case "wake": manager.wake()
                    case "sleep": manager.sleep()
                    case "network": manager.networkChanged(available: value["available"] as? Bool ?? false)
                    case "snapshot": snapshot(requestID: value["requestID"] as? String)
                    case "quit": manager.quit()
                    default: throw AppError("알 수 없는 명령: " + command)
                    }
                } catch { writeLine("LF_ERROR " + error.localizedDescription.replacingOccurrences(of: "\n", with: " | ")) }
            }
        }
        DispatchQueue.main.async { manager.quit() }
    }
    withExtendedLifetime((manager, terminate, interrupt)) { dispatchMain() }
}

let arguments = CommandLine.arguments
ProcessTools.sshLauncher = Bundle.main.executableURL?.path
if arguments.count >= 2 && arguments[1] == "--run-ssh" {
    signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL)
    guard setpgid(0, 0) == 0 || getpgrp() == getpid() else { exit(125) }
    let values = [SSHCommand.executable] + Array(arguments.dropFirst(2))
    let pointers = values.map { strdup($0) } + [nil]
    pointers.withUnsafeBufferPointer { buffer in _ = execv(SSHCommand.executable, buffer.baseAddress!) }
    exit(127)
} else if arguments.count == 3 && arguments[1] == "--setup-auth" {
    do { try AuthenticationSetup.run(store: ConfigurationStore(), profileID: arguments[2]); exit(0) }
    catch { fputs((error.localizedDescription + "\n").cString(using: .utf8)!, stderr); exit(1) }
} else if arguments.count == 5 && arguments[1] == "--test-setup-auth" {
    do { try AuthenticationSetup.run(store: ConfigurationStore(directory: URL(fileURLWithPath: arguments[2])), profileID: arguments[3], configuration: arguments[4]); exit(0) }
    catch { writeLine("LF_ERROR " + error.localizedDescription); exit(1) }
} else if arguments.count == 5 && arguments[1] == "--test-session" {
    runTestManager(directory: arguments[2], configuration: arguments[4], start: false, singleID: arguments[3])
} else if (arguments.count == 4 || (arguments.count == 5 && arguments[4] == "--no-start")) && arguments[1] == "--test-manager" {
    runTestManager(directory: arguments[2], configuration: arguments[3], start: arguments.count == 4)
} else if arguments.count == 1 || (arguments.count == 2 && arguments[1] == "--open-config") {
    // This executable enters AppKit synchronously on the main thread.
    MainActor.assumeIsolated {
        do {
            let instance = SingleInstance()
            guard try instance.acquire() else { exit(0) }
            let app = NSApplication.shared
            let delegate = AppDelegate(instance: instance, openConfiguration: arguments.contains("--open-config"))
            app.delegate = delegate
            withExtendedLifetime((instance, delegate)) { app.run() }
        } catch { fputs((error.localizedDescription + "\n").cString(using: .utf8)!, stderr); exit(1) }
    }
} else {
    fputs("Usage: LocalForwarder [--open-config | --setup-auth PROFILE_ID]\n", stderr); exit(2)
}
