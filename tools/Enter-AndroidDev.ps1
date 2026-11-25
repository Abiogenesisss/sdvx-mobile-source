param([string] $Toolchain = $env:SDVX_TOOLCHAIN)
if (!$Toolchain) { $Toolchain = 'D:/Dev/sdvx-source-tools' }

$SdvxToolchain = (Resolve-Path -LiteralPath $Toolchain -ErrorAction Stop).Path
$env:SDVX_TOOLCHAIN = $SdvxToolchain
$env:JAVA_HOME = Join-Path $SdvxToolchain 'jdk-17'
$env:ANDROID_HOME = Join-Path $SdvxToolchain 'android-sdk'
$env:ANDROID_AVD_HOME = Join-Path $SdvxToolchain 'android-avd'
$env:GRADLE_USER_HOME = Join-Path $SdvxToolchain 'gradle-cache'
$toolPaths = @(
    (Join-Path $env:JAVA_HOME 'bin')
    (Join-Path $env:ANDROID_HOME 'platform-tools')
    (Join-Path $env:ANDROID_HOME 'cmdline-tools/latest/bin')
    (Join-Path $SdvxToolchain 'gradle-8.13/bin')
)
$env:Path = ($toolPaths + ($env:Path -split ';')) -join ';'
