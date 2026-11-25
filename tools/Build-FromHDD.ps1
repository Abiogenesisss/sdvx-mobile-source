[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Hdd,
    [string] $Toolchain = $env:SDVX_TOOLCHAIN,
    [string] $ServiceData,
    [int[]] $SongIds,
    [switch] $PrepareOnly,
    [switch] $Offline
)
$ErrorActionPreference = 'Stop'
if (!$Toolchain) { $Toolchain = 'D:/Dev/sdvx-source-tools' }
$env:SDVX_TOOLCHAIN = [IO.Path]::GetFullPath($Toolchain)
$python = Join-Path $env:SDVX_TOOLCHAIN 'venv/Scripts/python.exe'
if (!(Test-Path -LiteralPath $python)) { & (Join-Path $PSScriptRoot 'Install-SourceTools.ps1') -Toolchain $env:SDVX_TOOLCHAIN }
$env:PYTHONUTF8 = '1'
$env:Path = "$(Split-Path -Parent $python);$env:Path"
$sdk = Join-Path $env:SDVX_TOOLCHAIN 'native-deps/CubismSdkForNative-5-r.5'
if (!(Test-Path -LiteralPath (Join-Path $sdk 'Framework/CMakeLists.txt'))) {
    throw "Install Cubism SDK for Native 5-r.5 under $sdk as described in docs/BUILD.md."
}
$ffmpeg = & $python -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())'
if ($LASTEXITCODE) { throw 'Install the export environment with tools/Install-SourceTools.ps1.' }
$extraArgs = @()
if ($SongIds) { $extraArgs += "--song-ids"; $extraArgs += $SongIds }
if ($ServiceData) { $extraArgs += '--service', (Resolve-Path -LiteralPath $ServiceData).ProviderPath }
& $python (Join-Path $PSScriptRoot 'prepare-hdd.py') (Resolve-Path -LiteralPath $Hdd).ProviderPath --sdk $sdk --ffmpeg $ffmpeg @extraArgs
if ($LASTEXITCODE) { throw "Local asset preparation failed. See the failing input and command above." }
if ($PrepareOnly) {
    & $python (Join-Path $PSScriptRoot 'check-build-inputs.py')
    if ($LASTEXITCODE) { throw 'Required output is missing.' }
} else {
    & (Join-Path $PSScriptRoot 'Build-SDVX.ps1') -Offline:$Offline
}
