param([Parameter(Mandatory=$true)][ValidateSet('x64','arm64')][string]$Architecture)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or -not $env:RUNNER_TEMP) {
    throw 'This fixture preparation is restricted to disposable GitHub Actions runners.'
}
# This SSH server is only a test fixture. It is never shipped with the app.
$asset = if ($Architecture -eq 'arm64') { 'OpenSSH-ARM64.zip' } else { 'OpenSSH-Win64.zip' }
$expectedHash = if ($Architecture -eq 'arm64') {
    'd2b1928225479fec9e0c938f527fdb5ed676446cca1bbcc5af84aca1f272842e'
} else {
    'b67ff8becd203502190ec42ac7787c24c2cd3325ed7700485fe7a1c83d2a7957'
}
$fixture = Join-Path $env:RUNNER_TEMP ('localforwarder-sshd-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$zip = Join-Path $fixture $asset
Invoke-WebRequest -Uri "https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/$asset" -OutFile $zip
if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'SSH test fixture checksum mismatch.'
}
Expand-Archive -Path $zip -DestinationPath $fixture
$server = @(Get-ChildItem -Path $fixture -Filter sshd.exe -Recurse)
if ($server.Count -ne 1) { throw 'Expected one test sshd executable.' }
"LOCALFORWARDER_TEST_SSHD=$($server[0].FullName)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
