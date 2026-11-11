import Foundation

public enum AuthenticationSetup {
    /// The command is constant; only public key bytes are supplied over stdin, never interpolated into shell code.
    public static let installCommand = #"""
    sh -c 'set -eu; umask 077; mkdir -p "$HOME/.ssh"; chmod 700 "$HOME/.ssh"; key=$(cat); blob=$(printf "%s\n" "$key" | awk "{print \$2}"); test -n "$blob"; touch "$HOME/.ssh/authorized_keys"; if ! awk -v wanted="$blob" "{for(i=1;i<=NF;i++) if (\$i == wanted) found=1} END {exit !found}" "$HOME/.ssh/authorized_keys"; then printf "\n%s\n" "$key" >> "$HOME/.ssh/authorized_keys"; fi; chmod 600 "$HOME/.ssh/authorized_keys"'
    """#
    public static func run(store: ConfigurationStore, profileID: String, configuration: String? = nil) throws {
        var host = try store.host(profileID)
        print("\(host.name)의 자동 로그인 설정입니다. 서버 지문을 확인하고, 필요한 비밀번호는 이 터미널에 입력하세요.")
        print("서버의 공개키 인증 정책 및 사용자 정의 AuthorizedKeysFile은 변경하지 않습니다.")
        let inspection = try ProcessTools.capture(SSHCommand.executable, SSHCommand.inspect(host, configuration: configuration))
        guard inspection.status == 0 else { throw AppError(inspection.output) }
        var candidates: [String] = []
        if let explicit = host.identityFile {
            let path = (explicit as NSString).expandingTildeInPath
            guard FileManager.default.fileExists(atPath: path) else { throw AppError("지정된 개인키를 찾을 수 없습니다: \(path)") }
            candidates = [path]
        } else {
            for line in inspection.output.components(separatedBy: .newlines) where line.hasPrefix("identityfile ") {
                let path = (String(line.dropFirst("identityfile ".count)) as NSString).expandingTildeInPath
                if FileManager.default.fileExists(atPath: path), !candidates.contains(path) { candidates.append(path) }
            }
        }
        let keyDirectory = store.directory.appendingPathComponent("keys")
        let generated = keyDirectory.appendingPathComponent(host.id).path
        if candidates.isEmpty, FileManager.default.fileExists(atPath: generated) { candidates.append(generated) }
        var key: String
        if candidates.count == 1 { key = candidates[0] }
        else if candidates.count > 1 {
            print("사용할 개인키를 선택하세요:")
            for (index, path) in candidates.enumerated() { print("\(index + 1). \(path)") }
            guard let answer = readLine(), let choice = Int(answer), (1...candidates.count).contains(choice) else { throw AppError("키 선택을 취소했습니다.") }
            key = candidates[choice - 1]
        } else {
            try FileManager.default.createDirectory(at: keyDirectory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            guard !FileManager.default.fileExists(atPath: generated + ".pub") else { throw AppError("기존 공개키 파일이 있어 새 키를 만들지 않았습니다: \(generated).pub") }
            key = generated
            let status = try ProcessTools.interactive("/usr/bin/ssh-keygen", ["-t", "ed25519", "-N", "", "-f", key, "-C", "LocalForwarder/\(host.id)"])
            guard status == 0 else { throw AppError("SSH 키 생성에 실패했습니다.") }
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: key)
        }
        print("사용할 키: \(key)")
        var derived = try ProcessTools.capture("/usr/bin/ssh-keygen", ["-y", "-P", "", "-f", key])
        if derived.status != 0 {
            print("기존 개인키가 암호화되어 있거나 잠겨 있습니다. 키 암호를 입력하고 SSH 에이전트에 등록합니다.")
            guard try ProcessTools.interactive("/usr/bin/ssh-add", [key]) == 0 else { throw AppError("SSH 에이전트에 키를 등록하지 못했습니다. ssh-agent 상태를 확인하세요.") }
            derived = try publicKeyInteractively(key)
        }
        guard derived.status == 0 else { throw AppError("개인키에서 공개키를 읽지 못했습니다.") }
        let parts = derived.output.trimmingCharacters(in: .whitespacesAndNewlines).split(separator: " ")
        guard parts.count >= 2, parts[0].hasPrefix("ssh-") || parts[0].hasPrefix("ecdsa-"), Data(base64Encoded: String(parts[1])) != nil else {
            throw AppError("유효한 공개키를 얻지 못했습니다.")
        }
        var publicKey = "\(parts[0]) \(parts[1])\n"
        if let original = try? String(contentsOfFile: key + ".pub", encoding: .utf8) {
            let line = original.trimmingCharacters(in: .whitespacesAndNewlines)
            let fields = line.split(separator: " ")
            if !line.contains("\n"), fields.count >= 2, fields[0] == parts[0], fields[1] == parts[1] { publicKey = line + "\n" }
        }
        // Registration may use an existing configured key or a terminal password before the selected key is authorized.
        host.identityFile = key
        let args = SSHCommand.options(host, configuration: configuration) + ["-T", "-o", "SessionType=default", "-o", "ClearAllForwardings=yes", "-o", "BatchMode=no", "-o", "StrictHostKeyChecking=ask", host.target, installCommand]
        var registered = false
        for attempt in 0..<2 {
            if try ProcessTools.interactive(SSHCommand.executable, args, input: Data(publicKey.utf8)) == 0 { registered = true; break }
            if attempt == 0 { print("첫 연결이 실패하여 1초 뒤 다시 시도합니다."); Thread.sleep(forTimeInterval: 1) }
        }
        guard registered else { throw AppError("공개키 등록에 실패했습니다. 서버 권한과 인증 정책을 확인하세요.") }
        let verification = SSHCommand.options(host, configuration: configuration) + ["-T", "-n", "-o", "SessionType=default", "-o", "ClearAllForwardings=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", host.target, "true"]
        var verified = false
        for attempt in 0..<2 {
            let result = try ProcessTools.capture(SSHCommand.executable, verification, timeout: 20)
            if result.status == 0 { verified = true; break }
            if attempt == 0 { Thread.sleep(forTimeInterval: 1) }
            else { print(result.output) }
        }
        guard verified else { throw AppError("공개키는 등록했지만 입력 없는 접속을 검증하지 못했습니다. 서버 정책을 확인하세요.") }
        try store.setIdentity(key, for: profileID)
        print("자동 로그인 검증을 완료했습니다. LocalForwarder를 다시 실행하세요.")
    }
    private static func publicKeyInteractively(_ key: String) throws -> CommandResult {
        let child = Process(); child.executableURL = URL(fileURLWithPath: "/usr/bin/ssh-keygen"); child.arguments = ["-y", "-f", key]
        child.standardInput = FileHandle.standardInput; child.standardError = FileHandle.standardError
        let pipe = Pipe(); child.standardOutput = pipe; child.environment = ProcessTools.environment(interactive: true)
        try child.run(); let restore = ProcessTools.foreground(child); defer { restore() }
        let data = pipe.fileHandleForReading.readDataToEndOfFile(); child.waitUntilExit()
        return CommandResult(status: child.terminationStatus, output: String(decoding: data, as: UTF8.self))
    }
}
