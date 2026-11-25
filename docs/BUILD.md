# 在 Windows 上用本地数据构建

目标为 Windows x64 上生成 arm64-v8a、OpenGL ES 3.2 的 Android 调试包，最低 Android API 26。普通手机构建无需安装模拟器或系统镜像。文中 PowerShell 命令从本仓库根目录执行。

## 1. 配置工具链

使用 PowerShell、CPython 3.13 和 Windows Python 启动器、Git for Windows、JDK 17 和 Gradle 8.13。用一个独立的 ASCII 目录作工具链根目录。如果电脑上还有旧开发工程，请给这个克隆使用不同的根目录，因为构建脚本会在里面建立指向当前源码的目录联接。

```powershell
$env:SDVX_TOOLCHAIN = 'D:\Dev\sdvx-source-tools'
New-Item -ItemType Directory -Force -Path $env:SDVX_TOOLCHAIN | Out-Null
```

将 JDK 解压至其下的 `jdk-17`（包含 `bin/java.exe`），Gradle 解压至 `gradle-8.13`（包含 `bin/gradle.bat`），Android 命令行工具放到 `android-sdk/cmdline-tools/latest`。下载入口：[JDK 17](https://adoptium.net/temurin/releases/?version=17)、[Gradle 发行版](https://gradle.org/releases/)、[Android 命令行工具](https://developer.android.com/studio#command-tools)。

```powershell
. .\tools\Enter-AndroidDev.ps1
sdkmanager.bat --sdk_root="$env:ANDROID_HOME" 'platform-tools' 'platforms;android-36' 'build-tools;36.0.0' 'ndk;28.2.13676358' 'cmake;3.22.1'
sdkmanager.bat --sdk_root="$env:ANDROID_HOME" --licenses
```

首次编译音频解码库还要安装 Visual Studio C++ x64/x86 Build Tools 与 Windows SDK。确保 `Get-Command bash.exe` 指向 Git for Windows 的 Bash，`curl.exe`、`tar.exe` 和 `python` 可用。Windows FFmpeg 命令行程序随下方独立 Python 工具环境安装。构建 Android 音频库的工具链单独配置。

### 第三方依赖

| 依赖 | 安装方式 |
| --- | --- |
| Live2D Cubism SDK for Native 5-r.5 | 从 [Live2D](https://www.live2d.com/en/sdk/download/native/) 阅读并接受适用条款，在 `$env:SDVX_TOOLCHAIN/native-deps/CubismSdkForNative-5-r.5` 保留完整的 `Core`、`Framework` 和许可文件。另见 [SDK 发布条款](https://www.live2d.com/en/sdk/license/)。 |
| BASS、BASS_FX for Android | 从 [Un4seen](https://www.un4seen.com/) 按其许可获取 Android 包，分别解压至 `native-deps/bass` 和 `native-deps/bass-fx`。目录内应有 `c/bass.h`、`C/bass_fx.h` 和对应 ABI 的库。 |
| SDL3、RmlUi、FreeType、stb、libwebp、nlohmann/json、FFmpeg | 构建脚本从上游获取指定版本。具体版本与路径在 [Build-SDVX.ps1](../tools/Build-SDVX.ps1) 和 [Ensure-SdvxAudioCodec.ps1](../tools/Ensure-SdvxAudioCodec.ps1)；首次运行需要联网。 |
| Python 工具环境 | `Install-SourceTools.ps1` 创建独立 venv，安装固定版本的解析模块及提供 Windows FFmpeg CLI 的 `imageio-ffmpeg`，并输出 Python 和 FFmpeg 的路径。首次安装需要联网。 |

```powershell
& .\tools\Install-SourceTools.ps1 -Toolchain $env:SDVX_TOOLCHAIN
```

## 2. 从 HDD 直接构建

从纯净的本地 `contents`、它的 `data` 子目录、或包含它的版本目录开始。识别依据是 `data/others/music_db.xml` 和 `modules/soundvoltex.dll`；根目录可以是 `E:\KFC\contents`，也可以是诸如 `E:\2026101300\LAZY_KFC\contents` 的布局。若一个目录下有多套数据，请直接指定其中一套的 `contents`。原盘作为只读输入使用。

完成第 1 节的 Android 工具链和厂商 SDK 配置后，从独立的 ASCII 源码路径运行：

```powershell
& .\tools\Build-FromHDD.ps1 -Hdd 'E:\KFC\contents' -Toolchain 'D:\Dev\sdvx-source-tools'
```

首次调用会在缺少 venv 时运行 [Install-SourceTools.ps1](../tools/Install-SourceTools.ps1)，使用该环境中的 Python 和 Windows FFmpeg。正常流程依次生成曲库和编曲、C++ 登记表、模型和语音、角色选择器和副屏、街机界面和舞台资源，再运行 Gradle/CMake、组装、对齐和签名。在 SDK、依赖以及原盘之外，不需要旧工程的素材目录、上一版 APK 或模拟器。

| 参数 | 用途 |
| --- | --- |
| `-SongIds 1,2,3` | 只内置这些 ID 对应的原盘目录。省略时采用 [默认内置清单](SONGS.md) 与本机数据的交集。 |
| `-PrepareOnly` | 只转换并检查本地输入，完成后若要封包，使用同一工具链运行 `& .\tools\Build-SDVX.ps1`。 |
| `-ServiceData 'E:\…\nbl.ts'` | 可选。本地第三方服务提供的初学者文件夹和段位课程表。懒人目录下的 `asphyxia/plugins/sdvx@asphyxia/data/nbl.ts` 会被自动识别。纯净版缺少该文件时，初学者分组没有服务标记、课程列表为空，其他流程继续。无需运行第三方启动器或服务。 |
| `-Offline` | 让 Gradle 使用已经取得的插件缓存。首次安装 Python 包、获取原生依赖仍需网络。 |

APK 输出在 `development/android-native/build/outputs/apk/debug/sdvx-android-native-debug.apk`，为本机 Android 调试密钥签名的手机包。制作与该 APK 配套的外置歌曲包和角色包，见[增量包格式、制作与手机安装](INCREMENTAL.md)。

### 版本与曲库差异

原盘按目录结构识别。角色、贴纸、判定和舞台登记目前按 `2026101300` 的 DLL 布局提取。其他版本若提示登记表无法读取，需为对应模块适配偏移。

**曲目文件夹、文件名及内容保持 HDD 原样。** 例如选中 ID 1 后，APK 的 assets 下有：

```text
music/0001_albida_muryoku/
  0001_albida_muryoku.s3v
  0001_albida_muryoku_pre.s3v
  0001_albida_muryoku_1n.vox
  jk_0001_1.png
  jk_0001_1_b.png
  …同一原目录的其他文件
```

手机版仅为所选内置歌曲复制目录，保留原曲所有规格的封面。其他歌曲只保留用于识别的轻量元数据，不导出整盘封面。运行时在进入打歌时解析原始 VOX，正曲与试听从原名 S3V 读取。仅供手机版使用的索引、判定参数、专属舞台演出及 1438 动态编曲在独立的 `songs/` 中，原曲目录不混入生成 JSON。

**手机端增量添加。** 把同版 HDD 上的单首或多首目录，连同里面的原始文件，复制到 `Android/media/local.sdvx.nativeport/resources/music/`。例如复制后应为 `resources/music/0002_broken_iroha/0002_broken_iroha_1n.vox`。完全退出并重新打开应用后，选曲器扫描实际目录，只显示本地存在、具备谱面、音轨及封面的难度；图片沿用按需加载。如果系统打开文件夹选择器，请在 `Android/media/local.sdvx.nativeport/resources/` 选择“使用此文件夹”并确认，让游戏读取从电脑或文件管理器复制的封面。系统会记住这个目录的授权，以后可以继续往 `music/` 里加歌。新增已登记歌曲不需要重新安装 APK。跨版首次出现的歌曲需要用包含它的音乐数据库重建索引。动态拼曲 1438 还需要从 HDD 单独的编曲素材库生成演出资源，构建时如需它请将 ID 1438 加入内置选择。

工具不会按历史曲数判断成败。找不到被选的目录、难度或可用输入时，会写入本地 `songs/catalog-audit.json`。如果 APK 没有内置可玩歌曲，请先复制至少一个已登记的完整原曲目录，再首次进入游戏。外置原目录也可用 `development/android-native/tools/pack-download-content.py` 按 ID 输出目录或 ZIP；参见该工具的 `--help`。

**打歌背景直接读取 IFS。** 原盘的 `data/graphics/game_bg/gmbg_0101_bridge.ifs` 在 APK 中保留为 `assets/graphics/game_bg/gmbg_0101_bridge.ifs`。其他用到的归档同样按原名、原字节存储。构建时生成本地 `graphics/stage-ifs.json`，为 AFP、BSI、GE2D 和纹理记录归档及字节范围。手机在现有图片加载队列内按需读取、解压和上传，继续使用 SDL3 和 GLES。AVSLZ、DXT5 和 BGRA 解码随本项目的 C++ 源码编译，手机端无需 Python 或额外第三方运行库。

`stage_bg/0/tex/bg_nor_00.png` 这样的名字只作为虚拟纹理键供 AFP 引用，包里不再包含这份解出的 PNG。独立的场景清单、原盘本来就是 PNG 的隧道贴图和 Live2D 模型/贴图各自保留。菜单界面的既有素材流程不受此项变更影响。

### 排查或恢复中断

总入口调用 [prepare-hdd.py](../tools/prepare-hdd.py)。在同一份数据转换中断后，可以只重跑失败的步骤。换另一套 HDD 时使用新的干净克隆和独立 staging，避免旧资源混入本次结果。

```powershell
$python = Join-Path $env:SDVX_TOOLCHAIN 'venv/Scripts/python.exe'
$ffmpeg = & $python -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())'
$sdk = Join-Path $env:SDVX_TOOLCHAIN 'native-deps/CubismSdkForNative-5-r.5'
& $python .\tools\prepare-hdd.py 'E:\KFC\contents' --sdk $sdk --ffmpeg $ffmpeg --steps graphics
& $python .\tools\check-build-inputs.py
```

`--steps` 支持 `songs`、`player` 和 `graphics`。背景转换需要 `songs` 步骤生成的 `local-inputs/stage-registration.json`。Gradle 合并 `development/android-native/assets-reference` 和 `development/android-native/src/main/assets`，同路径以后一层为准。

封包前会检查必要的运行时目录、生成的 C++ include，以及所选曲目实际需要的封面、VOX 和音频。

## 3. 给本地 AI 的交接文本

> 当前工程是 SDVX ∇ MOBILE。我的 HDD 在［本地路径］。请先按 docs/BUILD.md 配置 Windows 工具链和已获许可的 SDK，然后用 Build-FromHDD.ps1 生成 arm64 手机 APK。使用本地命令读取 HDD，不上传原盘、日志中的私有数据、生成资源或 APK。只为所选歌曲打包原目录和封面；其他已登记曲目从手机本地 music/ 增量读取。遇到不支持的 DLL 时，报告读不出的登记表和具体错误，不套用其他版本偏移。

## 4. 提交范围

Git 提交源码、构建脚本、文档，以及维护者明确批准的四张图片：两层 Android 启动图标和手机版触屏 BT/FX 各一张。原盘、曲目、角色、其余游戏媒体、SDK、签名密钥和 APK 保留在本机。本机验证记录、截图和日志放在 `evidence/`。提交前用 `git diff --cached --name-only` 审阅实际文件。
