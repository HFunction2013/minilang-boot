# install.ps1 - Build and install the minilang boot compiler + VM onto a
# per-user prefix (default %LOCALAPPDATA%\Programs\minilang).
#
# Steps: fetch (or reuse a local checkout) -> compile on site -> lay down
# bin\ and lib\minilang -> add the prefix bin to the user PATH -> smoke-test
# the installed command.
#
# Layout under $Prefix:
#   bin\minilang.cmd              wrapper (sets MINILANG_DIR, launches the boot exe)
#   lib\minilang\bin\minilang.exe real boot binary
#   lib\minilang\runtime.c        C runtime for the native backend
#   lib\minilang\minilang.h       shared C headers
#   lib\minilang\minilang_runtime.h
#   lib\minilang\syslib\*.mil     bundled standard library
#
# Parameters:
#   -Prefix    install prefix (default $env:LOCALAPPDATA\Programs\minilang)
#   -Source    reuse an existing boot source tree instead of fetching
#   -Repo      boot git URL to fetch
#   -SysRepo   syslib git URL (fetched separately; archives omit submodules)
#   -Branch    branch/tag to fetch (default main; empty = remote HEAD via git)
#   MINILANG_NO_GIT=1  skip git and download the source archive (zip) instead
#   -KeepWork  keep the temporary build tree for inspection

[CmdletBinding()]
param(
    [string]$Prefix = $(if ($env:MINILANG_PREFIX) { $env:MINILANG_PREFIX } else { Join-Path $env:LOCALAPPDATA 'Programs\minilang' }),
    [string]$Source = $(if ($env:BOOT_SOURCE) { $env:BOOT_SOURCE } else { '' }),
    [string]$Repo = $(if ($env:MINILANG_BOOT_REPO) { $env:MINILANG_BOOT_REPO } else { 'https://github.com/HFunction2013/minilang-boot.git' }),
    [string]$SysRepo = $(if ($env:MINILANG_SYS_REPO) { $env:MINILANG_SYS_REPO } else { 'https://github.com/HFunction2013/minilang-syslib.git' }),
    [string]$SysSource = $(if ($env:SYS_SOURCE) { $env:SYS_SOURCE } else { '' }),
    [string]$Branch = $(if ($env:MINILANG_BRANCH) { $env:MINILANG_BRANCH } else { 'main' }),
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir '..')).Path

function Log($m) { Write-Host "[install] $m" }
function Die($m) { Write-Host "[install] $m" -ForegroundColor Red; exit 1 }

function Get-Slug($Url) {
    $u = $Url -replace '^https?://', ''
    $u = $u -replace '^git@', ''
    $u = $u -replace '^github.com[:/]', ''
    $u = $u -replace '\.git$', ''
    return $u.TrimEnd('/')
}

function Fetch-Repo($RepoUrl, $Name) {
    if (-not $Work) { $script:Work = Join-Path $env:TEMP ("minilang-boot-{0}" -f ([guid]::NewGuid().ToString('N').Substring(0,8))) }
    $dir = Join-Path $Work $Name
    if (Test-Path $dir) { return $dir }
    $useGit = $true
    if ($env:MINILANG_NO_GIT -eq '1') { $useGit = $false }
    if ($useGit -and (Get-Command git -ErrorAction SilentlyContinue)) {
        $cloneArgs = @('clone', '--depth', '1')
        if ($Branch) { $cloneArgs += @('--branch', $Branch); Log "Cloning $RepoUrl ($Branch)" }
        else { Log "Cloning $RepoUrl (remote HEAD)" }
        $cloneArgs += @($RepoUrl, $dir)
        git @cloneArgs | Out-Host
    } else {
        if (-not $Branch) { Die 'git unavailable: set -Branch / MINILANG_BRANCH to download an archive' }
        $slug = Get-Slug $RepoUrl
        $zip = Join-Path $Work "$Name.zip"
        Log "Downloading archive $slug @ $Branch (no git)"
        Invoke-WebRequest -Uri "https://codeload.github.com/$slug/zip/refs/heads/$Branch" -OutFile $zip
        $extract = Join-Path $Work "$Name-extract"
        Expand-Archive -Path $zip -DestinationPath $extract -Force
        $nested = Get-ChildItem $extract -Directory | Select-Object -First 1
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Move-Item (Join-Path $nested.FullName '*') $dir -Force
        Remove-Item $zip -Force -ErrorAction SilentlyContinue
    }
    return $dir
}

