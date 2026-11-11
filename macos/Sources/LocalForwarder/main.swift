import AppKit
import Foundation
import LocalForwarderCore

func writeLine(_ message: String) {
    try? FileHandle.standardOutput.write(contentsOf: Data((message + "\n").utf8))
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
    let session = SSHSession(store: ConfigurationStore(directory: URL(fileURLWithPath: arguments[2])), sshConfiguration: arguments[4])
    var previous: ConnectionState?
    session.onChange = { state in if state != previous { previous = state; writeLine("LF_STATE " + state.rawValue) } }
    session.onError = { writeLine("LF_ERROR " + $0.replacingOccurrences(of: "\n", with: " | ")) }
    session.onInitialFailure = { _ in exit(1) }
    session.onExit = { exit(0) }
    signal(SIGTERM, SIG_IGN); signal(SIGINT, SIG_IGN)
    let terminate = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
    let interrupt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
    terminate.setEventHandler { session.quit() }; interrupt.setEventHandler { session.quit() }
    terminate.resume(); interrupt.resume()
    DispatchQueue.global().async {
        while let line = readLine() {
            DispatchQueue.main.async {
                if line == "quit" { session.quit() }
                else if line == "reconnect" { session.reconnect() }
            }
        }
        DispatchQueue.main.async { session.quit() }
    }
    session.start(arguments[3])
    withExtendedLifetime((session, terminate, interrupt)) { dispatchMain() }
} else if arguments.count != 1 {
    fputs("Usage: LocalForwarder [--setup-auth PROFILE_ID]\n", stderr); exit(2)
} else {
    let app = NSApplication.shared
    let delegate = AppDelegate(); app.delegate = delegate
    app.run()
}
