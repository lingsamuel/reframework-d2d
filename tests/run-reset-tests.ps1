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
& cmake --build $BuildDirectory --config $Configuration --target hdr-render-tests frame-mailbox-tests
if ($LASTEXITCODE -ne 0) { throw "Test build failed" }

function Invoke-RegressionTest([string]$Name, [string]$TestFilter = "") {
    $executable = Join-Path $BuildDirectory "$Configuration/$Name.exe"
    if (-not (Test-Path $executable)) { $executable = Join-Path $BuildDirectory "$Name.exe" }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $executable
    $process.StartInfo.UseShellExecute = $false
    if ($TestFilter) { $process.StartInfo.Arguments = '"' + $TestFilter + '"' }
    if (-not $process.Start()) { throw "Could not start $Name" }
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "$Name exceeded $TimeoutSeconds seconds (possible deadlock)"
    }
    $exitCode = $process.ExitCode
    $process.Dispose()
    if ($exitCode -ne 0) { throw "$Name failed: exit code $exitCode" }
}

Invoke-RegressionTest "hdr-render-tests" $Filter
Invoke-RegressionTest "frame-mailbox-tests"
