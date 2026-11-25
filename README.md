<h1 align="center">SDVX ∇ MOBILE</h1>

<p align="center">《SOUND VOLTEX》第七代 NABLA 的非官方 Android 竖屏复现项目</p>

<p align="center">
  <img alt="Android 8.0 及以上，64 位 ARM" src="https://img.shields.io/badge/Android-8.0%2B_%C2%B7_arm64-2E765C?style=flat&amp;logo=android&amp;logoColor=white&amp;labelColor=303944">
  <img alt="OpenGL ES 3.2" src="https://img.shields.io/badge/OpenGL_ES-3.2-456787?style=flat&amp;labelColor=303944">
  <a href="LICENSE"><img alt="MIT 许可证" src="https://img.shields.io/badge/License-MIT-655B86?style=flat&amp;labelColor=303944"></a>
</p>

<p align="center">
  <a href="docs/BUILD.md">Windows 构建指南</a> ·
  <a href="docs/INCREMENTAL.md">曲目与角色增量包安装说明</a>
</p>

## 项目现状

当前版本已复现部分街机界面与演出，支持领航员切换、选曲、打歌和本地成绩结算。APK 在 Windows 上使用自备的 HDD 数据构建，已验证的 HDD 版本为 `2026101300`。

| 功能 | 当前状态 |
| --- | --- |
| 触屏与激光判定 | 菜单支持触控。打歌时触屏操作仍不稳定，激光判定存在偏差。 |
| Type-C 手台 | 可控制菜单，打歌时输入延迟较高，难以正常游玩。 |
| 段位考核 | 考核流程尚未实现。 |
| 刷卡与联机 | 实体刷卡和联机游玩尚未接入。 |

## 许可

项目自有代码和文档采用 [MIT 许可证](LICENSE)。游戏素材、商标及第三方 SDK 的权利归各自权利人所有。仓库附带的启动图标和触屏按键图不在 MIT 授权范围内。
