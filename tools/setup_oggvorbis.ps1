# thmod: builds the static ogg/vorbis libraries the project links against
# (lib\oggvorbis\*.lib) using a standalone vcpkg. Run once from PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\setup_oggvorbis.ps1
# Requires: git, Visual Studio with MSVC x86 tools, internet access.
param([string]$VcpkgRoot = "$env:LOCALAPPDATA\thmod-vcpkg")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
    if (-not (Test-Path "$VcpkgRoot\bootstrap-vcpkg.bat")) {
        git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VcpkgRoot"
        if ($LASTEXITCODE) { throw "git clone of vcpkg failed" }
    }
    & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
    if ($LASTEXITCODE) { throw "vcpkg bootstrap failed" }
}

# x86-windows-static = 32-bit, static CRT (/MT, /MTd) - same as thehell3.vcxproj
& "$VcpkgRoot\vcpkg.exe" install libogg:x86-windows-static libvorbis:x86-windows-static
if ($LASTEXITCODE) { throw "vcpkg install failed" }

$inst = "$VcpkgRoot\installed\x86-windows-static"
$dst  = Join-Path $repo 'lib\oggvorbis'
New-Item -ItemType Directory -Force $dst | Out-Null

function Find-Lib([string]$dir, [string]$name) {
    foreach ($n in @("$name.lib", "${name}d.lib", "lib$name.lib")) {
        $p = Join-Path $dir $n
        if (Test-Path $p) { return $p }
    }
    throw "Not found: $name in $dir"
}

$map = [ordered]@{ ogg = 'libogg_static'; vorbis = 'libvorbis_static'; vorbisfile = 'libvorbisfile_static' }
foreach ($k in $map.Keys) {
    Copy-Item (Find-Lib "$inst\lib" $k)       (Join-Path $dst "$($map[$k]).lib") -Force
    Copy-Item (Find-Lib "$inst\debug\lib" $k) (Join-Path $dst "$($map[$k])_debug.lib") -Force
}
Get-ChildItem $dst | Format-Table Name, Length
Write-Host "OK: ogg/vorbis libraries are in $dst"
