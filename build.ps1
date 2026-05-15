param(
    [string] $ZigPath = ""
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $Root "src\kongoria_voice_plugin.cpp"
$Dist = Join-Path $Root "dist"
$Dll = Join-Path $Dist "kongoria_voice.dll"
$Ini = Join-Path $Root "config\kongoria_voice.example.ini"
$InstallerDir = Join-Path $Root "installer"
$SetupRc = Join-Path $InstallerDir "kongoria_voice_setup.rc"
$SetupRes = Join-Path $Dist "kongoria_voice_setup.res"
$SetupSrc = Join-Path $InstallerDir "kongoria_voice_setup.c"
$SetupExe = Join-Path $Dist "KongoriaVoicePluginSetup.exe"

function Resolve-Zig {
    param([string] $Requested)

    $candidates = @()
    if ($Requested) {
        $candidates += $Requested
    }
    if ($env:ZIG_EXE) {
        $candidates += $env:ZIG_EXE
    }
    $candidates += Join-Path $Root "tools\zig-x86_64-windows-0.16.0\zig.exe"
    $candidates += Join-Path (Split-Path -Parent $Root) "tools\zig-x86_64-windows-0.16.0\zig.exe"
    $candidates += Join-Path (Split-Path -Parent (Split-Path -Parent $Root)) "tools\zig-x86_64-windows-0.16.0\zig.exe"

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    $fromPath = Get-Command zig -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    throw "Could not find zig. Install Zig, add it to PATH, set ZIG_EXE, or pass -ZigPath C:\path\to\zig.exe."
}

$Zig = Resolve-Zig $ZigPath

New-Item -ItemType Directory -Force -Path $Dist | Out-Null

& $Zig c++ `
    -target x86_64-windows-gnu `
    -shared `
    -O2 `
    -std=c++17 `
    -Wall `
    -Wextra `
    -Wno-nullability-completeness `
    -o $Dll `
    $Src `
    -lws2_32 `
    -ladvapi32

if ($LASTEXITCODE -ne 0) {
    throw "Plugin build failed with exit code $LASTEXITCODE"
}

Copy-Item -Force $Ini (Join-Path $Dist "kongoria_voice.ini")

Push-Location $InstallerDir
try {
    & $Zig rc $SetupRc $SetupRes
    if ($LASTEXITCODE -ne 0) {
        throw "Installer resource build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

& $Zig cc `
    -target x86_64-windows-gnu `
    -O2 `
    -std=c11 `
    -Wall `
    -Wextra `
    "-Wl,--subsystem,windows" `
    -o $SetupExe `
    $SetupSrc `
    $SetupRes `
    -luser32 `
    -lshell32

if ($LASTEXITCODE -ne 0) {
    throw "Installer build failed with exit code $LASTEXITCODE"
}

Write-Host "Using Zig: $Zig"
Write-Host "Built plugin: $Dll"
Write-Host "Copied config: $(Join-Path $Dist 'kongoria_voice.ini')"
Write-Host "Built installer: $SetupExe"
