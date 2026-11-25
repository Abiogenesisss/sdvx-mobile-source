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

## 资源路径验收

使用同一个 Python，根据待检查的产物选择一个入口。

```powershell
& $python .\tools\check-asset-paths.py --merged 'D:\path\to\nabla-assets'
& $python .\tools\check-asset-paths.py --apk 'D:\path\to\app.apk'
```

[仓库基准](asset-path-baseline.md)只记录路径按组计数与摘要。曲目差异仅供参考。如果有对应的参照资源根和覆盖资源根，可按基准页的说明执行允许额外路径的子集检查。路径验收无需安装 APK。
