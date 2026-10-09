param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$')]
    [string]$Version,
    [string]$CertificateThumbprint
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$package = Join-Path $repo 'out\build\release\GamEngineEditor'
$distribution = Join-Path $PSScriptRoot 'dist'
$setupSource = Join-Path $PSScriptRoot 'Bootstrapper'

Push-Location $repo
try {
    cmake --preset release -DGE_BUILD_EDITOR=ON -DGE_BUILD_PLAYER=ON -DGE_DISTRIBUTION_BUILD=ON
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    cmake --build --preset release --target PackageEditor --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'PackageEditor build failed.' }

    foreach ($required in @('Editor.exe', 'Engine.dll', 'DearImGui.dll', 'SDL3.dll',
        'Tools\Slang\slangc.exe', 'Runtime\Win64\Release\GamEnginePlayer.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $package $required) -PathType Leaf)) {
            throw "Required package file is missing: $required"
        }
    }
    foreach ($requiredDirectory in @('shaders', 'ShaderSources', 'SDK', 'Runtime\Win64\Release')) {
        $directory = Join-Path $package $requiredDirectory
        if (-not (Test-Path -LiteralPath $directory -PathType Container) -or
            -not (Get-ChildItem -LiteralPath $directory -Recurse -File | Select-Object -First 1)) {
            throw "Required package content is missing: $requiredDirectory"
        }
    }

    New-Item -ItemType Directory -Path $distribution -Force | Out-Null
    $files = @(Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($package.Length + 1).Replace('\', '/')
        [ordered]@{
            path = $relative
            size = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    if ($files.Count -eq 0) { throw 'The editor package is empty.' }
    @{ version = $Version; files = $files } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $distribution 'manifest.json') -Encoding utf8

    $archive = Join-Path $distribution 'GamEngineEditor.zip'
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
    Add-Type -AssemblyName System.IO.Compression.ZipFile
    [System.IO.Compression.ZipFile]::CreateFromDirectory($package, $archive,
        [System.IO.Compression.CompressionLevel]::Optimal, $true)

    $setupOutput = Join-Path $PSScriptRoot 'out\bootstrapper'
    cmake -S $setupSource -B $setupOutput -G 'Visual Studio 17 2022' -A x64 -DGE_SETUP_ENABLE_TEST_COMMANDS=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Setup configuration failed.' }
    cmake --build $setupOutput --config Release --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Native setup build failed.' }
    $setup = Join-Path $distribution 'GamEngineSetup.exe'
    Copy-Item -LiteralPath (Join-Path $setupOutput 'Release\GamEngineSetup.exe') -Destination $setup -Force

    if ($CertificateThumbprint) {
        $signTool = (Get-Command signtool.exe -ErrorAction Stop).Source
        & $signTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $setup
        if ($LASTEXITCODE -ne 0) { throw 'Setup signing failed.' }
    }
    $checksum = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
    "$checksum  GamEngineSetup.exe" | Set-Content -LiteralPath (Join-Path $distribution 'GamEngineSetup.exe.sha256') -Encoding ascii
    $archiveChecksum = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$archiveChecksum  GamEngineEditor.zip" | Set-Content -LiteralPath (Join-Path $distribution 'GamEngineEditor.zip.sha256') -Encoding ascii
    Write-Host "Ready: $setup"
    if (-not $CertificateThumbprint) { Write-Warning 'Setup is unsigned. Do not publish it as a trusted update.' }
}
finally { Pop-Location }
