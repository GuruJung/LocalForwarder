import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

public struct CommandResult { public let status: Int32; public let output: String }
private final class CaptureCleanup {
    let process: Process
    private let lock = NSLock()
    private var finished = false
    init(_ process: Process) { self.process = process }
    func finish() {
        lock.lock(); defer { lock.unlock() }
        guard !finished else { return }
        ProcessTools.stopAndWait(process)
        finished = true
    }
}
public enum ProcessTools {
    private static let capturesLock = NSLock()
    private static var captures: [ObjectIdentifier: CaptureCleanup] = [:]
    private static var shuttingDown = false

    /// Called off the UI thread during final shutdown, including outstanding ssh -G inspections.
    public static func cancelCapturedProcesses() {
        capturesLock.lock()
        shuttingDown = true
        let pending = Array(captures.values)
        capturesLock.unlock()
        for task in pending { requestStop(task.process) }
        for task in pending { task.finish() }
    }
    /// The application helper establishes an owned process group BEFORE exec, including ProxyCommand descendants.
    public static var sshLauncher: String?
    public static func configure(_ process: Process, executable: String, arguments: [String]) {
        if executable == SSHCommand.executable, let launcher = sshLauncher {
            process.executableURL = URL(fileURLWithPath: launcher); process.arguments = ["--run-ssh"] + arguments
        } else { process.executableURL = URL(fileURLWithPath: executable); process.arguments = arguments }
    }
    public static func environment(interactive: Bool = false) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        env["LC_ALL"] = "C"
        if !interactive { env["SSH_ASKPASS_REQUIRE"] = "never"; env.removeValue(forKey: "SSH_ASKPASS") }
        return env
    }
    public static func requestStop(_ process: Process) {
        let pid = process.processIdentifier
        guard pid > 0 else { return }
        if process.arguments?.first == "--run-ssh" || (process.isRunning && getpgid(pid) == pid) {
            if kill(-pid, SIGTERM) != 0, process.isRunning { process.terminate() }
        } else if process.isRunning { process.terminate() }
    }
    /// Only called on worker queues. Wait for the owned process group, not just its SSH leader.
    public static func stopAndWait(_ process: Process) {
        let pid = process.processIdentifier
        guard pid > 0 else { return }
        let ownsGroup = process.arguments?.first == "--run-ssh" || (process.isRunning && getpgid(pid) == pid)
        requestStop(process)
        let end = ContinuousTime.now() + 1
        while (process.isRunning || (ownsGroup && kill(-pid, 0) == 0)) && ContinuousTime.now() < end {
            Thread.sleep(forTimeInterval: 0.01)
        }
        if ownsGroup && kill(-pid, 0) == 0 { _ = kill(-pid, SIGKILL) }
        else if process.isRunning { _ = kill(pid, SIGKILL) }
        if process.isRunning { process.waitUntilExit() }
    }
    public static func stop(_ process: Process) {
        requestStop(process)
        DispatchQueue.global().async { stopAndWait(process) }
    }
    public static func capture(_ executable: String, _ arguments: [String], timeout: Double = 12) throws -> CommandResult {
        let process = Process(); configure(process, executable: executable, arguments: arguments)
        process.environment = environment(); process.standardInput = FileHandle.nullDevice
        let pipe = Pipe(); process.standardOutput = pipe; process.standardError = pipe
        let cleanup = CaptureCleanup(process)
        capturesLock.lock()
        guard !shuttingDown else { capturesLock.unlock(); throw AppError("앱을 종료하고 있습니다.") }
        do { try process.run() } catch { capturesLock.unlock(); throw error }
        captures[ObjectIdentifier(process)] = cleanup
        capturesLock.unlock()
        _ = setpgid(process.processIdentifier, process.processIdentifier)
        let limit = DispatchWorkItem { cleanup.finish() }
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout, execute: limit)
        defer {
            limit.cancel()
            cleanup.finish()
            capturesLock.lock(); captures.removeValue(forKey: ObjectIdentifier(process)); capturesLock.unlock()
        }
        var bytes = Data(); var exceeded = false
        while true {
            let data = pipe.fileHandleForReading.readData(ofLength: 8192)
            if data.isEmpty { break }
            if bytes.count + data.count <= 1_048_576 { bytes.append(data) }
            else { exceeded = true; cleanup.finish() }
        }
        process.waitUntilExit(); limit.cancel()
        if exceeded { throw AppError("SSH 진단 출력이 허용 크기를 초과했습니다.") }
        return CommandResult(status: process.terminationStatus, output: String(decoding: bytes, as: UTF8.self))
    }
    /// Foundation may launch a new process group. Give interactive tools the terminal and restore it afterwards.
    public static func foreground(_ process: Process) -> () -> Void {
        let fd = open("/dev/tty", O_RDWR)
        guard fd >= 0 else { return {} }
        let previousGroup = tcgetpgrp(fd)
        let group = getpgid(process.processIdentifier)
        guard previousGroup > 0, group > 0, group != previousGroup else { _ = close(fd); return {} }
        let previousSignal = signal(SIGTTOU, SIG_IGN)
        if tcsetpgrp(fd, group) == 0 { _ = kill(-group, SIGCONT) }
        return {
            _ = tcsetpgrp(fd, previousGroup)
            _ = signal(SIGTTOU, previousSignal)
            _ = close(fd)
        }
    }
    @discardableResult public static func interactive(_ executable: String, _ arguments: [String], input: Data? = nil) throws -> Int32 {
        let process = Process(); process.executableURL = URL(fileURLWithPath: executable); process.arguments = arguments
        process.environment = environment(interactive: true)
        process.standardOutput = FileHandle.standardOutput; process.standardError = FileHandle.standardError
        var pipe: Pipe?
        if input != nil { let inputPipe = Pipe(); pipe = inputPipe; process.standardInput = inputPipe } else { process.standardInput = FileHandle.standardInput }
        try process.run()
        let restore = foreground(process); defer { restore(); if process.isRunning { stopAndWait(process) } }
        if let input, let pipe { try pipe.fileHandleForWriting.write(contentsOf: input); try pipe.fileHandleForWriting.close() }
        process.waitUntilExit(); return process.terminationStatus
    }
}
