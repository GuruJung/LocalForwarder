import Foundation
import Darwin
import LocalForwarderCore

/// A per-user advisory lock owns the socket for the lifetime of the GUI process.
/// The second launch exits only after the owner acknowledges its open request.
final class SingleInstance {
    private var lockFD: Int32 = -1
    private var listener: Int32 = -1
    private var source: DispatchSourceRead?
    private let directory = "/tmp/localforwarder-\(getuid())"
    private var socketPath: String { directory + "/gui.sock" }
    var onOpenSettings: (() -> Void)?

    func acquire() throws -> Bool {
        if mkdir(directory, 0o700) != 0 && errno != EEXIST { throw failure(L10n.text("mac.could.not.create.the.single.instance.directory")) }
        var metadata = stat()
        guard lstat(directory, &metadata) == 0, metadata.st_uid == getuid(),
              metadata.st_mode & mode_t(S_IFMT) == mode_t(S_IFDIR), metadata.st_mode & 0o077 == 0 else {
            throw AppError(.key("mac.check.the.owner.and.permissions.of.the.single.instance", ["directory": String(describing: directory)]))
        }
        lockFD = Darwin.open(directory + "/gui.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard lockFD >= 0 else { throw failure(L10n.text("mac.could.not.open.the.single.instance.lock")) }
        guard fstat(lockFD, &metadata) == 0, metadata.st_uid == getuid(), metadata.st_mode & mode_t(S_IFMT) == mode_t(S_IFREG) else {
            throw AppError(.key("mac.the.single.instance.lock.file.is.invalid"))
        }
        if flock(lockFD, LOCK_EX | LOCK_NB) != 0 {
            guard errno == EWOULDBLOCK else { throw failure(L10n.text("mac.could.not.acquire.the.single.instance.lock")) }
            close(lockFD); lockFD = -1
            for _ in 0..<60 {
                if requestSettings() { return false }
                Thread.sleep(forTimeInterval: 0.05)
            }
            throw AppError(.key("mac.the.running.localforwarder.did.not.respond.to.the.request"))
        }
        unlink(socketPath)
        listener = socket(AF_UNIX, SOCK_STREAM, 0)
        guard listener >= 0 else { throw failure(L10n.text("mac.could.not.create.the.settings.request.socket")) }
        _ = fcntl(listener, F_SETFD, FD_CLOEXEC)
        let bound = withAddress { Darwin.bind(listener, $0, $1) }
        guard bound == 0, chmod(socketPath, 0o600) == 0, listen(listener, 8) == 0 else {
            throw failure(L10n.text("mac.could.not.open.the.settings.request.socket"))
        }
        _ = fcntl(listener, F_SETFL, O_NONBLOCK)
        let descriptor = listener
        let source = DispatchSource.makeReadSource(fileDescriptor: descriptor, queue: .global(qos: .utility))
        source.setEventHandler { [weak self] in self?.acceptRequests(on: descriptor) }
        source.setCancelHandler { close(descriptor) }
        self.source = source
        source.resume()
        return true
    }

    private func withAddress<T>(_ body: (UnsafePointer<sockaddr>, socklen_t) -> T) -> T {
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let bytes = Array(socketPath.utf8) + [0]
        let capacity = MemoryLayout.size(ofValue: address.sun_path)
        precondition(bytes.count <= capacity)
        withUnsafeMutablePointer(to: &address.sun_path) { pointer in
            pointer.withMemoryRebound(to: UInt8.self, capacity: capacity) { destination in
                for (index, byte) in bytes.enumerated() { destination[index] = byte }
            }
        }
        return withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { body($0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
    }

    private func configure(_ fd: Int32) {
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
        let flags = fcntl(fd, F_GETFL)
        _ = fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)
        var timeout = timeval(tv_sec: 1, tv_usec: 0)
        _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        _ = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        var yes: Int32 = 1
        _ = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
    }
    private func send(_ message: String, to fd: Int32) -> Bool {
        let data = Array(message.utf8)
        return data.withUnsafeBytes { Darwin.write(fd, $0.baseAddress!, $0.count) == $0.count }
    }
    private func receive(_ fd: Int32) -> String {
        var result = [UInt8]()
        var byte: UInt8 = 0
        while result.count < 64 && Darwin.read(fd, &byte, 1) == 1 {
            result.append(byte)
            if byte == 10 { break }
        }
        return String(decoding: result, as: UTF8.self)
    }
    private func requestSettings() -> Bool {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        configure(fd)
        guard withAddress({ Darwin.connect(fd, $0, $1) }) == 0,
              send("OPEN_SETTINGS\n", to: fd) else { return false }
        return receive(fd) == "OK\n"
    }
    private func acceptRequests(on descriptor: Int32) {
        while true {
            let fd = accept(descriptor, nil, nil)
            guard fd >= 0 else { return }
            configure(fd)
            DispatchQueue.global(qos: .utility).async { [weak self] in
                guard let self else { close(fd); return }
                guard self.receive(fd) == "OPEN_SETTINGS\n" else { close(fd); return }
                DispatchQueue.main.async {
                    self.onOpenSettings?()
                    _ = self.send("OK\n", to: fd)
                    close(fd)
                }
            }
        }
    }
    func stop() {
        onOpenSettings = nil
        if let source { source.cancel(); self.source = nil }
        else if listener >= 0 { close(listener) }
        if listener >= 0 { listener = -1; unlink(socketPath) }
        if lockFD >= 0 { _ = flock(lockFD, LOCK_UN); close(lockFD); lockFD = -1 }
    }
    private func failure(_ message: String) -> AppError { AppError(message + " " + String(cString: strerror(errno))) }
    deinit { stop() }
}
