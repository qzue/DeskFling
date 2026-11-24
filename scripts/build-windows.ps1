# Windows-only, clean x64 build. Does not launch either desktop executable.
# Requires PowerShell 7, VS 2022 C++ workload + Windows SDK, and CMake >= 3.25.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'Requires a native Windows x64 execution environment.'
}
$root = Split-Path $PSScriptRoot -Parent
$run = Join-Path $root ('out/windows-' + [guid]::NewGuid().ToString('N'))
$source = Join-Path $run 'source'
$build = Join-Path $run 'build'
$stage = Join-Path $run 'stage'
$artifacts = Join-Path $run 'artifacts'
New-Item -ItemType Directory -Path $source, $artifacts | Out-Null
function Write-Json($Value, $Path) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}
function Invoke-Checked([string]$Name, [string]$Program, [string[]]$Arguments) {
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $artifacts "$Name.log") | Out-Host
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "$Name failed: exit $code" }
}
function Protect-ArtifactPaths {
    # Keep build-tree diagnostics intact; exported logs must not expose user directories.
    foreach ($file in Get-ChildItem $artifacts -File) {
        if ($file.Extension -notin @('.log', '.txt', '.xml', '.json')) { continue }
        $text = [IO.File]::ReadAllText($file.FullName)
        foreach ($path in @($run, $env:USERPROFILE)) {
            $replacement = if ($path -eq $run) { '[RUN]' } else { '%USERPROFILE%' }
            # Native paths, CMake's forward slashes, and JSON-escaped backslashes.
            $text = $text.Replace($path.Replace('\', '\\'), $replacement)
            $text = $text.Replace($path, $replacement).Replace($path.Replace('\', '/'), $replacement)
        }
        [IO.File]::WriteAllText($file.FullName, $text, [Text.UTF8Encoding]::new($false))
    }
}
try {
    # Explicit input allowlist: includes uncommitted files, never Git metadata or desktop evidence.
    $inputs = @('CMakeLists.txt', 'CMakePresets.json', 'LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md',
        'core', 'apps', 'platforms/windows', 'platforms/mirror_hit.h', 'platforms/display_migration.h', 'platforms/filename_geometry.h', 'tests', 'benchmarks', 'third_party/box2d',
        'third_party/notices', 'scripts/build-windows.ps1', 'docs/windows-build.md')
    if (Test-Path (Join-Path $root 'third_party/archives/googletest-1.15.2.tar.gz')) {
        $inputs += 'third_party/archives/googletest-1.15.2.tar.gz'
    }
    foreach ($inputPath in $inputs) {
        $from = Join-Path $root $inputPath
        $to = Join-Path $source $inputPath
        New-Item -ItemType Directory -Force -Path (Split-Path $to -Parent) | Out-Null
        # Reject links rather than following unexpected files outside the project.
        $entries = @(Get-Item -LiteralPath $from) + @(Get-ChildItem -LiteralPath $from -Recurse -Force)
        if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
            throw "Linked input is not allowed: $inputPath"
        }
        Copy-Item -LiteralPath $from -Destination $to -Recurse
    }
    $files = @(Get-ChildItem $source -Recurse -File | Sort-Object FullName | ForEach-Object {
        @{ path = [IO.Path]::GetRelativePath($source, $_.FullName).Replace('\', '/');
           sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $inputManifest = Join-Path $artifacts 'source-inputs.json'
    Write-Json $files $inputManifest
    $inputHash = (Get-FileHash $inputManifest -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = [ordered]@{
        status = 'building'; source_input_id = $inputHash; source_identity = 'SHA256 of source-inputs.json; not Git HEAD'
        os = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
        architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, AddressWidth)
        powershell = $PSVersionTable.PSVersion.ToString(); image_os = $env:ImageOS; image_version = $env:ImageVersion
        github_run_id = $env:GITHUB_RUN_ID; github_run_attempt = $env:GITHUB_RUN_ATTEMPT
        compiler_sdk = 'See configure.log and toolchain.txt'; runtime = 'static MSVC CRT'
        w2 = 'not_run'; shell_probe = 'not_run'; signing = 'unsigned development build'
    }
    Write-Json $manifest (Join-Path $artifacts 'build-manifest.json')
    Invoke-Checked 'cmake-version' 'cmake' @('--version')
    # Visual Studio generator locates MSVC itself; no assumption of an initialized Ninja shell.
    Invoke-Checked 'configure' 'cmake' @('--preset', 'windows-msvc', '-S', $source, '-B', $build,
        '-DDPP_WARNINGS_AS_ERRORS=ON', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>')
    Get-ChildItem (Join-Path $build 'CMakeFiles') -Recurse -File |
        Where-Object { $_.Name -in @('CMakeCCompiler.cmake', 'CMakeCXXCompiler.cmake', 'CMakeSystem.cmake') } |
        ForEach-Object { Get-Content $_.FullName } | Set-Content (Join-Path $artifacts 'toolchain.txt')
    Select-String -Path (Join-Path $build 'DesktopPhysicsPlayground.sln'), (Join-Path $build 'CMakeCache.txt') -Pattern 'VisualStudioVersion|CMAKE_GENERATOR|CMAKE_VS_' |
        Out-File (Join-Path $artifacts 'toolchain.txt') -Append
    Get-ChildItem $build -Recurse -Filter '*.vcxproj' | Select-String -Pattern 'WindowsTargetPlatformVersion|PlatformToolset' |
        Out-File (Join-Path $artifacts 'toolchain.txt') -Append
    foreach ($config in @('Debug', 'Release')) {
        Invoke-Checked "build-$config" 'cmake' @('--build', $build, '--config', $config, '--parallel', '3')
        Invoke-Checked "tests-$config" 'ctest' @('--test-dir', $build, '-C', $config, '--output-on-failure',
            '--no-tests=error', '--output-junit', (Join-Path $artifacts "tests-$config.xml"))
        $directExe = @(
            (Join-Path $build "platforms/windows/$config/windows_direct_native_tests.exe"),
            (Join-Path $build "tests/$config/windows_direct_native_tests.exe")
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $directExe) { throw "Missing windows_direct_native_tests.exe ($config)" }
        Invoke-Checked "direct-native-tests-$config" $directExe @()
    }
    Invoke-Checked 'install' 'cmake' @('--install', $build, '--config', 'Release', '--prefix', $stage)
    foreach ($exe in @('physics_sandbox.exe', 'desktop_probe.exe', 'desktop_mirror_preview.exe', 'desktop_protected_mirror.exe')) {
        if (-not (Test-Path (Join-Path $stage "bin/$exe"))) { throw "Missing installed target: $exe" }
    }
    $compilerFile = Get-ChildItem (Join-Path $build 'CMakeFiles') -Recurse -Filter CMakeCXXCompiler.cmake | Select-Object -First 1
    $compilerText = Get-Content $compilerFile.FullName -Raw
    if ($compilerText -notmatch 'set\(CMAKE_CXX_COMPILER "([^"]+)"\)') { throw 'Cannot locate compiler for dependency inspection' }
    $dumpbin = Join-Path (Split-Path $Matches[1] -Parent) 'dumpbin.exe'
    foreach ($exe in @('physics_sandbox.exe', 'desktop_probe.exe', 'desktop_mirror_preview.exe', 'desktop_protected_mirror.exe')) {
        Invoke-Checked "dependencies-$exe" $dumpbin @('/DEPENDENTS', (Join-Path $stage "bin/$exe"))
        Invoke-Checked "headers-$exe" $dumpbin @('/HEADERS', (Join-Path $stage "bin/$exe"))
    }
    Protect-ArtifactPaths
    # Only the dedicated Windows instructions enter the portable package.
    # The source snapshot contains no docs/evidence, preferences, screenshots or credentials.
    $manifest.status = 'build_and_tests_passed_gui_unverified'
    Write-Json $manifest (Join-Path $artifacts 'build-manifest.json')
    Copy-Item (Join-Path $artifacts 'build-manifest.json'), $inputManifest -Destination $stage
    $zip = Join-Path $artifacts "DesktopPhysicsSandbox-Windows-x64-$($inputHash.Substring(0,12)).zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), [IO.Path]::GetRelativePath($stage, $_.FullName)
    } | Set-Content (Join-Path $artifacts 'installed-sha256.txt')
    '{0}  {1}' -f (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path $zip -Leaf) |
        Set-Content (Join-Path $artifacts 'package-sha256.txt')
    Write-Host "Artifacts: $artifacts"
} catch {
    # These are only this invocation's own incomplete outputs, never prior packages.
    Get-ChildItem $artifacts -Filter '*.zip' -File | Remove-Item
    Write-Json @{ status = 'failed'; error = $_.Exception.Message } (Join-Path $artifacts 'failure.json')
    if (Get-Variable manifest -ErrorAction SilentlyContinue) {
        $manifest.status = 'failed'
        Write-Json $manifest (Join-Path $artifacts 'build-manifest.json')
    }
    Protect-ArtifactPaths
    Write-Error $_ -ErrorAction Continue
    exit 1
}
