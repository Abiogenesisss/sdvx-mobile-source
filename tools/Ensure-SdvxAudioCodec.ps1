param(
    [Parameter(Mandatory)] [string] $Dependencies,
    [Parameter(Mandatory)] [string] $Ndk,
    [Parameter(Mandatory)] [ValidateSet('arm64-v8a', 'x86_64')] [string] $Abi
)

$ErrorActionPreference = 'Stop'
$source = Join-Path $Dependencies 'ffmpeg-9.0.2'
$build = Join-Path $Dependencies "ffmpeg-9.0.2-android/$Abi"
$libraries = 'avformat', 'avcodec', 'avutil', 'swresample'
if (!(($libraries | Where-Object { !(Test-Path -LiteralPath (Join-Path $build "install/lib/lib$_.so")) }))) { return }

if (!(Test-Path -LiteralPath (Join-Path $source 'configure'))) {
    $archive = "$source.tar.xz"
    curl.exe --fail --silent --show-error --location --retry 2 --output $archive https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
    if ($LASTEXITCODE) { throw 'Could not fetch FFmpeg source.' }
    tar.exe -xf $archive -C $Dependencies
    if ($LASTEXITCODE) { throw 'Could not extract FFmpeg source.' }
}

$hostTools = Join-Path $Dependencies 'ffmpeg-host'
$make = Join-Path $hostTools 'usr/bin/make.exe'
if (!(Test-Path -LiteralPath $make)) {
    New-Item -ItemType Directory -Force -Path $hostTools | Out-Null
    $archive = Join-Path $hostTools 'make-4.4.1-3-x86_64.pkg.tar.zst'
    curl.exe --fail --silent --show-error --location --retry 2 --output $archive https://mirror.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst
    if ($LASTEXITCODE) { throw 'Could not fetch GNU Make.' }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne 'AF0BDBA17F06FE037F0194069ADAA31A8FE45F1A11381501896AEA1FAE37BD5D') {
        throw 'GNU Make download digest mismatch.'
    }
    tar.exe -xf $archive -C $hostTools usr/bin/make.exe
    if ($LASTEXITCODE) { throw 'Could not extract GNU Make.' }
}

$bash = (Get-Command bash.exe -ErrorAction Stop).Source
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Building FFmpeg on Windows requires the Visual Studio C++ build tools.' }
$vc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$registeredSdk = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
$sdk = @($env:WindowsSdkDir, $registeredSdk, (Join-Path (Split-Path -Parent $vs) 'Windows Kits/10')) |
    Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'Include')) } | Select-Object -First 1
if (!$sdk) { throw 'Building FFmpeg on Windows requires the Windows SDK.' }
$sdkInclude = Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt/ctype.h') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$sdkInclude) { throw 'Could not locate the Windows SDK C runtime headers.' }
$sdkLib = Join-Path $sdk ('Lib/' + $sdkInclude.Name)
$oldInclude, $oldLib, $oldPath = $env:INCLUDE, $env:LIB, $env:Path
try {
    $env:INCLUDE = "$($vc.FullName)\include;$($sdkInclude.FullName)\ucrt;$($sdkInclude.FullName)\shared;$($sdkInclude.FullName)\um"
    $env:LIB = "$($vc.FullName)\lib\x64;$sdkLib\ucrt\x64;$sdkLib\um\x64"
    $env:Path = "$($vc.FullName)\bin\Hostx64\x64;$oldPath"
    $script = Join-Path $PSScriptRoot 'Build-SdvxAudioCodec.sh'
    $arguments = @($script, $source, $build, $Ndk, $Abi, $make) -replace '\\', '/'
    & $bash @arguments
    if ($LASTEXITCODE) { throw "FFmpeg build failed for $Abi." }
} finally {
    $env:INCLUDE, $env:LIB, $env:Path = $oldInclude, $oldLib, $oldPath
}
