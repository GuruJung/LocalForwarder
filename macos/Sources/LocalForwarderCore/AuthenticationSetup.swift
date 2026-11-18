import Foundation

public enum AuthenticationSetup {
    /// The command is constant; only public key bytes are supplied over stdin, never interpolated into shell code.
    public static let installCommand = #"""
    sh -c 'set -eu; umask 077; mkdir -p "$HOME/.ssh"; chmod 700 "$HOME/.ssh"; key=$(cat); blob=$(printf "%s\n" "$key" | awk "{print \$2}"); test -n "$blob"; touch "$HOME/.ssh/authorized_keys"; if ! awk -v wanted="$blob" "{for(i=1;i<=NF;i++) if (\$i == wanted) found=1} END {exit !found}" "$HOME/.ssh/authorized_keys"; then printf "\n%s\n" "$key" >> "$HOME/.ssh/authorized_keys"; fi; chmod 600 "$HOME/.ssh/authorized_keys"'
    """#
    public static func run(store: ConfigurationStore, profileID: String, configuration: String? = nil) throws {
        var host = try store.host(profileID)
        print(L10n.text("mac.set.up.automatic.login.for.name.verify.the.server", ["name": String(describing: host.name)]))
        print(L10n.text("mac.the.server.s.public.key.authentication.policy.and.custom"))
        let inspection = try ProcessTools.capture(SSHCommand.executable, SSHCommand.inspect(host, configuration: configuration))
        guard inspection.status == 0 else { throw AppError(inspection.output) }
        var candidates: [String] = []
        if let explicit = host.identityFile {
            let path = (explicit as NSString).expandingTildeInPath
            guard FileManager.default.fileExists(atPath: path) else { throw AppError(.key("mac.the.specified.private.key.was.not.found.path", ["path": String(describing: path)])) }
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
            print(L10n.text("mac.select.a.private.key"))
            for (index, path) in candidates.enumerated() { print("\(index + 1). \(path)") }
            guard let answer = readLine(), let choice = Int(answer), (1...candidates.count).contains(choice) else { throw AppError(.key("mac.key.selection.was.cancelled")) }
            key = candidates[choice - 1]
        } else {
            try FileManager.default.createDirectory(at: keyDirectory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            guard !FileManager.default.fileExists(atPath: generated + ".pub") else { throw AppError(.key("mac.an.existing.public.key.file.prevented.key.creation.generated", ["generated": String(describing: generated)])) }
            key = generated
            let status = try ProcessTools.interactive("/usr/bin/ssh-keygen", ["-t", "ed25519", "-N", "", "-f", key, "-C", "LocalForwarder/\(host.id)"])
            guard status == 0 else { throw AppError(.key("mac.could.not.generate.the.ssh.key")) }
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: key)
        }
        print(L10n.text("mac.selected.key.key", ["key": String(describing: key)]))
        var derived = try ProcessTools.capture("/usr/bin/ssh-keygen", ["-y", "-P", "", "-f", key])
        if derived.status != 0 {
            print(L10n.text("mac.the.private.key.is.encrypted.or.locked.enter.its"))
            guard try ProcessTools.interactive("/usr/bin/ssh-add", [key]) == 0 else { throw AppError(.key("mac.could.not.add.the.key.to.the.ssh.agent")) }
            derived = try publicKeyInteractively(key)
        }
        guard derived.status == 0 else { throw AppError(.key("mac.could.not.read.the.public.key.from.the.private")) }
        let parts = derived.output.trimmingCharacters(in: .whitespacesAndNewlines).split(separator: " ")
        guard parts.count >= 2, parts[0].hasPrefix("ssh-") || parts[0].hasPrefix("ecdsa-"), Data(base64Encoded: String(parts[1])) != nil else {
            throw AppError(.key("mac.could.not.obtain.a.valid.public.key"))
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
        // Interactive SSH writes diagnostics directly to the terminal. Do not retry an
        // unclassified failure (which could be authentication or host-key rejection).
        guard try ProcessTools.interactive(SSHCommand.executable, args, input: Data(publicKey.utf8)) == 0 else {
            throw AppError(.key("mac.could.not.install.the.public.key.check.the.terminal"))
        }
        let verification = SSHCommand.options(host, configuration: configuration) + ["-T", "-n", "-o", "SessionType=default", "-o", "ClearAllForwardings=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", host.target, "true"]
        var verified = false
        for attempt in 0..<2 {
            let result = try ProcessTools.capture(SSHCommand.executable, verification, timeout: 20)
            if result.status == 0 { verified = true; break }
            guard attempt == 0, host.retryOnConnectionRefused, SSHCommand.isConnectionRefusal(result.output) else {
                print(result.output); break
            }
            print(L10n.text("mac.connection.refused.retrying.verification.once.in.1.second"))
            Thread.sleep(forTimeInterval: 1)
        }
        guard verified else { throw AppError(.key("mac.the.public.key.was.installed.but.noninteractive.login.could")) }
        try store.setIdentity(key, for: profileID)
        print(L10n.text("mac.automatic.login.verified.select.connect.for.this.host.in"))
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
