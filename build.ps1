# Builds zygisk/arm64-v8a.so and packages a flashable Magisk zip.
# Requires the Android NDK; set ANDROID_NDK_HOME to its root folder.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not $env:ANDROID_NDK_HOME) { throw 'Set ANDROID_NDK_HOME to your Android NDK folder.' }
$hostTag = if ($IsLinux) { 'linux-x86_64' } elseif ($IsMacOS) { 'darwin-x86_64' } else { 'windows-x86_64' }
$ext = if ($hostTag -eq 'windows-x86_64') { '.cmd' } else { '' }
$cxx = Join-Path $env:ANDROID_NDK_HOME "toolchains/llvm/prebuilt/$hostTag/bin/aarch64-linux-android26-clang++$ext"
if (-not (Test-Path $cxx)) { throw "Compiler not found: $cxx" }

New-Item -ItemType Directory -Force zygisk | Out-Null
& $cxx -shared -fPIC -O2 -std=c++17 -fno-exceptions -fno-rtti -fvisibility=hidden `
    -Wall -Wno-misleading-indentation -static-libstdc++ '-Wl,--gc-sections' -s `
    jni/module.cpp -llog -o zygisk/arm64-v8a.so
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }

$version = ((Get-Content module.prop) -match '^version=') -replace '^version=', ''
$zip = "pico4-steamlink-velfix-$version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path module.prop, zygisk -DestinationPath $zip
Write-Host "Built $zip"
