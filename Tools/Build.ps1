[CmdletBinding()]
param(
    [string]$BuildDirectory = "cmake-build-debug-visual-studio",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Target = "PackageEditor",
    [ValidateRange(1, 128)]
    [int]$Parallel = 4,
    [switch]$VerboseBuild
)

$ErrorActionPreference = "Stop"
$buildPath = [System.IO.Path]::GetFullPath($BuildDirectory)
$logDirectory = Join-Path $buildPath "logs"
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $logDirectory "build-$timestamp.log"
$arguments = @(
    "--build", $buildPath,
    "--config", $Configuration,
    "--target", $Target,
    "--parallel", $Parallel
)
if ($VerboseBuild) { $arguments += "--verbose" }

$header = @(
    "GamEngine build log",
    "Started: $(Get-Date -Format o)",
    "Command: cmake $($arguments -join ' ')",
    ""
)
$header | Tee-Object -FilePath $logPath

& cmake @arguments 2>&1 | Tee-Object -FilePath $logPath -Append
$exitCode = $LASTEXITCODE

@(
    "",
    "Finished: $(Get-Date -Format o)",
    "Exit code: $exitCode"
) | Tee-Object -FilePath $logPath -Append

Write-Host "Build log: $logPath"
exit $exitCode
