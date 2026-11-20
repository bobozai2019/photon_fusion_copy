# 参考构建 · Fusion Godot Racing 3.0.0

目标为本地 Racing Starter Kit 3.0.0 的 Shared Authority。此次选择样例实际附带的插件；相邻的独立预览 SDK 压缩包应另行计算指纹。插件通过[扩展清单](../../../../photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/fusion.gdextension)按平台选库。

| 用途 | `addons/fusion/bin/` 中的文件 | SHA-256 |
| --- | --- | --- |
| Linux x86-64 静态分析，地址采用 ELF 虚拟地址 | `libfusion.linux.editor.x86_64.release.so` | `3BCBEC5F76344BB98F82F4F65448AFDF65206400B4268E28A1D4B95D38EBF61B` |
| Windows x86-64 原版布局探针，Godot 4.7.1 stable | `libfusion.windows.editor.x86_64.release.dll` | `104B1E2C3A0289B7391584C75A90F1480A9DAB750099114C99356A813CE69752` |

两个平台分别记录证据。Windows 的 word 数量观测无法给出 Linux 缓存项的成员偏移。

```powershell
# 从 netcode 仓库根目录运行
$bin = '../photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/bin'
Get-FileHash -Algorithm SHA256 -LiteralPath "$bin/libfusion.linux.editor.x86_64.release.so","$bin/libfusion.windows.editor.x86_64.release.dll"
```
