import Foundation
#if canImport(Darwin)
import Darwin
#endif

public enum ConnectionState: String { case disconnected, connecting, connected, stopping }
public enum SessionEffect: Equatable { case launch, stop }
public enum DeadlineKind { case attempt, retry }
public struct SessionDeadline {
    public let time: TimeInterval
    public let kind: DeadlineKind
}
/// One connection request, with at most one explicitly classified pre-authentication retry.
/// Recovery events and user intent belong to ConnectionManager, never to this machine.
public struct ConnectionMachine {
    public private(set) var state: ConnectionState = .disconnected
    public private(set) var attempt = 0
    public private(set) var deadline: SessionDeadline?
    public init() {}
    public mutating func begin(now: TimeInterval, reconnect: Bool = false) -> [SessionEffect] {
        state = .connecting; attempt = 1
        deadline = SessionDeadline(time: now + 20, kind: .attempt)
        return [.stop, .launch]
    }
    public mutating func ready() {
        guard state == .connecting else { return }
        state = .connected; deadline = nil
    }
    public mutating func failed(now: TimeInterval, retryable: Bool = false) -> [SessionEffect] {
        guard state == .connecting || state == .connected else { return [] }
        if state == .connecting && attempt == 1 && retryable {
            attempt = 2; deadline = SessionDeadline(time: now + 1, kind: .retry)
        } else { state = .disconnected; deadline = nil }
        return [.stop]
    }
    public mutating func manualReconnect(now: TimeInterval) -> [SessionEffect] {
        guard state == .connected || state == .disconnected else { return [] }
        return begin(now: now)
    }
    public mutating func tick(now: TimeInterval) -> [SessionEffect] {
        guard let deadline, now >= deadline.time else { return [] }
        self.deadline = nil
        switch deadline.kind {
        case .attempt: return failed(now: now)
        case .retry:
            self.deadline = SessionDeadline(time: now + 20, kind: .attempt)
            return [.launch]
        }
    }
    public mutating func quit() -> [SessionEffect] { state = .stopping; deadline = nil; return [.stop] }
    public mutating func stopped() { state = .disconnected; deadline = nil }
}
public enum ContinuousTime {
    public static func now() -> TimeInterval {
        #if canImport(Darwin)
        var info = mach_timebase_info_data_t(); mach_timebase_info(&info)
        return Double(mach_continuous_time()) * Double(info.numer) / Double(info.denom) / 1_000_000_000
        #else
        return ProcessInfo.processInfo.systemUptime
        #endif
    }
}
