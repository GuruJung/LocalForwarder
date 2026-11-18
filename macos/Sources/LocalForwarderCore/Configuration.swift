import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

public struct Forward: Codable, Equatable {
    public var id: String
    public var enabled: Bool
    public var localPort: Int
    public var remoteHost: String
    public var remotePort: Int
    public var description: String
    public init(id: String = UUID().uuidString, enabled: Bool = true, localPort: Int = 8080,
                remoteHost: String = "localhost", remotePort: Int = 80, description: String = "") {
        self.id = id; self.enabled = enabled; self.localPort = localPort
        self.remoteHost = remoteHost; self.remotePort = remotePort; self.description = description
    }
    private enum CodingKeys: String, CodingKey {
        case id, enabled, localPort, remoteHost, remotePort, description
    }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decode(String.self, forKey: .id)
        enabled = try values.decode(Bool.self, forKey: .enabled)
        localPort = try values.decode(Int.self, forKey: .localPort)
        remoteHost = try values.decode(String.self, forKey: .remoteHost)
        remotePort = try values.decode(Int.self, forKey: .remotePort)
        description = try values.decodeIfPresent(String.self, forKey: .description) ?? ""
    }
    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(id, forKey: .id)
        try values.encode(enabled, forKey: .enabled)
        try values.encode(localPort, forKey: .localPort)
        try values.encode(remoteHost, forKey: .remoteHost)
        try values.encode(remotePort, forKey: .remotePort)
        if !description.isEmpty { try values.encode(description, forKey: .description) }
    }
}
public struct Host: Codable, Equatable {
    public var id: String
    public var name: String
    public var description: String
    public var target: String
    public var user: String?
    public var port: Int?
    public var identityFile: String?
    public var forwards: [Forward]
    public var autoConnect: Bool
    public var exitOnForwardFailure: Bool
    public var retryOnConnectionRefused: Bool
    public init(id: String = UUID().uuidString, name: String = L10n.text("mac.new.host"), target: String = "",
                user: String? = nil, port: Int? = nil, identityFile: String? = nil,
                forwards: [Forward] = [], autoConnect: Bool = true,
                exitOnForwardFailure: Bool = true, retryOnConnectionRefused: Bool = true,
                description: String = "") {
        self.id = id; self.name = name; self.target = target; self.user = user
        self.description = description
        self.port = port; self.identityFile = identityFile; self.forwards = forwards
        self.autoConnect = autoConnect; self.exitOnForwardFailure = exitOnForwardFailure
        self.retryOnConnectionRefused = retryOnConnectionRefused
    }
    private enum CodingKeys: String, CodingKey {
        case id, name, description, target, user, port, identityFile, forwards
        case autoConnect, exitOnForwardFailure, retryOnConnectionRefused
    }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decode(String.self, forKey: .id)
        name = try values.decode(String.self, forKey: .name)
        description = try values.decodeIfPresent(String.self, forKey: .description) ?? ""
        target = try values.decode(String.self, forKey: .target)
        user = try values.decodeIfPresent(String.self, forKey: .user)
        port = try values.decodeIfPresent(Int.self, forKey: .port)
        identityFile = try values.decodeIfPresent(String.self, forKey: .identityFile)
        forwards = try values.decode([Forward].self, forKey: .forwards)
        autoConnect = try values.decodeIfPresent(Bool.self, forKey: .autoConnect) ?? true
        exitOnForwardFailure = try values.decodeIfPresent(Bool.self, forKey: .exitOnForwardFailure) ?? true
        retryOnConnectionRefused = try values.decodeIfPresent(Bool.self, forKey: .retryOnConnectionRefused) ?? true
    }
    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(id, forKey: .id)
        try values.encode(name, forKey: .name)
        if !description.isEmpty { try values.encode(description, forKey: .description) }
        try values.encode(target, forKey: .target)
        try values.encodeIfPresent(user, forKey: .user)
        try values.encodeIfPresent(port, forKey: .port)
        try values.encodeIfPresent(identityFile, forKey: .identityFile)
        try values.encode(forwards, forKey: .forwards)
        try values.encode(autoConnect, forKey: .autoConnect)
        try values.encode(exitOnForwardFailure, forKey: .exitOnForwardFailure)
        try values.encode(retryOnConnectionRefused, forKey: .retryOnConnectionRefused)
    }
    /// Host and rule descriptions are saved metadata, not changes to an executing connection.
    public func hasSameExecutionSettings(as other: Host) -> Bool {
        var lhs = self, rhs = other
        lhs.description = ""; rhs.description = ""
        for index in lhs.forwards.indices { lhs.forwards[index].description = "" }
        for index in rhs.forwards.indices { rhs.forwards[index].description = "" }
        return lhs == rhs
    }
    public func validate() throws {
        guard !description.unicodeScalars.contains(where: { [0, 10, 13, 0x85, 0x2028, 0x2029].contains($0.value) }) else {
            throw AppError(.key("mac.enter.a.description.on.one.line.without.newline.or"))
        }
        guard !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { throw AppError(.key("mac.enter.a.host.name")) }
        guard Self.validID(id) else { throw AppError(.key("mac.the.host.id.is.invalid")) }
        guard Self.validToken(target) else { throw AppError(.key("mac.enter.a.valid.ssh.alias.or.host.address")) }
        if let user, !Self.validToken(user) { throw AppError(.key("mac.the.ssh.username.is.invalid")) }
        if let port, !(1...65535).contains(port) { throw AppError(.key("mac.the.ssh.port.must.be.between.1.and.65535")) }
        if let key = identityFile, key.isEmpty || key.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) {
            throw AppError(.key("mac.the.private.key.path.is.invalid"))
        }
        var ports = Set<Int>()
        var ids = Set<String>()
        for forward in forwards {
            guard !forward.description.unicodeScalars.contains(where: { [0, 10, 13, 0x85, 0x2028, 0x2029].contains($0.value) }) else {
                throw AppError(.key("mac.enter.a.rule.description.on.one.line.without.newline"))
            }
            guard Self.validID(forward.id), ids.insert(forward.id).inserted else { throw AppError(.key("mac.a.rule.id.is.duplicated.or.invalid")) }
            guard (1...65535).contains(forward.localPort), (1...65535).contains(forward.remotePort), Self.validToken(forward.remoteHost) else {
                throw AppError(.key("mac.check.the.rule.s.ports.1.65535.and.destination"))
            }
            if forward.enabled && !ports.insert(forward.localPort).inserted { throw AppError(.key("mac.enabled.rules.contain.duplicate.local.ports")) }
        }
    }
    public static func validToken(_ value: String) -> Bool {
        !value.isEmpty && !value.hasPrefix("-") && !value.unicodeScalars.contains { CharacterSet.whitespacesAndNewlines.union(.controlCharacters).union(CharacterSet(charactersIn: "\"'\\;$`|&<>" )).contains($0) }
    }
    public static func validID(_ value: String) -> Bool {
        !value.isEmpty && value.count <= 128 && value.utf8.allSatisfy { (65...90).contains($0) || (97...122).contains($0) || (48...57).contains($0) || $0 == 45 || $0 == 95 }
    }
}
public struct Configuration: Codable, Equatable {
    public var version: Int = 2
    public var hosts: [Host] = []
    public init(hosts: [Host] = []) { self.hosts = hosts }
}
public struct AppError: LocalizedError {
    public let localizedMessage: LocalizedMessage
    public var message: String { localizedMessage.rendered }
    public init(_ message: String) { self.localizedMessage = .raw(message) }
    public init(_ message: LocalizedMessage) { self.localizedMessage = message }
    public var errorDescription: String? { message }
}
public final class ConfigurationStore {
    public let directory: URL
    public var file: URL { directory.appendingPathComponent("config.json") }
    // Serialize store mutations within this process as well as between the GUI and auth helper.
    private static let mutationLock = NSLock()
    public init(directory: URL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".LocalForwarder")) { self.directory = directory }
    public func load() throws -> Configuration {
        guard FileManager.default.fileExists(atPath: file.path) else { return Configuration() }
        let data = try Data(contentsOf: file)
        struct Header: Decodable { let version: Int }
        let version = try JSONDecoder().decode(Header.self, from: data).version
        guard version == 2 else { throw AppError(.key("mac.unsupported.configuration.version.the.existing.file.was.not.changed")) }
        let result = try JSONDecoder().decode(Configuration.self, from: data)
        var ids = Set<String>()
        for host in result.hosts {
            try host.validate()
            guard Host.validID(host.id), ids.insert(host.id).inserted else { throw AppError(.key("mac.the.configuration.contains.an.invalid.or.duplicate.host.id")) }
        }
        return result
    }
    public func host(_ id: String) throws -> Host {
        guard let host = try load().hosts.first(where: { $0.id == id }) else { throw AppError(.key("mac.the.host.was.deleted.or.was.not.found.in")) }
        return host
    }
    public func save(_ host: Host) throws {
        try host.validate()
        try mutate { configuration in
            if let index = configuration.hosts.firstIndex(where: { $0.id == host.id }) { configuration.hosts[index] = host }
            else { configuration.hosts.append(host) }
        }
    }
    public func delete(_ id: String) throws {
        try mutate { $0.hosts.removeAll { $0.id == id } }
    }
    public func setIdentity(_ path: String, for id: String) throws {
        try mutate { configuration in
            guard let index = configuration.hosts.firstIndex(where: { $0.id == id }) else {
                throw AppError(.key("mac.the.host.was.deleted.or.was.not.found.in"))
            }
            configuration.hosts[index].identityFile = path
            try configuration.hosts[index].validate()
        }
    }
    private func mutate(_ change: (inout Configuration) throws -> Void) throws {
        Self.mutationLock.lock(); defer { Self.mutationLock.unlock() }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let lockPath = directory.appendingPathComponent("config.lock").path
        let descriptor = open(lockPath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, mode_t(0o600))
        guard descriptor >= 0 else { throw fileError(.key("mac.could.not.open.the.configuration.lock.file")) }
        defer { _ = close(descriptor) }
        var info = stat()
        guard fstat(descriptor, &info) == 0, (info.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG), info.st_uid == getuid() else {
            throw AppError(.key("mac.the.configuration.lock.is.not.a.regular.file.owned"))
        }
        while flock(descriptor, LOCK_EX) != 0 {
            if errno != EINTR { throw fileError(.key("mac.could.not.lock.the.configuration")) }
        }
        defer { _ = flock(descriptor, LOCK_UN) }
        var configuration = try load()
        try change(&configuration)
        configuration.version = 2
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        let data = try encoder.encode(configuration)
        let temporary = directory.appendingPathComponent(".config-\(UUID().uuidString).tmp")
        try writeExclusive(data, to: temporary)
        defer { try? FileManager.default.removeItem(at: temporary) }
        guard rename(temporary.path, file.path) == 0 else { throw fileError(.key("mac.could.not.replace.the.configuration.file")) }
    }
    private func writeExclusive(_ data: Data, to destination: URL) throws {
        let descriptor = open(destination.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode_t(0o600))
        guard descriptor >= 0 else { throw fileError(.key("mac.could.not.create.the.configuration.file")) }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        do {
            try handle.write(contentsOf: data)
            try handle.synchronize()
            try handle.close()
        } catch {
            try? handle.close()
            try? FileManager.default.removeItem(at: destination)
            throw error
        }
    }
    private func fileError(_ message: LocalizedMessage) -> AppError {
        AppError(message + .raw(": " + String(cString: strerror(errno))))
    }
}
