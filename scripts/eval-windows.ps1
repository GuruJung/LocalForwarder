param([ValidateSet('x64','arm64')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
& python scripts/version.py --check
if ($LASTEXITCODE -ne 0) { throw 'Product version check failed.' }
& python -m unittest discover -s tests -p 'test_release*.py'
if ($LASTEXITCODE -ne 0) { throw 'Release tests failed.' }
& python scripts/localization.py --check
if ($LASTEXITCODE -ne 0) { throw 'Localization catalog check failed.' }
& python -m unittest discover -s tests -p 'test_localization*.py'
if ($LASTEXITCODE -ne 0) { throw 'Localization tests failed.' }
$productVersion = & python scripts/version.py --print
if ($LASTEXITCODE -ne 0) { throw 'Invalid product version.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools or Community with C++ tools is required.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { $vs = & $vswhere -latest -products '*' -property installationPath }
if (-not $vs) { throw 'MSVC installation was not found.' }
$hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$bootstrap = Join-Path ([System.IO.Path]::GetTempPath()) ("localforwarder-msvc-" + [guid]::NewGuid().ToString('N') + '.cmd')
try {
    @(
        '@echo off'
        "call `"$devcmd`" -no_logo -arch=$Architecture -host_arch=$hostArch >nul"
        'if errorlevel 1 exit /b 1'
        'set'
    ) | Set-Content -LiteralPath $bootstrap -Encoding ascii
    $environment = & $env:ComSpec /d /c $bootstrap
    if ($LASTEXITCODE -ne 0) { throw 'VsDevCmd failed.' }
} finally {
    Remove-Item -LiteralPath $bootstrap -Force -ErrorAction SilentlyContinue
}
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$build = Join-Path $root "build/windows-$Architecture"
& cmake -S windows -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $build --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
New-Item -ItemType Directory -Path dist -Force | Out-Null
& ctest --test-dir $build --output-on-failure --output-junit (Join-Path $root "dist/windows-$Architecture-tests.xml")
if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
$app = Join-Path $build 'LocalForwarder.exe'
$versionInfo = (Get-Item $app).VersionInfo
if ($versionInfo.FileVersion -ne $productVersion -or $versionInfo.ProductVersion -ne $productVersion) { throw 'Executable string version does not match VERSION.' }
$parts = $productVersion.Split('.')
if ($versionInfo.FileMajorPart -ne [int]$parts[0] -or $versionInfo.FileMinorPart -ne [int]$parts[1] -or $versionInfo.FileBuildPart -ne [int]$parts[2] -or $versionInfo.FilePrivatePart -ne 0 -or
    $versionInfo.ProductMajorPart -ne [int]$parts[0] -or $versionInfo.ProductMinorPart -ne [int]$parts[1] -or $versionInfo.ProductBuildPart -ne [int]$parts[2] -or $versionInfo.ProductPrivatePart -ne 0) { throw 'Executable numeric version does not match VERSION.' }
$env:PYTHONIOENCODING = 'utf-8'
& python tests/integration.py --app $app
if ($LASTEXITCODE -ne 0) { throw 'SSH integration tests failed.' }
New-Item -ItemType Directory -Path dist -Force | Out-Null
$zip = Join-Path $root "dist/LocalForwarder-$productVersion-windows-$Architecture.zip"
$stage = Join-Path $build 'package'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item $app,LICENSE,README.md,README_ko.md $stage -Force
Copy-Item third_party/yyjson/LICENSE (Join-Path $stage 'yyjson-LICENSE.txt') -Force
& python scripts/package-info.py --platform windows --arch $Architecture --executable $app --output (Join-Path $stage 'build-info.json')
if ($LASTEXITCODE -ne 0) { throw 'Package identity generation failed.' }
Compress-Archive -Path "$stage/*" -DestinationPath $zip -Force
Get-Item $app,$zip | Select-Object Name,Length | Format-Table
@{
    version = $productVersion
    architecture = $Architecture
    revision = (& git rev-parse HEAD)
    executable_bytes = (Get-Item $app).Length
    archive_bytes = (Get-Item $zip).Length
    behavior = 'multi-host-v2'
    native_desktop_manual_verification = 'not performed by this script'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root "dist/windows-$Architecture-build-evidence.json") -Encoding utf8
