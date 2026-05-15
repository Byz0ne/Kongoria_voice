param(
    [string] $ZigPath = "",
    [string] $Target = "x86_64-linux-gnu"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $Root "src\kongoria_voice_plugin.cpp"
$Dist = Join-Path $Root "dist\linux"
$So = Join-Path $Dist "kongoria_voice.so"
$Ini = Join-Path $Root "config\kongoria_voice.example.ini"

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
    -target $Target `
    -shared `
    -fPIC `
    -O2 `
    -std=c++17 `
    -Wall `
    -Wextra `
    -Wno-nullability-completeness `
    -o $So `
    $Src `
    -pthread `
    -ldl `
    -lm

if ($LASTEXITCODE -ne 0) {
    throw "Linux plugin build failed with exit code $LASTEXITCODE"
}

Copy-Item -Force $Ini (Join-Path $Dist "kongoria_voice.ini")

Write-Host "Using Zig: $Zig"
Write-Host "Built Linux plugin: $So"
Write-Host "Copied config: $(Join-Path $Dist 'kongoria_voice.ini')"
