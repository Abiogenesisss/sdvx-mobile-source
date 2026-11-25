# Requires Git and the Windows Python launcher with CPython 3.13 installed.
param(
    [string] $Toolchain = $env:SDVX_TOOLCHAIN
)

$ErrorActionPreference = 'Stop'
if (!$Toolchain) { $Toolchain = 'D:/Dev/sdvx-source-tools' }

$venv = Join-Path $Toolchain 'venv'
$python = Join-Path $venv 'Scripts/python.exe'
if (!(Test-Path -LiteralPath $python -PathType Leaf)) {
    py -3.13 -m venv $venv
    if ($LASTEXITCODE) { throw 'Unable to create the Python virtual environment.' }
}

& $python -m pip install --only-binary ':all:' @(
    'numpy==2.5.5'
    'Pillow==13.0.0'
    'capstone==5.0.9'
    'pefile==2024.8.26'
    'ifstools==3.2'
    'imageio-ffmpeg==0.8.1'
    'Requests==2.34.2'
    'Flask-Caching==2.5.1'
    'pycryptodome==4.6.0'
)
if ($LASTEXITCODE) { throw 'Unable to install the Python tool dependencies.' }

# Use the portable parser build and install its runtime imports above.
$previousPurePython = $env:PURE_PYTHON
try {
    $env:PURE_PYTHON = '1'
    & $python -m pip install --no-deps 'bemani @ git+https://github.com/DragonMinded/bemaniutils.git@1e66fcd780afb3088ce8e78c88615a143db492ee'
    if ($LASTEXITCODE) { throw 'Unable to install the upstream bemani parsers.' }
} finally {
    $env:PURE_PYTHON = $previousPurePython
}

$ffmpeg = & $python -c 'import bemani.format, bemani.format.afp, numpy, PIL, capstone, pefile, ifstools; from imageio_ffmpeg import get_ffmpeg_exe; print(get_ffmpeg_exe())'
if ($LASTEXITCODE) { throw 'Unable to import the installed tools or locate FFmpeg.' }
& $ffmpeg -version | Select-Object -First 1
if ($LASTEXITCODE) { throw 'Unable to run the FFmpeg executable.' }

[pscustomobject] @{
    Python = (Resolve-Path -LiteralPath $python).Path
    Ffmpeg = $ffmpeg
}
