import Foundation
#if canImport(Darwin)
import Darwin
#endif

public enum ConnectionState: String { case editing, connecting, connected, waitingReconnect, reconnecting, waitingExit, exiting }
public enum SessionEffect: Equatable { case launch, stop, fatal, exit }
public enum DeadlineKind { case attempt, retry, reconnect, exit }
public struct SessionDeadline {
    public let time: TimeInterval
    public let kind: DeadlineKind
}
/// Pure transition model. Time is supplied by the caller so boundaries and sleep/resume are testable.
public struct ConnectionMachine {
    public private(set) var state: ConnectionState = .editing
    public private(set) var attempt = 0
    public private(set) var deadline: SessionDeadline?
    public init() {}
    public mutating func begin(now: TimeInterval, reconnect: Bool = false) -> [SessionEffect] {
        guard state != .exiting else { return [] }
        state = reconnect ? .reconnecting : .connecting
        attempt = 1; deadline = SessionDeadline(time: now + 20, kind: .attempt)
        return [.stop, .launch]
    }
    public mutating func ready() {
        guard state == .connecting || state == .reconnecting else { return }
        state = .connected; deadline = nil
    }
    public mutating func failed(now: TimeInterval) -> [SessionEffect] {
        if state == .connected {
            state = .waitingReconnect; deadline = SessionDeadline(time: now + 60, kind: .reconnect); return [.stop]
        }
        guard state == .connecting || state == .reconnecting else { return [] }
        if attempt < 2 {
            attempt += 1; deadline = SessionDeadline(time: now + 1, kind: .retry); return [.stop]
        }
        if state == .connecting { state = .exiting; deadline = nil; return [.stop, .fatal] }
        state = .waitingExit; deadline = SessionDeadline(time: now + 60, kind: .exit); return [.stop]
    }
    public mutating func manualReconnect(now: TimeInterval) -> [SessionEffect] {
        guard state == .connected || state == .waitingReconnect || state == .waitingExit else { return [] }
        return begin(now: now, reconnect: true)
    }
    public mutating func tick(now: TimeInterval) -> [SessionEffect] {
        guard let deadline, now >= deadline.time else { return [] }
        self.deadline = nil
        switch deadline.kind {
        case .attempt: return failed(now: now)
        case .retry:
            self.deadline = SessionDeadline(time: now + 20, kind: .attempt); return [.launch]
        case .reconnect: return begin(now: now, reconnect: true)
        case .exit: return quit()
        }
    }
    public mutating func quit() -> [SessionEffect] { state = .exiting; deadline = nil; return [.stop, .exit] }
}
public enum ContinuousTime {
    public static func now() -> TimeInterval {
        #if canImport(Darwin)
        var info = mach_timebase_info_data_t(); mach_timebase_info(&info)
        return Double(mach_continuous_time()) * Double(info.numer) / Double(info.denom) / 1_000_000_000
        #else
        return Date().timeIntervalSinceReferenceDate
        #endif
    }
}
