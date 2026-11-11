import Foundation

public struct Forward: Codable, Equatable {
    public var id: String
    public var enabled: Bool
    public var localPort: Int
    public var remoteHost: String
    public var remotePort: Int
    public init(id: String = UUID().uuidString, enabled: Bool = true, localPort: Int = 8080,
                remoteHost: String = "localhost", remotePort: Int = 80) {
        self.id = id; self.enabled = enabled; self.localPort = localPort
        self.remoteHost = remoteHost; self.remotePort = remotePort
    }
}
public struct Host: Codable, Equatable {
    public var id: String
    public var name: String
    public var target: String
    public var user: String?
    public var port: Int?
    public var identityFile: String?
    public var forwards: [Forward]
    public init(id: String = UUID().uuidString, name: String = "새 호스트", target: String = "",
                user: String? = nil, port: Int? = nil, identityFile: String? = nil,
                forwards: [Forward] = []) {
        self.id = id; self.name = name; self.target = target; self.user = user
        self.port = port; self.identityFile = identityFile; self.forwards = forwards
    }
    public func validate() throws {
        guard !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { throw AppError("호스트 이름을 입력하세요.") }
        guard Self.validID(id) else { throw AppError("호스트 ID가 올바르지 않습니다.") }
        guard Self.validToken(target) else { throw AppError("SSH 별칭 또는 호스트 주소를 올바르게 입력하세요.") }
        if let user, !Self.validToken(user) { throw AppError("SSH 사용자명이 올바르지 않습니다.") }
        if let port, !(1...65535).contains(port) { throw AppError("SSH 포트는 1–65535여야 합니다.") }
        if let key = identityFile, key.isEmpty || key.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) {
            throw AppError("개인키 경로가 올바르지 않습니다.")
        }
        var ports = Set<Int>()
        var ids = Set<String>()
        for forward in forwards {
            guard Self.validID(forward.id), ids.insert(forward.id).inserted else { throw AppError("규칙 ID가 중복되거나 잘못되었습니다.") }
            guard (1...65535).contains(forward.localPort), (1...65535).contains(forward.remotePort), Self.validToken(forward.remoteHost) else {
                throw AppError("규칙의 포트(1–65535)와 목적지 호스트를 확인하세요.")
            }
            if forward.enabled && !ports.insert(forward.localPort).inserted { throw AppError("활성 규칙에 중복된 로컬 포트가 있습니다.") }
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
    public var version: Int = 1
    public var hosts: [Host] = []
    public init(hosts: [Host] = []) { self.hosts = hosts }
}
public struct AppError: LocalizedError {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}
public final class ConfigurationStore {
    public let directory: URL
    public var file: URL { directory.appendingPathComponent("config.json") }
    public init(directory: URL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".LocalForwarder")) { self.directory = directory }
    public func load() throws -> Configuration {
        guard FileManager.default.fileExists(atPath: file.path) else { return Configuration() }
        let result = try JSONDecoder().decode(Configuration.self, from: Data(contentsOf: file))
        guard result.version == 1 else { throw AppError("지원하지 않는 설정 버전입니다. 기존 파일은 변경하지 않았습니다.") }
        var ids = Set<String>()
        for host in result.hosts {
            try host.validate()
            guard Host.validID(host.id), ids.insert(host.id).inserted else { throw AppError("설정 파일의 호스트 ID가 잘못되었거나 중복되었습니다.") }
        }
        return result
    }
    public func host(_ id: String) throws -> Host {
        guard let host = try load().hosts.first(where: { $0.id == id }) else { throw AppError("호스트가 삭제되었거나 설정에서 찾을 수 없습니다.") }
        try host.validate(); return host
    }
    public func save(_ host: Host) throws {
        try host.validate()
        var configuration = try load()
        if let index = configuration.hosts.firstIndex(where: { $0.id == host.id }) { configuration.hosts[index] = host }
        else { configuration.hosts.append(host) }
        try write(configuration)
    }
    public func delete(_ id: String) throws {
        var configuration = try load(); configuration.hosts.removeAll { $0.id == id }; try write(configuration)
    }
    public func setIdentity(_ path: String, for id: String) throws {
        var host = try self.host(id); host.identityFile = path; try save(host)
    }
    private func write(_ configuration: Configuration) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        try encoder.encode(configuration).write(to: file, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
    }
}