# ---- 1. Locate source: explicit override, local checkout, else fetch ----
$Work = $null
if (-not $Source -and (Test-Path (Join-Path $RepoRoot 'main.c'))) {
    $Source = $RepoRoot
    Log "Using local checkout: $Source"
}
if (-not $Source) { $Source = Fetch-Repo $Repo 'boot' }
if (-not $SysSource -and (Test-Path (Join-Path $Source 'syslib\builtin.mil'))) {
    $SysSource = Join-Path $Source 'syslib'
}
if (-not $SysSource) { $SysSource = Fetch-Repo $SysRepo 'syslib' }
if (-not (Test-Path (Join-Path $Source 'main.c'))) { Die "boot source not found at $Source" }

# ---- 2. Compile on site (Windows: CMake, VS generator Release) ----
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Die 'cmake not found on PATH' }

Log "Building boot compiler in $Source"
Push-Location $Source
try {
    cmake -B build | Out-Host
    if ($LASTEXITCODE -ne 0) { Die 'cmake configure failed' }
    cmake --build build --config Release | Out-Host
    if ($LASTEXITCODE -ne 0) { Die 'cmake build failed' }
} finally { Pop-Location }

$BootExe = Join-Path $Source 'build\Release\minilang.exe'
if (-not (Test-Path $BootExe)) {
    # Single-config generators (e.g. MinGW/Ninja) drop the exe in build\.
    $BootExe = Join-Path $Source 'build\minilang.exe'
}
if (-not (Test-Path $BootExe)) { Die "build failed: minilang.exe not produced" }

# ---- 3. Lay down the install tree ----
$Lib    = Join-Path $Prefix 'lib\minilang'
$BinDir = Join-Path $Lib 'bin'
Log "Installing to $Prefix"
New-Item -ItemType Directory -Force -Path $BinDir, (Join-Path $Lib 'syslib'), (Join-Path $Prefix 'bin') | Out-Null

Copy-Item $BootExe (Join-Path $BinDir 'minilang.exe') -Force

# Import library produced by the build (MSVC minilang.lib, MinGW
# libminilang.dll.a) lives next to the exe so prebuilt extension DLLs link it.
foreach ($imp in 'minilang.lib','libminilang.dll.a') {
    $candidates = @(
        (Join-Path $Source $imp),
        (Join-Path $Source "build\Release\$imp"),
        (Join-Path $Source "build\$imp")
    )
    foreach ($c in $candidates) { if (Test-Path $c) { Copy-Item $c $BinDir -Force } }
}

foreach ($f in 'runtime.c','minilang.h','minilang_runtime.h') {
    $src = Join-Path $Source $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $Lib $f) -Force }
}
if ($SysSource -and (Test-Path $SysSource)) {
    Get-ChildItem -Path $SysSource -Filter '*.mil' -File | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $Lib 'syslib') -Force
    }
}

# ---- 4. Wrapper on PATH: anchor MINILANG_DIR to the installed lib dir ----
$wrapper = Join-Path $Prefix 'bin\minilang.cmd'
$cmd = @"
@echo off
rem Generated by minilang-boot/scripts/install.ps1.
if not defined MINILANG_DIR set "MINILANG_DIR=%~dp0..\lib\minilang"
"%~dp0..\lib\minilang\bin\minilang.exe" %*
"@
Set-Content -Path $wrapper -Value $cmd -Encoding ASCII

# ---- 5. Add $Prefix\bin to the user PATH (idempotent) ----
$userPath = [Environment]::GetEnvironmentVariable('Path','User')
$binPath  = Join-Path $Prefix 'bin'
if (-not ($userPath -split ';') -contains $binPath) {
    Log "Adding $binPath to user PATH"
    $newPath = (($userPath.TrimEnd(';')) + ';' + $binPath)
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    $env:Path = "$env:Path;$binPath"
}

# ---- 6. Smoke test the installed command ----
$tmp = Join-Path $env:TEMP ("minilang-smoke-{0}" -f ([guid]::NewGuid().ToString('N').Substring(0,8)))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
Set-Content -Path (Join-Path $tmp 'hello.mil') -Value @(
    'func main() {',
    '    println "minilang install OK";',
    '    return 0;',
    '}'
) -Encoding ASCII
Push-Location $tmp
try { & $wrapper run hello.mil } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Die 'smoke test failed' }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($Work -and -not $KeepWork) { Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue }
Log "Done. Run 'minilang' from a fresh terminal (or '$wrapper')."
