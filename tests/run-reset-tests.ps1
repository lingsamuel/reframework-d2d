param(
    [string]$BuildDirectory = "$PSScriptRoot/../build/reset-tests",
    [string]$Configuration = "Release",
    [string]$Filter = "",
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"
$sourceDirectory = (Resolve-Path "$PSScriptRoot/..").Path
# Reuse an existing configured build, or let CMake select the installed Windows toolchain.
if (-not (Test-Path "$BuildDirectory/CMakeCache.txt")) {
    & cmake -S $sourceDirectory -B $BuildDirectory -DREFD2D_BUILD_TESTS=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
}
& cmake --build $BuildDirectory --config $Configuration --target hdr-render-tests
if ($LASTEXITCODE -ne 0) { throw "Test build failed" }
$executable = Join-Path $BuildDirectory "$Configuration/hdr-render-tests.exe"
if (-not (Test-Path $executable)) { $executable = Join-Path $BuildDirectory "hdr-render-tests.exe" }
$process = New-Object System.Diagnostics.Process
$process.StartInfo.FileName = $executable
$process.StartInfo.UseShellExecute = $false
if ($Filter) { $process.StartInfo.Arguments = '"' + $Filter + '"' }
if (-not $process.Start()) { throw "Could not start regression tests" }
if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    $process.Kill()
    $process.WaitForExit()
    throw "Test process exceeded $TimeoutSeconds seconds (possible deadlock)"
}
$exitCode = $process.ExitCode
$process.Dispose()
if ($exitCode -ne 0) { throw "Regression tests failed: exit code $exitCode" }
