# Windows Python 工具环境

从仓库根目录执行以下 PowerShell 命令。安装需要 Git for Windows 和可用的 `py -3.13`。首次安装会从上游下载 Python 工具。

```powershell
$toolchain = 'D:\Dev\sdvx-source-tools'
& .\tools\Install-SourceTools.ps1 -Toolchain $toolchain
```

安装脚本在工具链根目录下创建 `venv`，验证 Python 模块可导入和 Windows FFmpeg 可运行，然后显示两个可执行文件的完整路径。显式传入 `-Toolchain` 可让多个工作区使用不同的工具目录。省略该参数时，脚本使用 `SDVX_TOOLCHAIN` 的值，默认目录为 `D:\Dev\sdvx-source-tools`。

无需激活 venv。可直接使用其中的 Python，FFmpeg 的路径可由 `imageio-ffmpeg` 查询。

```powershell
$python = Join-Path $toolchain 'venv/Scripts/python.exe'
$ffmpeg = & $python -c 'from imageio_ffmpeg import get_ffmpeg_exe; print(get_ffmpeg_exe())'
& $ffmpeg -version
```

## 排查缺失文件

需要定位漏打的文件时，可以把本次构建的两个资源目录与 APK 直接按相对路径比较。缺失项会写入 `local-inputs/asset-paths-missing.txt`。

```powershell
& $python .\tools\check-asset-paths.py --reference .\development\android-native\assets-reference --override .\development\android-native\src\main\assets --apk 'D:\path\to\app.apk'
```
