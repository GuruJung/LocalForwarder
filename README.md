# LocalForwarder

English · [한국어](README_ko.md)

LocalForwarder is a small native app for managing SSH local forwarding on macOS and Windows. Keep several hosts connected from one menu bar or system tray icon. Your operating system's OpenSSH handles authentication and traffic; LocalForwarder provides the settings and connection controls.

## Download and run

| System | CPU | Download |
|---|---|---|
| macOS 15 or later | Apple Silicon ARM64 | `LocalForwarder-macos-arm64.zip` |
| Windows 11 | x64 | `LocalForwarder-windows-x64.zip` |
| Windows 11 | ARM64 | `LocalForwarder-windows-arm64.zip` |

Open a successful run in [GitHub Actions](https://github.com/GuruJung/LocalForwarder/actions/workflows/build.yml) and download your platform's package from **Artifacts**. Downloads require a GitHub login and are retained for 7 days. Extract both the artifact archive and the distribution ZIP inside it, then run the `.app` on macOS or `.exe` on Windows. No installer is required.

The macOS app is ad-hoc signed and is not notarized; Windows builds do not have a signing certificate. If macOS blocks launch, verify the source before allowing the app in **System Settings → Privacy & Security**. Windows may display a SmartScreen warning. Follow your organization's policies if execution is restricted.

macOS uses `/usr/bin/ssh`. Windows requires the system **OpenSSH Client 9.5 or later**, which you can check in PowerShell:

```powershell
& "$env:WINDIR\System32\OpenSSH\ssh.exe" -V
```

Update older Windows clients through Windows Update or [Microsoft's OpenSSH update instructions](https://learn.microsoft.com/en-us/troubleshoot/windows-server/system-management-components/upgrade-in-box-openssh-to-latest-openssh-release). The app does not install SSH, request administrator access, or change services. It does not use Git for Windows' SSH. Remote servers do not need to meet this Windows client version requirement.

## Quick start

The app stays in the menu bar or system tray. On first launch, or when launched with `--open-config`, it opens Settings. Launching it again opens the existing app's Settings.

1. Create a host and enter its name and SSH alias or address. Aliases from `~/.ssh/config` work. An optional one-line description can help you identify the host.
2. Set a username, SSH port, or private key path if needed. Blank connection fields use your SSH configuration.
3. Choose **New rule (`새 규칙…`)** and enter a local port, destination host, and destination port. Add an optional description. Edit with **Edit rule (`규칙 편집…`)** or a double-click; disable a rule with its checkbox. Confirming the rule updates the draft, not the saved file.
4. **Save** the selected host. Auto-connect, `ExitOnForwardFailure`, and retry on connection refusal are enabled by default.
5. Connect the host. Reconnecting with unsaved edits offers save and reconnect, reconnect without saving, or cancel. The Settings **OK** button does not save automatically.

For example, `15432 → 127.0.0.1:5432` makes `127.0.0.1:15432` on your computer reach `127.0.0.1:5432` **as seen by the SSH server**. Point your database client at the local address. App-defined rules listen only on IPv4 loopback (`127.0.0.1`). Existing SSH `LocalForward`, `RemoteForward`, and `DynamicForward` directives also run with their original binding addresses.

When starting through Settings, **OK** connects saved hosts with auto-connect enabled; **Cancel** enters tray mode without connecting. Normal startup with an existing valid configuration connects auto-connect hosts. Closing Settings or a connection failure does not exit the app. Use **Quit** in the tray menu to stop all connections and exit.

## Connections and the tray

The tray menu provides Settings, Launch at Login, Reconnect All, host menus, and Quit. Each host menu shows its SSH summary, forwarding rules and status icons, and connection actions. Disabled rules remain visible. Use **Connection Status (`연결 상태…`)** in Settings for detailed errors and differences between saved settings and the running connection.

Saving changes only the selected host and leaves active connections and other unsaved drafts intact. Changes take effect on the next connection. A connection made without saving keeps its temporary settings for manual reconnects and automatic recovery; cancelling later edits does not undo the running settings. Explicitly saving that host clears the temporary override for its next connection. A new host connected without saving exists only until the app exits.

**Reconnect All** clears temporary overrides and reconnects only saved hosts with auto-connect enabled. You can manually connect hosts with auto-connect disabled. Disconnecting manually prevents automatic recovery during the current run without changing the saved auto-connect option.

After waking from sleep or a network transition from unavailable to available, recovery runs **5 seconds after the last event**. Only disconnected hosts that should still be connected are retried. Healthy, connecting, and manually disconnected hosts are left alone. Going offline again cancels pending recovery. Opening Settings or focusing the app does not trigger recovery. A disconnection detected after that recovery waits for another event or a manual connection.

With retry on connection refusal enabled, an explicit refusal, early close, or reset before connection establishment gets **one retry after 1 second**. Authentication errors, host key errors, port collisions, and ordinary timeouts do not trigger that retry.

Forwarding states are preparing, ready, failed to open, disconnected, or unknown. **Ready means the forwarding was opened; it does not guarantee the destination database or web service is healthy.** The app does not poll destination services. Observed failures appear in recent errors. With `ExitOnForwardFailure` disabled, SSH can remain connected even when some or all forwarding rules fail.

## Launch at login

Check **Launch at Login (`로그인 자동 시작`)** in the tray menu to register the app for your current user; uncheck it to remove the registration. It is off by default and separate from each host's auto-connect option. Changing it does not interrupt current connections, and quitting the app keeps the registration.

- **macOS:** `등록 필요` means registration is needed. `승인 필요` means the registration is checked but needs system approval; selecting it opens the login item settings. Unchecking also removes a pending registration.
- **Windows:** the checkmark shows the app's registration. If Task Manager, Windows settings, or system policy disables startup separately, check those settings; the app does not override them.

Put the app in its intended folder before enabling this option. Moving or deleting it can break startup. After moving it, uncheck and recheck the option from the new location. With a valid configuration, login starts the app in the tray and connects auto-connect hosts. Missing or damaged configuration receives the usual setup or error handling. The OS determines when login startup occurs.

## Configuration file

Both platforms use `.LocalForwarder/config.json` in your actual user home directory. **Only configuration version 2 is supported.** For example:

```json
{
  "version": 2,
  "hosts": [{
    "id": "development",
    "name": "Development server",
    "description": "Development database access",
    "target": "dev-server",
    "autoConnect": true,
    "exitOnForwardFailure": true,
    "retryOnConnectionRefused": true,
    "forwards": [{
      "id": "database",
      "enabled": true,
      "localPort": 15432,
      "remoteHost": "127.0.0.1",
      "remotePort": 5432,
      "description": "PostgreSQL"
    }]
  }]
}
```

Optional host fields are `user`, `port`, `identityFile`, and `description`. Rules also accept `description`. Descriptions are single-line text without line breaks or NUL; missing, `null`, or empty descriptions mean no description, and empty values are omitted when saving. Rule descriptions appear in Settings and the rule editor. Descriptions are never SSH arguments and do not change connection behavior. Older v2 apps without description support may discard them when saving.

Host IDs must be unique, and rule IDs must be unique within a host; use letters, digits, hyphens, and underscores. Omitted `autoConnect`, `exitOnForwardFailure`, and `retryOnConnectionRefused` options default to enabled. Passwords and private key contents are not stored in this JSON file.

Settings reloads the file when the window opens and preserves drafts while switching hosts. Saves lock and atomically update the latest file, changing only the selected host. For edits to the same item, the last save wins. Damaged or unsupported files are preserved and are not used for automatic connections.

A v1 file is rejected without conversion or a new migration backup. To start fresh, quit the app and any authentication helpers, move the old `config.json` to a safe backup location, then reopen the app and recreate the settings. Existing backups are left untouched.

## Authentication and troubleshooting

Normal connections do not prompt for input. Use the selected host's authentication setup action in Settings to open an external terminal helper. It leaves the app and other connections running. After setup, connect the host yourself.

The helper prefers existing keys. If a new key is needed, it creates an unencrypted Ed25519 key under `.LocalForwarder/keys/`. For ordinary Linux/macOS accounts, it registers the public key in `~/.ssh/authorized_keys` while preserving existing keys and restrictions and avoiding duplicates. Passwords and existing key passphrases are entered only in the terminal. Server authentication policy and changed host keys are not bypassed. Automatic key registration for remote Windows accounts is not supported; connections to already configured Windows SSH servers are supported.

- **Authentication fails:** check the selected host's SSH alias, username, key, and host key message. On Windows, encrypted keys need a working `ssh-agent`; the terminal helper explains setup if it is unavailable. The app does not remove passphrases or silently replace your key.
- **SSH is connected but the service is unreachable:** inspect Connection Status, then check the destination address and service from the server's side. With `ExitOnForwardFailure` off, check for failed rules even when SSH is connected.
- **Port conflicts:** use distinct local ports for different hosts. Duplicate active ports within a host are rejected. Across hosts the app follows SSH's result; Windows OpenSSH may allow port reuse, so the destination receiving traffic is not guaranteed when ports overlap.
- **Recovery does not run after changing adapters:** Windows uses overall internal-network or Internet availability. An adapter change while the network remains available is not a recovery event. Reconnect manually if necessary.

## License

[MIT License](LICENSE). Windows JSON parsing uses [yyjson](https://github.com/ibireme/yyjson); its license is included in Windows packages as `yyjson-LICENSE.txt`.

## Build and test

Clone or download the source repository and run commands from its root. macOS requires Xcode, Python 3.11 or later, and system OpenSSH tools, including `sshd` for integration tests:

```sh
bash scripts/eval-macos.sh
```

Windows requires PowerShell 7, Visual Studio or Build Tools with C++ desktop tools for the target architecture, CMake 3.24 or later, Ninja, Python 3.11 or later, OpenSSH Client 9.5 or later, and OpenSSH Server tools for integration tests:

```powershell
pwsh -NoProfile -File scripts/eval-windows.ps1 -Architecture x64
# On ARM64 Windows:
pwsh -NoProfile -File scripts/eval-windows.ps1 -Architecture arm64
```

These commands build, test, and package the app for the current platform. ZIPs are written to `dist/` using the filenames in the download table; macOS also leaves `dist/LocalForwarder.app`, and Windows builds under `build/windows-<architecture>/`. Test reports are written under `dist/`.

Integration tests use temporary keys, configuration, and a loopback SSH server, and take several minutes. On macOS, running the isolated test server as a non-root user requires `sudo -n` permission; the test cannot answer a password prompt. On Windows, provide `sshd.exe` through the system OpenSSH installation or `LOCALFORWARDER_TEST_SSHD`. The Windows CI server preparation script is restricted to disposable GitHub runners; it is not a local machine setup command. Real macOS login-item registration tests also run only on disposable GitHub-hosted CI. Automated tests do not replace checking real sleep/network transitions and desktop behavior.

For product intent and development issues, see the [current specification](https://github.com/GuruJung/LocalForwarder/blob/main/docs/dev-plans/current-spec.md).
