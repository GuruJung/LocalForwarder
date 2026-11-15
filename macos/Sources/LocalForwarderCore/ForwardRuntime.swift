import Foundation

public enum ForwardState: String { case preparing, ready, failed, disconnected, unknown }
public struct ForwardRuntime: Equatable {
    public var id: String
    public var description: String
    public var source: String
    public var detail: String
    public var state: ForwardState
    public init(id: String, description: String, source: String, detail: String = "", state: ForwardState = .preparing) {
        self.id = id; self.description = description; self.source = source; self.detail = detail; self.state = state
    }
}
public struct DestinationError: Equatable {
    public let time: Date
    public let message: String
    public init(time: Date, message: String) { self.time = time; self.message = message }
}
/// Conservative interpretation of OpenSSH's observations; never probes destination services.
public struct ForwardTracker {
    public private(set) var forwards: [ForwardRuntime] = []
    public private(set) var recentErrors: [DestinationError] = []
    private struct Binding { let remote: Bool; let port: Int?; let address: String? }
    private var bindings: [Binding] = []
    private var pendingListener: (address: String, port: Int)?
    private var establishedAddresses: [Int: Set<String>] = [:]
    public init(host: Host, inherited: [String] = []) {
        for rule in host.forwards where rule.enabled {
            forwards.append(ForwardRuntime(id: rule.id, description: SSHCommand.forwarding(rule), source: "앱 설정"))
            bindings.append(Binding(remote: false, port: rule.localPort, address: "127.0.0.1"))
        }
        for (index, line) in inherited.enumerated() {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            let endpoint = fields.count > 1 ? fields[1] : ""
            let port = Int(endpoint.split(separator: ":").last.map(String.init) ?? endpoint)
            let address = endpoint.contains(":") ? String(endpoint.prefix(upTo: endpoint.lastIndex(of: ":")!)).replacingOccurrences(of: "[", with: "").replacingOccurrences(of: "]", with: "") : nil
            forwards.append(ForwardRuntime(id: "ssh-config-\(index)", description: line, source: "SSH 설정", detail: port == nil ? "이 규칙의 개설 결과를 SSH 진단에서 식별할 수 없습니다." : ""))
            bindings.append(Binding(remote: line.hasPrefix("remoteforward "), port: port, address: address))
        }
    }
    private func captures(_ pattern: String, _ text: String) -> [String]? {
        guard let regex = try? NSRegularExpression(pattern: pattern), let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) else { return nil }
        return (1..<match.numberOfRanges).map { Range(match.range(at: $0), in: text).map { String(text[$0]) } ?? "" }
    }
    private mutating func mark(remote: Bool, port: Int?, address: String? = nil, state: ForwardState, line: String) {
        guard let port else { return }
        let candidates = bindings.indices.filter { index in
            let binding = bindings[index]
            return binding.remote == remote && binding.port == port && (address == nil || binding.address == nil || binding.address == address || binding.address == "*" || binding.address == "" || (binding.address == "localhost" && (address == "127.0.0.1" || address == "::1")))
        }
        guard candidates.count == 1, let index = candidates.first else {
            for index in candidates where forwards[index].state == .preparing {
                forwards[index].detail = "같은 리스너를 사용하는 규칙들의 개별 결과를 식별할 수 없습니다."
            }
            return
        }
        if !remote {
            if state == .ready { establishedAddresses[index, default: []].insert(address ?? "") }
            else if state == .failed {
                if let address { establishedAddresses[index]?.remove(address) }
                else { establishedAddresses[index] = [] }
            }
            // A wildcard/localhost rule may have distinct IPv4 and IPv6 bind outcomes.
            if !(establishedAddresses[index] ?? []).isEmpty {
                forwards[index].state = .ready
                forwards[index].detail = state == .failed ? "일부 주소의 개설 실패: " + line : "포워딩 개설 성공 · 목적지 서비스 상태는 검사하지 않음"
                return
            }
        }
        forwards[index].state = state; forwards[index].detail = state == .ready ? "포워딩 개설 성공 · 목적지 서비스 상태는 검사하지 않음" : line
    }
    public mutating func consume(_ line: String, now: Date = Date()) {
        if let match = captures("Local forwarding listening on (.+) port ([0-9]+)\\.", line) {
            // OpenSSH prints this BEFORE bind/listen. Only channel creation proves success.
            pendingListener = Int(match[1]).map { (address: match[0], port: $0) }
        } else if line.contains(": new port-listener [port listener]"), let pending = pendingListener {
            mark(remote: false, port: pending.port, address: pending.address, state: .ready, line: line)
            pendingListener = nil
        } else if let match = captures("(?:bind|listen) \\[([^]]+)\\]:([0-9]+):", line) {
            mark(remote: false, port: Int(match[1]), address: match[0], state: .failed, line: line)
            pendingListener = nil
        } else if let match = captures("cannot listen to port: ([0-9]+)", line) {
            mark(remote: false, port: Int(match[0]), state: .failed, line: line)
            pendingListener = nil
        } else if line.contains("remote forward success for:"), let match = captures("listen (?:.*:)?([0-9]+),", line) {
            mark(remote: true, port: Int(match[0]), state: .ready, line: line)
        } else if let match = captures("remote port forwarding failed for listen port ([0-9]+)", line) {
            mark(remote: true, port: Int(match[0]), state: .failed, line: line)
        } else if line.contains("remote forward failure for:"), let match = captures("listen (?:.*:)?([0-9]+),", line) {
            mark(remote: true, port: Int(match[0]), state: .failed, line: line)
        }
        if line.contains("open failed:") || line.contains("connect failed:") {
            recentErrors.append(DestinationError(time: now, message: String(line.suffix(2048))))
            if recentErrors.count > 10 { recentErrors.removeFirst(recentErrors.count - 10) }
        }
    }
    public mutating func sessionReady() {
        pendingListener = nil
        for index in forwards.indices where forwards[index].state == .preparing {
            forwards[index].state = .unknown
            if forwards[index].detail.isEmpty { forwards[index].detail = "SSH 연결은 성립했지만 이 규칙의 개설 결과는 아직 확인되지 않았습니다." }
        }
    }
    public mutating func disconnected() {
        pendingListener = nil; establishedAddresses = [:]
        for index in forwards.indices {
            forwards[index].state = .disconnected
        }
    }
}
