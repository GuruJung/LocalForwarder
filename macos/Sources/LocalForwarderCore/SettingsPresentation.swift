import Foundation

/// A modal owns this value until validation succeeds; cancelling never touches the host draft.
public struct RuleDraft {
    public var rule: Forward
    public var localPort: String
    public var remotePort: String
    public init(_ rule: Forward = Forward()) {
        self.rule = rule
        localPort = String(rule.localPort); remotePort = String(rule.remotePort)
    }
    public func applying(to rules: [Forward], replacing id: String? = nil) throws -> [Forward] {
        func port(_ text: String) throws -> Int {
            guard !text.isEmpty, text.utf8.allSatisfy({ (48...57).contains($0) }),
                  let number = Int(text), (1...65535).contains(number) else {
                throw AppError("포트를 1–65535 사이의 숫자로 입력하세요.")
            }
            return number
        }
        var value = rule
        value.localPort = try port(localPort); value.remotePort = try port(remotePort)
        var result = rules
        if let id {
            guard let index = result.firstIndex(where: { $0.id == id }) else { throw AppError("편집할 규칙을 찾을 수 없습니다.") }
            value.id = id; result[index] = value
        } else { result.append(value) }
        // Validate rules independently of unfinished host identity fields.
        try Host(id: "validation", name: "validation", target: "localhost", forwards: result).validate()
        return result
    }
}

public enum ForwardMenuState: String, CaseIterable {
    case preparing, ready, failed, disconnected, unknown, disabled
    public var title: String {
        switch self {
        case .preparing: return "준비 중"
        case .ready: return "준비됨"
        case .failed: return "개설 실패"
        case .disconnected: return "연결 끊김"
        case .unknown: return "확인 불가"
        case .disabled: return "비활성"
        }
    }
    public var symbol: String {
        switch self {
        case .preparing: return "arrow.triangle.2.circlepath"
        case .ready: return "checkmark.circle.fill"
        case .failed: return "xmark.circle.fill"
        case .disconnected: return "circle"
        case .unknown: return "questionmark.circle"
        case .disabled: return "minus.circle"
        }
    }
}

public struct ForwardMenuRow: Equatable {
    public var id: String
    public var text: String
    public var state: ForwardMenuState
    public var detail: String
}

public enum HostMenuState: Equatable {
    case connected, disconnected, disabled
    public var symbol: String {
        switch self {
        case .connected: return "checkmark.circle.fill"
        case .disconnected: return "circle"
        case .disabled: return "minus.circle"
        }
    }
    public var title: String {
        switch self {
        case .connected: return "연결됨"
        case .disconnected: return "연결 안 됨"
        case .disabled: return "자동 연결 꺼짐, 연결 안 됨"
        }
    }
}

public struct HostMenuRow {
    public var title: String
    public var state: HostMenuState
}

public enum TrayPresentation {
    /// Host icons describe SSH connectivity only; forwarding results belong to the child rows.
    public static func host(_ host: Host, runtime: HostRuntime?) -> HostMenuRow {
        let state: HostMenuState = runtime?.state == .connected ? .connected : (host.autoConnect ? .disconnected : .disabled)
        let busy = runtime?.state == .connecting || runtime?.state == .stopping
        return HostMenuRow(title: host.name + (runtime?.temporary == true ? " · 임시" : "") + (busy ? " · 작업 중…" : ""), state: state)
    }
    /// Use one execution snapshot, never a mixture of saved and currently executing rules.
    public static func forwards(host: Host, runtime: HostRuntime?) -> [ForwardMenuRow] {
        let execution = runtime?.executionHost
        let rules = (execution ?? host).forwards
        let observed = execution == nil ? [] : (runtime?.forwards ?? [])
        var rows = rules.map { rule -> ForwardMenuRow in
            let observation = observed.first { $0.id == rule.id && $0.source == "앱 설정" }
            let state: ForwardMenuState
            if !rule.enabled { state = .disabled }
            else if let observation { state = ForwardMenuState(rawValue: observation.state.rawValue) ?? .unknown }
            else if runtime?.state == .connecting, execution != nil { state = .preparing }
            else if runtime?.state == .connected, execution != nil { state = .unknown }
            else { state = .disconnected }
            return ForwardMenuRow(id: "app:" + rule.id,
                                  text: "\(rule.localPort) → \(rule.remoteHost):\(rule.remotePort) · 앱 설정",
                                  state: state, detail: observation?.detail ?? "")
        }
        rows += observed.filter { $0.source != "앱 설정" }.map {
            ForwardMenuRow(id: "inherited:" + $0.id, text: $0.description + " · " + $0.source,
                           state: ForwardMenuState(rawValue: $0.state.rawValue) ?? .unknown, detail: $0.detail)
        }
        return rows
    }
}
