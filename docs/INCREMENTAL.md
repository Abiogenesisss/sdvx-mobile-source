# 手机版曲目与领航员增量包

增量包配合一个具体的 APK 使用。制作工具读取该 APK 的曲库和领航员登记表，从本地 HDD 补上它能识别、且尚未内置的资源。ZIP 在手机本地解压，游戏重启时读取新增内容。

## 在手机上安装

1. 安装目标 APK，启动一次后完全退出。打开手机内部存储的 `Android/media/local.sdvx.nativeport/resources/`。
2. 从交付目录的 `music/` 中任选歌曲 ZIP，各分包都能独立安装。从 `crew/` 中取角色 ZIP 可一次安装整组额外领航员。电脑端可以打开对应的 `index.json` 查询歌曲 ID 或角色 ID 与款式。
3. 将所选 ZIP 的**内容直接解压到同一个 `resources/`**，允许合并目录。关闭解压工具的“解压到同名文件夹”选项。正确路径例如 `resources/music/0001_albida_muryoku/0001_albida_muryoku_1n.vox` 和 `resources/crew/catalog.json`。压缩包旁的 `index.json` 供人和工具查询，放在电脑上即可。
4. 重新打开游戏。Android 16 若弹出系统文件夹选择器，选中上述 `resources` 目录，点击“使用此文件夹”并确认。可在选曲页检查曲名、封面和可选难度，在 MY ROOM 的领航员选择页切换角色。

每个歌曲和角色 ZIP 的根目录都带有零字节隐藏文件 `.nomedia`。解压时保留隐藏文件，使其落在 `resources/.nomedia`，防止曲目封面、音乐和角色素材出现在系统图库或媒体库中。

Android 16 首次加载外置角色贴图时，游戏可通过对 `resources` 的只读授权读取文件。遇到首次贴图空白时，请保留 `.nomedia`，确认已授权的是 `resources` 本身。排查首次读取要选一张刚解压、游戏还未读过的贴图；重启后已能显示的贴图无法复现这个条件。

追加歌曲时可继续解压其他分包，也可以直接复制 HDD 上的一首歌的完整目录。更新角色包时使用同一份 ZIP 中的 `crew/catalog.json`、`crew/voice/index.json` 和随包资源，避免把两个版本的清单拼在一起。旧格式的 `songs/<编号>/` 需要用原 HDD 重新制作；只改文件夹名无法恢复原 VOX、音轨及封面的对应关系。

## 格式第 1 版

| 交付物 | 手机读取的内容 |
| --- | --- |
| `music/SDVX-s3v-songs-*.zip` | ZIP 根为 `music/<HDD 原目录>/`。完整保留 VOX、共用和难度专用 S3V、试听音轨、各规格封面与原文件名。每包最多 500 首；同一首的文件放在同一包。 |
| `crew/SDVX-extra-crew.zip` | ZIP 根为 `crew/`。角色目录登记每个款式，`crew/voice/index.json` 登记语音。模型、纹理、动作、AFP、立绘与语音文件按清单的相对路径存放。只依赖目标 APK 与这个角色包。 |
| `music/index.json`、`crew/index.json` | 电脑端清单。`format` 为 `sdvx-mobile-incremental/1`，`target_apk` 是制作时使用的 APK 文件名。`archives` 列出本次生成的 ZIP、字节数和内容。歌曲以 `song_ids` 标识，款式以 `crew_styles` 中的 `[角色 ID, 款式 ID]` 标识。 |

歌曲包只收录 APK 已登记且本地至少有一个可玩的外置难度的曲目。难度必须有谱面、可用音轨和封面。若 APK 为该难度登记了专用音轨，例如《月光乱舞》GRV 的 `_4i.s3v`，须保留该文件。1438 的动态编曲需要在制作 APK 时选择。首次出现在其他 HDD 版本的曲目或款式，需要先用相应登记数据构建 APK。

## 从 HDD 重新制作

准备三个输入：源码、与手机安装版本对应的本地 APK、自己的 HDD。曲目分包使用 Python 标准库；完整角色重建使用项目的 Python 导出环境和 FFmpeg。首次准备工具可从仓库根目录运行 `& .\tools\Install-SourceTools.ps1 -Toolchain 'D:\Dev\sdvx-source-tools'`。角色登记解析支持已核对的 `2026101300` DLL。

```powershell
$python = 'D:\Dev\sdvx-source-tools\venv\Scripts\python.exe'
$hdd = 'E:\KFC\contents'
$apk = 'D:\Builds\SDVX-phone.apk'
& $python .\tools\build-incremental-packs.py --hdd $hdd --apk $apk --output 'D:\Builds\extra'
```

每次成功构建都会打印一个新的 `release-…/` 交付目录，里面包含本次请求的全部 ZIP、对应清单和安装说明。程序在所有类别都完成后，一次性替换输出根目录的 `latest.json`。自动化先读取一次其中的 `directory`，再从该目录读取 `music/index.json` 或 `crew/index.json` 及其 ZIP。

中断或重试时，已经发布的目录仍可使用。并发构建分别产生独立目录，最后成功切换的版本成为最新交付。仅构建一种资源时，最新交付只包含该类别。旧工具直接写在输出根目录的资源可按原清单继续使用。确认旧交付已无人使用后可自行清理以释放空间。

只做角色、只做音乐或挑选几首时，可用以下命令。

```powershell
& $python .\tools\build-incremental-packs.py --hdd $hdd --apk $apk --part crew  --output 'D:\Builds\crew-only'
& $python .\tools\build-incremental-packs.py --hdd $hdd --apk $apk --part music --output 'D:\Builds\music-only'
& $python .\tools\build-incremental-packs.py --hdd $hdd --apk $apk --songs 1 2 3 --output 'D:\Builds\three-songs'
```

工具按实际匹配结果生成，已内置、未登记或缺少必要文件的曲目会跳过。角色和歌曲素材、生成清单、ZIP 与 APK 均保存在本地。准备源码仓库时仅提交制作程序和文档。
