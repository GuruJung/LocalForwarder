import Foundation

/// Explicit editor action; equal ports preserve their original relative order, including disabled rules.
public enum ForwardOrder {
    public static func byLocalPort(_ rules: [Forward]) -> [Forward] {
        rules.enumerated().sorted { left, right in
            if left.element.localPort == right.element.localPort { return left.offset < right.offset }
            return left.element.localPort < right.element.localPort
        }.map(\.element)
    }
}

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
                throw AppError(.key("mac.enter.a.port.number.between.1.and.65535"))
            }
            return number
        }
        var value = rule
        value.localPort = try port(localPort); value.remotePort = try port(remotePort)
        var result = rules
        if let id {
            guard let index = result.firstIndex(where: { $0.id == id }) else { throw AppError(.key("mac.the.rule.to.edit.was.not.found")) }
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
        case .preparing: return L10n.text("mac.preparing")
        case .ready: return L10n.text("mac.ready")
        case .failed: return L10n.text("mac.failed.to.open")
        case .disconnected: return L10n.text("mac.disconnected")
        case .unknown: return L10n.text("mac.unknown")
        case .disabled: return L10n.text("mac.disabled")
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
        case .connected: return L10n.text("mac.connected")
        case .disconnected: return L10n.text("mac.disconnected.2")
        case .disabled: return L10n.text("mac.auto.connect.off.disconnected")
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
        return HostMenuRow(title: host.name + (runtime?.temporary == true ? L10n.text("mac.temporary") : "") + (busy ? L10n.text("mac.working") : ""), state: state)
    }
    /// Use one execution snapshot, never a mixture of saved and currently executing rules.
    public static func forwards(host: Host, runtime: HostRuntime?) -> [ForwardMenuRow] {
        let execution = runtime?.executionHost
        let rules = (execution ?? host).forwards
        let observed = execution == nil ? [] : (runtime?.forwards ?? [])
        var rows = rules.map { rule -> ForwardMenuRow in
            let observation = observed.first { $0.id == rule.id && $0.sourceMessage == .key("mac.app.configuration") }
            let state: ForwardMenuState
            if !rule.enabled { state = .disabled }
            else if let observation { state = ForwardMenuState(rawValue: observation.state.rawValue) ?? .unknown }
            else if runtime?.state == .connecting, execution != nil { state = .preparing }
            else if runtime?.state == .connected, execution != nil { state = .unknown }
            else { state = .disconnected }
            return ForwardMenuRow(id: "app:" + rule.id,
                                  text: L10n.text("mac.localport.remotehost.remoteport.app.configuration", ["localPort": String(describing: rule.localPort), "remoteHost": String(describing: rule.remoteHost), "remotePort": String(describing: rule.remotePort)]),
                                  state: state, detail: observation?.detail ?? "")
        }
        rows += observed.filter { $0.sourceMessage != .key("mac.app.configuration") }.map {
            ForwardMenuRow(id: "inherited:" + $0.id, text: $0.description + " · " + $0.source,
                           state: ForwardMenuState(rawValue: $0.state.rawValue) ?? .unknown, detail: $0.detail)
        }
        return rows
    }
}
