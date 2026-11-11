import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

/// All public methods and callbacks run on the main queue. The worker queue owns blocking cleanup/config inspection.
public final class SSHSession {
    public private(set) var machine = ConnectionMachine()
    public private(set) var lastError = ""
    public private(set) var hostName = ""
    public var onChange: ((ConnectionState) -> Void)?
    public var onError: ((String) -> Void)?
    public var onInitialFailure: ((String) -> Void)?
    public var onExit: (() -> Void)?
    public let store: ConfigurationStore
    public let sshConfiguration: String?
    private var profileID: String?
    private var process: Process?
    private var generation = 0
    private var timer: DispatchSourceTimer?
    private var readiness = Readiness()
    private var bytes = Data()
    private var diagnostic = ""
    private let worker = DispatchQueue(label: "LocalForwarder.process")
    private var readSource: DispatchSourceRead?
    public init(store: ConfigurationStore, sshConfiguration: String? = nil) {
        self.store = store; self.sshConfiguration = sshConfiguration
    }
    public var state: ConnectionState { machine.state }
    public func start(_ id: String) {
        guard state == .editing else { return }
        profileID = id; lastError = ""; apply(machine.begin(now: ContinuousTime.now()))
    }
    public func reconnect() { apply(machine.manualReconnect(now: ContinuousTime.now())) }
    public func quit() { apply(machine.quit()) }
    public func wake() { advanceTimer() }
    private func advanceTimer() {
        let timedOut = machine.deadline.map { $0.kind == .attempt && ContinuousTime.now() >= $0.time } ?? false
        if timedOut { report("SSH 연결 준비 시간이 20초를 초과했습니다.\n" + diagnostic) }
        apply(machine.tick(now: ContinuousTime.now()))
    }
    private func apply(_ effects: [SessionEffect]) {
        onChange?(state)
        timer?.cancel(); timer = nil
        if let deadline = machine.deadline {
            let timer = DispatchSource.makeTimerSource(queue: .main)
            timer.schedule(deadline: .now() + max(0.001, deadline.time - ContinuousTime.now()))
            timer.setEventHandler { [weak self] in self?.advanceTimer() }
            self.timer = timer; timer.resume()
        }
        for effect in effects {
            switch effect {
            case .stop: stopOwned()
            case .launch: launch()
            case .fatal:
                let error = lastError
                worker.async { [weak self] in DispatchQueue.main.async { self?.onInitialFailure?(error) } }
            case .exit:
                worker.async { [weak self] in DispatchQueue.main.async { self?.onExit?() } }
            }
        }
    }
    private func stopOwned() {
        generation += 1
        readSource?.cancel(); readSource = nil
        guard let old = process else { return }
        process = nil
        ProcessTools.requestStop(old)
        worker.async { ProcessTools.stopAndWait(old) }
    }
    private func report(_ text: String) { lastError = SSHCommand.userMessage(text); onError?(lastError) }
    private func fail(_ text: String) { report(text); apply(machine.failed(now: ContinuousTime.now())) }
    private func launch() {
        let token = generation
        guard let id = profileID else { fail("접속 호스트가 선택되지 않았습니다."); return }
        bytes = Data(); diagnostic = ""; readiness = Readiness()
        worker.async { [weak self] in
            guard let self else { return }
            do {
                let host = try self.store.host(id)
                guard FileManager.default.isExecutableFile(atPath: SSHCommand.executable) else { throw AppError("시스템 OpenSSH를 찾지 못했습니다. macOS 명령행 도구를 확인하세요.") }
                let result = try ProcessTools.capture(SSHCommand.executable, SSHCommand.inspect(host, configuration: self.sshConfiguration))
                guard result.status == 0 else { throw AppError(result.output) }
                let inherited = SSHCommand.inheritedForwards(result.output)
                guard host.forwards.contains(where: { $0.enabled }) || !inherited.isEmpty else { throw AppError("활성 포워딩 규칙이 없습니다.") }
                let remoteCount = inherited.filter { $0.hasPrefix("remoteforward ") }.count
                DispatchQueue.main.async { [weak self] in
                    guard let self, token == self.generation else { return }
                    self.hostName = host.name; self.readiness = Readiness(remoteExpected: remoteCount)
                    self.spawn(host, token: token)
                }
            } catch {
                DispatchQueue.main.async { [weak self] in
                    guard let self, token == self.generation else { return }
                    self.fail(error.localizedDescription)
                }
            }
        }
    }
    private func spawn(_ host: Host, token: Int) {
        let child = Process(); ProcessTools.configure(child, executable: SSHCommand.executable, arguments: SSHCommand.connect(host, configuration: sshConfiguration))
        child.environment = ProcessTools.environment(); child.standardInput = FileHandle.nullDevice; child.standardOutput = FileHandle.nullDevice
        let pipe = Pipe(); child.standardError = pipe
        let handle = pipe.fileHandleForReading
        let fd = handle.fileDescriptor
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
        do {
            process = child; try child.run(); _ = setpgid(child.processIdentifier, child.processIdentifier)
        } catch {
            source.cancel(); readSource = nil; process = nil; fail(error.localizedDescription)
        }
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
            let line = String(decoding: bytes[..<index], as: UTF8.self)
            bytes.removeSubrange(...index)
            diagnostic += line + "\n"
            if diagnostic.utf8.count > 65_536 { diagnostic = String(diagnostic.suffix(32_768)) }
            readiness.consume(line)
            if readiness.ready && (state == .connecting || state == .reconnecting) {
                lastError = ""; machine.ready(); apply([])
            }
        }
        if bytes.count > 65_536 { bytes = Data(bytes.suffix(32_768)) }
    }
}
