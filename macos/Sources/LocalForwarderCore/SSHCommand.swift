import Foundation

public enum SSHCommand {
    public static let executable = "/usr/bin/ssh"
    public static let common = ["-o", "ControlMaster=no", "-o", "ControlPath=none", "-o", "ControlPersist=no",
                               "-o", "ForkAfterAuthentication=no", "-o", "RemoteCommand=none", "-o", "PermitLocalCommand=no",
                               "-o", "ConnectionAttempts=1", "-o", "ConnectTimeout=10"]
    public static func options(_ host: Host, configuration: String? = nil) -> [String] {
        var result = common
        if let configuration { result += ["-F", configuration] }
        if let user = host.user { result += ["-l", user] }
        if let port = host.port { result += ["-p", String(port)] }
        if let identity = host.identityFile { result += ["-i", (identity as NSString).expandingTildeInPath] }
        return result
    }
    public static func forwarding(_ forward: Forward) -> String {
        let host = forward.remoteHost.contains(":") && !forward.remoteHost.hasPrefix("[") ? "[\(forward.remoteHost)]" : forward.remoteHost
        return "127.0.0.1:\(forward.localPort):\(host):\(forward.remotePort)"
    }
    public static func connect(_ host: Host, configuration: String? = nil) -> [String] {
        var result = options(host, configuration: configuration) + ["-v", "-N", "-n", "-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
                       "-o", "ExitOnForwardFailure=yes", "-o", "ClearAllForwardings=no", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3"]
        for rule in host.forwards where rule.enabled { result += ["-L", forwarding(rule)] }
        return result + [host.target]
    }
    public static func inspect(_ host: Host, configuration: String? = nil) -> [String] {
        options(host, configuration: configuration) + ["-G", "-N", "-T", "-o", "ClearAllForwardings=no", "-o", "BatchMode=yes", host.target]
    }
    public static func inheritedForwards(_ output: String) -> [String] {
        output.components(separatedBy: .newlines).filter {
            $0.hasPrefix("localforward ") || $0.hasPrefix("remoteforward ") || $0.hasPrefix("dynamicforward ")
        }
    }
    public static func shellQuote(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'" }
    public static func needsAuthenticationHelp(_ message: String) -> Bool {
        let text = message.lowercased()
        return ["permission denied", "host key verification failed", "remote host identification has changed", "no such identity", "sign_and_send_pubkey", "load key"].contains { text.contains($0) }
    }
    public static func userMessage(_ log: String) -> String {
        if log.contains("Address already in use") || log.contains("cannot listen to port") {
            return "누군가 해당 로컬 포트를 사용하고 있습니다.\n\n" + log
        }
        return log.isEmpty ? "SSH 연결을 완료하지 못했습니다." : log
    }
}

/// Readiness uses SSH's own establishment events; destination application health is intentionally unrelated.
public struct Readiness {
    public var authenticated = false
    public var enteredSession = false
    public var localListener = false
    public var remoteExpected: Int
    public var remoteSucceeded = 0
    public var forwardingFailed = false
    public init(remoteExpected: Int = 0) { self.remoteExpected = remoteExpected }
    public mutating func consume(_ line: String) {
        if line.contains("Authenticated to ") || line.contains("Authentication succeeded") {
            // ProxyJump's -W child shares stderr. Its session entry is not target readiness.
            authenticated = true; enteredSession = false; localListener = false; remoteSucceeded = 0
        }
        if line.contains("Local forwarding listening on ") { localListener = true }
        if line.contains("Entering interactive session") { enteredSession = true }
        if line.contains("remote forward success for:") { remoteSucceeded += 1 }
        if line.contains("Could not request local forwarding") || line.contains("remote port forwarding failed") || line.contains("Could not request remote forwarding") {
            forwardingFailed = true
        }
    }
    public var ready: Bool {
        let forwardsReady = remoteExpected > 0 ? remoteSucceeded >= remoteExpected : localListener
        return authenticated && enteredSession && forwardsReady && !forwardingFailed
    }
}
