import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

/// Main-queue interface. stop completes only after every process owned by this session has stopped.
public protocol SSHSessionProtocol: AnyObject {
    var state: ConnectionState { get }
    var lastError: String { get }
    var lastErrorMessage: LocalizedMessage { get }
    var forwards: [ForwardRuntime] { get }
    var recentErrors: [DestinationError] { get }
    var onChange: (() -> Void)? { get set }
    var onError: ((String) -> Void)? { get set }
    func start(_ host: Host)
    func stop(completion: @escaping () -> Void)
}
public extension SSHSessionProtocol {
    var recentErrors: [DestinationError] { [] }
    var lastErrorMessage: LocalizedMessage { .raw(lastError) }
}

/// Owns exactly one host snapshot. Saving or editing a profile cannot mutate a running attempt.
public final class SSHSession: SSHSessionProtocol {
    public private(set) var machine = ConnectionMachine()
    public private(set) var lastErrorMessage: LocalizedMessage = .raw("")
    public var lastError: String { lastErrorMessage.rendered }
    public var onChange: (() -> Void)?
    public var onError: ((String) -> Void)?
    public let sshConfiguration: String?
    private var host: Host?
    private var process: Process?
    private var generation = 0
    private var timer: DispatchSourceTimer?
    private var readiness = Readiness()
    private var tracker: ForwardTracker?
    private final class DiagnosticStream {
        let handle: FileHandle
        let source: DispatchSourceRead
        let targetProcess: Bool
        var bytes = Data()
        init(pipe: Pipe, targetProcess: Bool) {
            let handle = pipe.fileHandleForReading
            self.handle = handle; self.targetProcess = targetProcess
            let fd = handle.fileDescriptor
            _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
            source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: .main)
            source.setCancelHandler { try? handle.close() }
        }
    }
    private var diagnostic = ""
    private var targetDiagnostic = ""
    private var inspection: ProcessCancellation?
    private let worker = DispatchQueue(label: "LocalForwarder.host-process")
    private var streams: [DiagnosticStream] = []
    public init(sshConfiguration: String? = nil) { self.sshConfiguration = sshConfiguration }
    public var state: ConnectionState { machine.state }
    public var forwards: [ForwardRuntime] { tracker?.forwards ?? [] }
    public var recentErrors: [DestinationError] { tracker?.recentErrors ?? [] }
    public func start(_ host: Host) {
        guard state == .disconnected else {
            stop { [weak self] in self?.start(host) }; return
        }
        self.host = host; lastErrorMessage = .raw(""); tracker = ForwardTracker(host: host)
        apply(machine.begin(now: ContinuousTime.now()))
    }
    public func stop(completion: @escaping () -> Void) {
        timer?.cancel(); timer = nil
        _ = machine.quit(); stopOwned(); tracker?.disconnected(); onChange?()
        let token = generation
        // Retain self until cleanup completes, including deleted hosts and final app shutdown.
        worker.async { DispatchQueue.main.async {
            if token == self.generation { self.machine.stopped(); self.onChange?() }
            completion()
        } }
    }
    private func advanceTimer() {
        if machine.deadline.map({ $0.kind == .attempt && ContinuousTime.now() >= $0.time }) == true {
            report(.key("mac.ssh.connection.preparation.timed.out.after.20.seconds") + .raw(diagnostic))
        }
        apply(machine.tick(now: ContinuousTime.now()))
    }
    private func apply(_ effects: [SessionEffect]) {
        timer?.cancel(); timer = nil
        for effect in effects {
            switch effect { case .stop: stopOwned(); tracker?.disconnected(); case .launch: launch() }
        }
        if let deadline = machine.deadline {
            let timer = DispatchSource.makeTimerSource(queue: .main)
            timer.schedule(deadline: .now() + max(0.001, deadline.time - ContinuousTime.now()))
            timer.setEventHandler { [weak self] in self?.advanceTimer() }
            self.timer = timer; timer.resume()
        }
        onChange?()
    }
    private func stopOwned() {
        generation += 1
        inspection?.cancel(); inspection = nil
        for stream in streams { stream.source.cancel() }; streams = []
        guard let old = process else { return }
        process = nil
        ProcessTools.requestStop(old)
        worker.async { ProcessTools.stopAndWait(old) }
    }
    private func report(_ message: LocalizedMessage) { lastErrorMessage = SSHCommand.userMessageValue(message); onError?(lastError) }
    private func fail(_ text: String) { fail(.raw(text)) }
    private func fail(_ message: LocalizedMessage) {
        let text = message.rendered
        // Successful jump authentication must not hide a target's pre-authentication refusal.
        // Conversely, an explicit proxy authentication/configuration error must not be
        // mistaken for a retryable target close merely because its transport disappears.
        let retryable = host?.retryOnConnectionRefused == true && !readiness.authenticated && SSHCommand.isConnectionRefusal(targetDiagnostic.isEmpty ? text : targetDiagnostic, auxiliaryDiagnostic: text)
        report(message); apply(machine.failed(now: ContinuousTime.now(), retryable: retryable))
    }
    private func launch() {
        let token = generation
        guard let host else { fail(.key("mac.no.connection.host.is.selected")); return }
        diagnostic = ""; targetDiagnostic = ""; readiness = Readiness(requireForwards: false)
        tracker = ForwardTracker(host: host)
        let cancellation = ProcessCancellation(); inspection = cancellation
        worker.async { [weak self] in
            guard let self else { return }
            do {
                try host.validate()
                guard FileManager.default.isExecutableFile(atPath: SSHCommand.executable) else { throw AppError(.key("mac.system.openssh.was.not.found")) }
                let result = try ProcessTools.capture(SSHCommand.executable, SSHCommand.inspect(host, configuration: self.sshConfiguration), cancellation: cancellation)
                guard result.status == 0 else { throw AppError(result.output) }
                let inherited = SSHCommand.inheritedForwards(result.output)
                let settings = result.output.components(separatedBy: .newlines)
                let target = settings.first(where: { $0.hasPrefix("hostname ") }).map { String($0.dropFirst(9)) } ?? host.target
                let port = settings.first(where: { $0.hasPrefix("port ") }).flatMap { Int($0.dropFirst(5)) } ?? host.port ?? 22
                DispatchQueue.main.async { [weak self] in
                    guard let self, token == self.generation else { return }
                    self.inspection = nil
                    self.readiness = Readiness(requireForwards: false, expectedTarget: target, expectedPort: port)
                    self.tracker = ForwardTracker(host: host, inherited: inherited)
                    self.spawn(host, token: token); self.onChange?()
                }
            } catch {
                DispatchQueue.main.async { [weak self] in
                    guard let self, token == self.generation else { return }
                    self.inspection = nil; self.fail(.error(error))
                }
            }
        }
    }
    private func spawn(_ host: Host, token: Int) {
        let child = Process()
        // -E writes through OpenSSH's private logging fd, without redirecting stderr.
        // The ProxyJump/ProxyCommand child's stderr therefore cannot establish target readiness.
        ProcessTools.configure(child, executable: SSHCommand.executable, arguments: SSHCommand.connect(host, configuration: sshConfiguration))
        child.environment = ProcessTools.environment(); child.standardInput = FileHandle.nullDevice
        let targetPipe = Pipe(); let auxiliaryPipe = Pipe()
        child.standardOutput = targetPipe; child.standardError = auxiliaryPipe
        let readers = [DiagnosticStream(pipe: targetPipe, targetProcess: true), DiagnosticStream(pipe: auxiliaryPipe, targetProcess: false)]
        streams = readers
        for stream in readers {
            stream.source.setEventHandler { [weak self, weak stream] in
                guard let stream else { return }
                self?.drain(stream, token: token)
            }
            stream.source.resume()
        }
        child.terminationHandler = { [weak self] child in
            DispatchQueue.main.async { [weak self] in
                guard let self, token == self.generation else {
                    for stream in readers { stream.source.cancel() }; return
                }
                for stream in readers {
                    self.drain(stream, token: token)
                    if !stream.bytes.isEmpty { self.consume(Data([10]), from: stream, token: token) }
                    stream.source.cancel()
                }
                self.streams = []
                self.fail(self.diagnostic.isEmpty ? .key("mac.ssh.exited.terminationstatus", ["terminationStatus": String(child.terminationStatus)]) : .raw(self.diagnostic))
            }
        }
        do { process = child; try child.run(); _ = setpgid(child.processIdentifier, child.processIdentifier) }
        catch {
            for stream in readers { stream.source.cancel() }; streams = []
            process = nil; fail(.error(error))
        }
    }
    private func drain(_ stream: DiagnosticStream, token: Int) {
        guard token == generation else { return }
        var buffer = [UInt8](repeating: 0, count: 8192)
        while true {
            let count = read(stream.handle.fileDescriptor, &buffer, buffer.count)
            if count <= 0 { break }
            consume(Data(buffer.prefix(count)), from: stream, token: token)
        }
    }
    private func consume(_ data: Data, from stream: DiagnosticStream, token: Int) {
        guard token == generation else { return }
        stream.bytes.append(data)
        while let index = stream.bytes.firstIndex(of: 10) {
            let line = String(decoding: stream.bytes[..<index], as: UTF8.self); stream.bytes.removeSubrange(...index)
            diagnostic += line + "\n"
            if diagnostic.utf8.count > 65_536 { diagnostic = String(diagnostic.suffix(32_768)) }
            guard stream.targetProcess else { continue }
            targetDiagnostic += line + "\n"
            if targetDiagnostic.utf8.count > 65_536 { targetDiagnostic = String(targetDiagnostic.suffix(32_768)) }
            readiness.consume(line)
            if readiness.authenticated { tracker?.consume(line) }
            if readiness.ready && state == .connecting {
                lastErrorMessage = .raw(""); machine.ready(); tracker?.sessionReady(); apply([])
            } else { onChange?() }
            if let recent = tracker?.recentErrors.last, recent.message == line {
                let formatter = ISO8601DateFormatter()
                lastErrorMessage = .raw("\(formatter.string(from: recent.time)) · \(recent.message)"); onChange?()
            }
        }
        if stream.bytes.count > 65_536 { stream.bytes = Data(stream.bytes.suffix(32_768)) }
    }
}
