[CmdletBinding()]
param([switch] $EmulatorOnly, [switch] $EmulatorGles31, [switch] $Offline)

$ErrorActionPreference = 'Stop'
if ($EmulatorGles31 -and !$EmulatorOnly) { throw 'The GLES 3.1 test target is emulator-only.' }
. (Join-Path $PSScriptRoot 'Enter-AndroidDev.ps1')
$project = Join-Path (Split-Path -Parent $PSScriptRoot) 'development/android-native'
$deps = Join-Path $env:SDVX_TOOLCHAIN 'native-deps'
& python (Join-Path $PSScriptRoot 'check-build-inputs.py')
if ($LASTEXITCODE) { throw 'Prepare the missing local inputs before building. See docs/BUILD.md.' }
foreach ($required in @('CubismSdkForNative-5-r.5/Framework/CMakeLists.txt', 'bass/c/bass.h', 'bass-fx/C/bass_fx.h')) {
    if (!(Test-Path -LiteralPath (Join-Path $deps $required))) { throw "Install the vendor SDK in $deps first: $required" }
}
New-Item -ItemType Directory -Force -Path $deps | Out-Null
$packages = @(
    @('SDL-release-3.4.18', 'https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-3.4.18', 'CMakeLists.txt'),
    @('RmlUi-bc6ea0ec9968600d0b6e9dc379c5b9ba20f2806d', 'https://codeload.github.com/mikke89/RmlUi/tar.gz/bc6ea0ec9968600d0b6e9dc379c5b9ba20f2806d', 'CMakeLists.txt'),
    @('freetype-VER-2-14-3', 'https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-14-3', 'CMakeLists.txt'),
    @('stb-4e9b368bfc071e97206ea5c6adba68dd0cbf8897', 'https://codeload.github.com/nothings/stb/tar.gz/4e9b368bfc071e97206ea5c6adba68dd0cbf8897', 'stb_image.h'),
    @('libwebp-1.6.0', 'https://codeload.github.com/webmproject/libwebp/tar.gz/refs/tags/v1.6.0', 'CMakeLists.txt'),
    @('json-3.12.0', 'https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0', 'single_include/nlohmann/json.hpp')
)
foreach ($package in $packages) {
    $name, $url, $entry = $package
    $directory = Join-Path $deps $name
    if (Test-Path -LiteralPath (Join-Path $directory $entry)) { continue }
    $archive = Join-Path $deps "$name.tar.gz"
    curl.exe --fail --silent --show-error --location --retry 2 --output $archive $url
    if ($LASTEXITCODE) { throw "Could not fetch $name." }
    tar.exe -xzf $archive --exclude='*/android-project-ant' -C $deps
    if ($LASTEXITCODE) { throw "Could not unpack $name." }
    Get-Item -LiteralPath (Join-Path $directory $entry) | Out-Null
}

# Keep each ABI's generated build files in its own directory.
$stagingName = if ($EmulatorOnly) { 'source-emulator-build' } else { 'source-device-build' }
$staging = Join-Path $env:SDVX_TOOLCHAIN $stagingName
New-Item -ItemType Directory -Force -Path $staging | Out-Null
$staging = (Resolve-Path -LiteralPath $staging).ProviderPath
if ((Get-Item -LiteralPath $staging -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw "The native build staging directory is linked: $staging"
}
# Both ABIs read the same project sources and assets.
$inputs = @{
    'src' = 'src'
    'assets-reference' = 'assets-reference'
    'assets-native' = 'src/main/assets'
}
foreach ($name in $inputs.Keys) {
    $target = Join-Path $staging $name
    $source = (Resolve-Path -LiteralPath (Join-Path $project $inputs[$name])).ProviderPath
    if (!(Test-Path -LiteralPath $target)) {
        New-Item -ItemType Junction -Path $target -Target $source | Out-Null
    }
    $link = Get-Item -LiteralPath $target -Force
    if ($link.LinkType -ne 'Junction' -or $link.Target -ne $source) {
        throw "Build input $target must point to $source."
    }
}
foreach ($file in @('settings.gradle', 'build.gradle', 'gradle.properties')) {
    Copy-Item -LiteralPath (Join-Path $project $file) -Destination $staging
}
$abis = if ($EmulatorOnly) { 'x86_64' } else { 'arm64-v8a' }
$compat = if ($EmulatorGles31) { 'ON' } else { 'OFF' }
& (Join-Path $PSScriptRoot 'Ensure-SdvxAudioCodec.ps1') $deps (Join-Path $env:ANDROID_HOME 'ndk/28.2.13676358') $abis
Push-Location -LiteralPath $staging
try {
    # Avoid AGP's per-asset JAR pipeline: hashing its thousands of Windows files
    # stalls this workspace. Keep Gradle's compiler/linker/merger outputs fresh,
    # then assemble those outputs directly into the same debug APK format.
    # The inputs below junctions are shared by both ABI workspaces. A daemon's
    # retained filesystem snapshot can miss writes through the original path.
    $cacheOptions = @()
    if ($Offline) { $cacheOptions += '--offline' }
    gradle.bat @cacheOptions --no-watch-fs --console=plain processDebugResources mergeProjectDexDebug mergeLibDexDebug mergeExtDexDebug mergeDebugJavaResource stripDebugDebugSymbols stageNablaAssets validateSigningDebug "-PsdvxDeps=$deps" "-PsdvxAbis=$abis" "-PsdvxEmulatorGles31=$compat"
    if ($LASTEXITCODE) { throw 'Native Android build failed.' }
    $packaging = Join-Path $staging 'build/native-apk'
    New-Item -ItemType Directory -Force -Path $packaging | Out-Null
    $unsigned = Join-Path $packaging 'unsigned.apk'
    $aligned = Join-Path $packaging 'aligned.apk'
    $signed = Join-Path $packaging 'sdvx-native-debug.apk'
    python (Join-Path $project 'tools/package-native-apk.py') $staging $unsigned $abis
    if ($LASTEXITCODE) { throw 'Assembling the native APK failed.' }
    $buildTools = Join-Path $env:ANDROID_HOME 'build-tools/36.0.0'
    & (Join-Path $buildTools 'zipalign.exe') -P 16 -f 4 $unsigned $aligned
    if ($LASTEXITCODE) { throw 'Aligning the native APK failed.' }
    $debugKey = Join-Path $env:USERPROFILE '.android/debug.keystore'
    & (Join-Path $buildTools 'apksigner.bat') sign --ks $debugKey --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android --out $signed $aligned
    if ($LASTEXITCODE) { throw 'Signing the native APK failed.' }
    $output = Join-Path $project 'build/outputs/apk/debug'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $filename = if ($EmulatorGles31) { 'sdvx-native-emulator-debug.apk' } else { 'sdvx-android-native-debug.apk' }
    Copy-Item -LiteralPath $signed -Destination (Join-Path $output $filename) -PassThru
} finally {
    Pop-Location
}
