# Build and package DeskFling-Windows-x64.zip on Windows (VS 2022 + CMake >= 3.25).
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/windows-msvc'
$pkgDir = Join-Path $root 'dist/DeskFling-Windows-x64'
$zipOut = Join-Path $root 'dist/DeskFling-Windows-x64.zip'

cmake --preset windows-msvc -S $root -B $build -DDPP_WARNINGS_AS_ERRORS=ON '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
cmake --build $build --config Release --target desktop_protected_mirror --parallel 3

if (Test-Path $pkgDir) { Remove-Item -Recurse -Force $pkgDir }
if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null

$exeSrc = Join-Path $build 'platforms/windows/Release/desktop_protected_mirror.exe'
Copy-Item -Force $exeSrc (Join-Path $pkgDir 'DeskFling.exe')

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$recoverCmd = "@echo off`r`nchcp 65001 >nul`r`ncd /d `"%~dp0`"`r`n`"DeskFling.exe`" --recover`r`n`"DeskFling.exe`" --status`r`npause`r`n"
[System.IO.File]::WriteAllText((Join-Path $pkgDir 'Recover-Desktop.cmd'), $recoverCmd, $utf8NoBom)

Compress-Archive -Path "$pkgDir\*" -DestinationPath $zipOut -Force
Get-FileHash -Algorithm SHA256 (Join-Path $pkgDir 'DeskFling.exe'), $zipOut | Format-List Path, Hash
