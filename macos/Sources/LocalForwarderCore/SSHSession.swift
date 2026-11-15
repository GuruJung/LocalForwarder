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
    var forwards: [ForwardRuntime] { get }
    var recentErrors: [DestinationError] { get }
    var onChange: (() -> Void)? { get set }
    var onError: ((String) -> Void)? { get set }
    func start(_ host: Host)
    func stop(completion: @escaping () -> Void)
}
public extension SSHSessionProtocol { var recentErrors: [DestinationError] { [] } }

/// Owns exactly one host snapshot. Saving or editing a profile cannot mutate a running attempt.
public final class SSHSession: SSHSessionProtocol {
    public private(set) var machine = ConnectionMachine()
    public private(set) var lastError = ""
    public var onChange: (() -> Void)?
    public var onError: ((String) -> Void)?
    public let sshConfiguration: String?
    private var host: Host?
    private var process: Process?
    private var generation = 0
    private var timer: DispatchSourceTimer?
    private var readiness = Readiness()
    private var tracker: ForwardTracker?
    private var bytes = Data()
    private var diagnostic = ""
    private var inspection: ProcessCancellation?
    private let worker = DispatchQueue(label: "LocalForwarder.host-process")
    private var readSource: DispatchSourceRead?
    public init(sshConfiguration: String? = nil) { self.sshConfiguration = sshConfiguration }
    public var state: ConnectionState { machine.state }
    public var forwards: [ForwardRuntime] { tracker?.forwards ?? [] }
    public var recentErrors: [DestinationError] { tracker?.recentErrors ?? [] }
    public func start(_ host: Host) {
        guard state == .disconnected else {
            stop { [weak self] in self?.start(host) }; return
        }
        self.host = host; lastError = ""; tracker = ForwardTracker(host: host)
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
            report("SSH 연결 준비 시간이 20초를 초과했습니다.\n" + diagnostic)
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
        readSource?.cancel(); readSource = nil
        guard let old = process else { return }
        process = nil
        ProcessTools.requestStop(old)
        worker.async { ProcessTools.stopAndWait(old) }
    }
    private func report(_ text: String) { lastError = SSHCommand.userMessage(text); onError?(lastError) }
    private func fail(_ text: String) {
        let retryable = host?.retryOnConnectionRefused == true && !readiness.authenticated && SSHCommand.isConnectionRefusal(text)
        report(text); apply(machine.failed(now: ContinuousTime.now(), retryable: retryable))
    }
    private func launch() {
        let token = generation
        guard let host else { fail("접속 호스트가 선택되지 않았습니다."); return }
        bytes = Data(); diagnostic = ""; readiness = Readiness(requireForwards: false)
        tracker = ForwardTracker(host: host)
        let cancellation = ProcessCancellation(); inspection = cancellation
        worker.async { [weak self] in
            guard let self else { return }
            do {
                try host.validate()
                guard FileManager.default.isExecutableFile(atPath: SSHCommand.executable) else { throw AppError("시스템 OpenSSH를 찾지 못했습니다.") }
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
                    self.inspection = nil; self.fail(error.localizedDescription)
                }
            }
        }
    }
    private func spawn(_ host: Host, token: Int) {
        let child = Process(); ProcessTools.configure(child, executable: SSHCommand.executable, arguments: SSHCommand.connect(host, configuration: sshConfiguration))
        child.environment = ProcessTools.environment(); child.standardInput = FileHandle.nullDevice; child.standardOutput = FileHandle.nullDevice
        let pipe = Pipe(); child.standardError = pipe
        let handle = pipe.fileHandleForReading; let fd = handle.fileDescriptor
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: .main)
        source.setEventHandler { [weak self] in self?.drain(fd, token: token) }
        source.setCancelHandler { try? handle.close() }
        readSource = source; source.resume()
        child.terminationHandler = { [weak self] child in
            DispatchQueue.main.async { [weak self] in
                guard let self, token == self.generation else { source.cancel(); return }
                self.drain(fd, token: token)
                source.cancel(); self.readSource = nil
                if !self.bytes.isEmpty { self.consume(Data([10]), token: token) }
                self.fail(self.diagnostic.isEmpty ? "SSH가 종료되었습니다 (\(child.terminationStatus))." : self.diagnostic)
            }
        }
        do { process = child; try child.run(); _ = setpgid(child.processIdentifier, child.processIdentifier) }
        catch { source.cancel(); readSource = nil; process = nil; fail(error.localizedDescription) }
    }
    private func drain(_ fd: Int32, token: Int) {
        guard token == generation else { return }
        var buffer = [UInt8](repeating: 0, count: 8192)
        while true {
            let count = read(fd, &buffer, buffer.count)
            if count <= 0 { break }
            consume(Data(buffer.prefix(count)), token: token)
        }
    }
    private func consume(_ data: Data, token: Int) {
        guard token == generation else { return }
        bytes.append(data)
        while let index = bytes.firstIndex(of: 10) {
            let line = String(decoding: bytes[..<index], as: UTF8.self); bytes.removeSubrange(...index)
            diagnostic += line + "\n"
            if diagnostic.utf8.count > 65_536 { diagnostic = String(diagnostic.suffix(32_768)) }
            readiness.consume(line)
            // Ignore the ProxyJump child's listener/session diagnostics until the target authenticates.
            if readiness.authenticated { tracker?.consume(line) }
            if readiness.ready && state == .connecting {
                lastError = ""; machine.ready(); tracker?.sessionReady(); apply([])
            } else { onChange?() }
            if let recent = tracker?.recentErrors.last, recent.message == line {
                let formatter = ISO8601DateFormatter()
                lastError = "\(formatter.string(from: recent.time)) · \(recent.message)"; onChange?()
            }
        }
        if bytes.count > 65_536 { bytes = Data(bytes.suffix(32_768)) }
    }
}
