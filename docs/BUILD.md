# 在 Windows 上用本地数据构建

目标为 Windows x64 上生成 arm64-v8a、OpenGL ES 3.2 的 Android 调试包，最低 Android API 26。普通手机构建无需安装模拟器或系统镜像。文中 PowerShell 命令从本仓库根目录执行。

## 1. 配置工具链

使用 PowerShell、Python 3.10 或更新版本、Git for Windows、JDK 17 和 Gradle 8.13。用一个独立的 ASCII 目录作工具链根目录。如果电脑上还有旧开发工程，请给这个克隆使用不同的根目录，因为构建脚本会在里面建立指向当前源码的目录联接。

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

首次编译音频解码库还要安装 Visual Studio C++ x64/x86 Build Tools 与 Windows SDK。确保 `Get-Command bash.exe` 指向 Git for Windows 的 Bash，`curl.exe`、`tar.exe` 和 `python` 可用。转换音频还需可在 Windows 上运行的 FFmpeg CLI；Android 目标的 FFmpeg 动态库不能用作这个命令行程序。

### 第三方依赖

| 依赖 | 安装方式 |
| --- | --- |
| Live2D Cubism SDK for Native 5-r.5 | 从 [Live2D](https://www.live2d.com/en/sdk/download/native/) 阅读并接受适用条款，在 `$env:SDVX_TOOLCHAIN/native-deps/CubismSdkForNative-5-r.5` 保留完整的 `Core`、`Framework` 和许可文件。另见 [SDK 发布条款](https://www.live2d.com/en/sdk/license/)。 |
| BASS、BASS_FX for Android | 从 [Un4seen](https://www.un4seen.com/) 按其许可获取 Android 包，分别解压至 `native-deps/bass` 和 `native-deps/bass-fx`。目录内应有 `c/bass.h`、`C/bass_fx.h` 和对应 ABI 的库。 |
| SDL3、RmlUi、FreeType、stb、libwebp、nlohmann/json、FFmpeg | 构建脚本从上游获取指定版本。具体版本与路径在 [Build-SDVX.ps1](../tools/Build-SDVX.ps1) 和 [Ensure-SdvxAudioCodec.ps1](../tools/Ensure-SdvxAudioCodec.ps1)；首次运行需要联网。 |
| Python 导出环境 | 建议单独创建虚拟环境。按脚本需要安装 Pillow、numpy、pefile、capstone、[bemaniutils](https://github.com/DragonMinded/bemaniutils)（导入名 `bemani`）及 [ifstools](https://github.com/mon/ifstools) CLI。AFP 导出使用 bemaniutils 的 AFP/IFS 子模块，兼容版本组合尚未在干净机器上锁定。 |

## 2. 接入本地原盘

输入基线为 `2026101300/LAZY_KFC/contents`。多数工具读其 `data` 子目录；登记表还要读 `modules/soundvoltex.dll`，只有 `data` 不够。将自己的版本目录放在仓库根的 `2026101300`，或在该位置建立指向它的目录联接。下面示例的目标目录应直接包含 `LAZY_KFC`。

```powershell
New-Item -ItemType Junction -Path .\2026101300 -Target 'E:\MyData\2026101300'
$app = 'development/android-native'
$data = '2026101300/LAZY_KFC/contents/data'
$assets = "$app/src/main/assets"
Get-Item "$data/others/music_db.xml", '2026101300/LAZY_KFC/contents/modules/soundvoltex.dll'
```

原盘只用作本地输入。中间文件可放在已被 Git 忽略的 `local-inputs/`。端上使用的资源分两层：`$app/assets-reference` 存放共用资源，`$assets` 存放补充和覆盖项。相同路径优先取后者。保留仓库中的 `$assets/ui` 界面源码。

## 3. 准备构建输入

**已有自己合法准备的完整导出时**，按原相对路径放回上述两层目录，并准备本地登记表和启动图标。不要将整份原盘直接复制到 Android 的 assets 目录。只有 HDD 时，下面列出实际转换入口；它们覆盖不同环节，并非完整流水线。

| 环节 | 输入、工具与输出 |
| --- | --- |
| 曲谱和曲目目录 | `pack-song-library.py` 读 `data`、本地判定 JSON（含 `judgment` 和 `offsets`）、选定的曲目 ID，生成已内置曲目的目录。`prepare-song-catalog.py` 在该目录上补全曲库，向 `src/main/assets/songs` 写入 `library.json` 等。打包工具另有两个现存输入约定：外置曲包使用 `assets-reference/songs/666/advanced.json` 的判定字段，动态编曲使用 `assets-reference/songs/840/gravity.json`。单独运行曲库索引工具不会生成可玩谱面。 |
| 舞台 | 有曲库后，按下方命令从本地 DLL 生成 C++ 必需的 `.inc`。转换共用背景时给 `prepare-stage-backgrounds.py` 传入同一次生成的完整 JSON（`--registration`）、已解出的 game_bg 目录和 `--game-textures`。专属背景另用 `prepare-stage-scenes.py`。 |
| 角色、语音和着色器 | `prepare-navigator.py` 读取本地 Live2D 资源、语音库、Cubism SDK 和 FFmpeg CLI，将领航员资源及 SDK 许可文件写入 `src/main/assets/crew`。`compile-crew-catalog.py` 另需本地角色登记表 `tools/data/crew-catalog-original.json`和已解出的角色图形目录。 |
| 界面、HUD、打击特效 | `prepare-cabinet-afp.py` 按场景名读取 `data/graphics/ver07`。`compile-hud.py`、`compile-button-hits.py` 等各自产出局部目录。多数脚本提供 `--help`。 |
| 选曲、段位和贴纸 | `prepare-selection.py` 从本地 DLL 读取比赛名称，同版目录若含 `2026101300/LAZY_KFC/asphyxia/plugins/sdvx@asphyxia/data/nbl.ts`，可直接将其传给选曲和段位转换器，分别读取 `BEGINNER_MUSIC_FOLDER` 和 `COURSES7`，不需要启动服务。生成资源不会实现段位玩法。`compile-subscreen.py` 读取脚本旁本地生成的 `sticker-metadata.json`。 |
| 启动图标 | Android 资源链接要求 `src/main/res/drawable-xxxhdpi/ic_launcher_background.png` 和 `ic_launcher_foreground.png`。提供有权使用的背景与前景图层。 |

以下示例共用第 2 节的变量。入场提示文案直接从本地 DLL 提取，打击特效工具只生成一个局部资源组。舞台登记表转换要求 `src/main/assets/songs/library.json` 已准备好。

```powershell
python "$app/tools/prepare-entry-notice.py"
python "$app/tools/compile-button-hits.py" "$data/graphics/ver06/game_effect.ifs" "$assets/button_hit"
New-Item -ItemType Directory -Force -Path local-inputs | Out-Null
python "$app/tools/extract-stage-registration.py" --root . --output local-inputs/stage-registration.json --runtime-inc "$app/src/main/cpp/stage_registration_2026101300.inc"
```

**已知的重建缺口**：完整共用素材的首轮导出、内置曲目的判定输入、角色登记表和贴纸元数据还没有一套仅凭原盘即可复现的流程。例如 `compile-cabinet-ui.py` 的 `case` 参数需要带 `playthrough/scenes/*_ifs/tex`、`original-layout/game_hud_ifs/tex` 和解码 WAV 的中间目录。直接传入 HDD 根目录不能满足这些输入。请记录具体缺项，不要用空 JSON、占位音频或 AI 猜测的数据继续封包。外置曲库和角色 ZIP 是可选的本地产物，不是构建依赖的公共下载项。

本地 Android 工程原先选择的内置曲目以 `ID:输出目录` 记在 [内置曲目构建清单](SONGS.md)。这份清单记录打包选择，用它可以重建目录关系。歌曲 1438 的动态编曲还需单独处理。

## 4. 检查和构建

```powershell
python .\tools\check-build-inputs.py
```

检查会列出缺少的关键文件，退出码 0 表示这些入口已就绪。它抽查可玩谱面与核心资源，不能证明每个场景的全部资源已生成。在用户明确需要生成 APK 时，再运行：

```powershell
& .\tools\Build-SDVX.ps1
```

首次允许 Gradle 获取插件。缓存就绪后可传 `-Offline` 让 Gradle 离线解析。脚本使用本机 Android 调试密钥签名，输出 `development/android-native/build/outputs/apk/debug/sdvx-android-native-debug.apk`。若提示目录联接指向其他工程，换用专属于本克隆的工具链目录。不要递归删除指向源码或素材的联接。

## 5. 提交代码

仓库的 `.gitignore` 默认排除文件，只放行源码、构建配置和文档。即使克隆里已有 HDD、SDK、生成的 JSON 或 APK，普通的 `git add` 也不会把这些路径加入索引。新建代码目录时先显式加入规则，提交前用 `git diff --cached --name-only` 审阅实际文件。不要强制加入被忽略的素材，也不要把素材或构建产物上传到 Releases、CI 缓存或工单。
