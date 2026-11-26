# Windows x64 SDK 验收

## 逐 word 修复版开发安装

源码改动已分别提交到 `netcode` 的 `9828d9e` 和示例仓的 `25925e5`。提交内容在独立工作树中通过 Core 9 项测试，车场 `match` 和 `prediction` 分别在修复版和上一发布版 Core 下通过。上一发布版 Core 为 `7c86097d5ebf426f`。[提交与回归范围](roadmap.md#godot-原生节点与-sync-lab)

2026-11-25 验收的 Windows x64 Release 开发安装使用 Core `929c4f1663907b23` 构建三个平台 SDK。路径以框架仓库根目录为准。平台本身的构建标识位于斜杠之后。使用 `build-id.txt` 可复核实际安装的版本。

| 产品 | 本地安装路径 | 已核实的验证范围 |
| --- | --- | --- |
| Core | `out/install/win-x64-msvc-mt/Release/core` | CTest 9/9，车场新旧版本分别作为房主均通过回环状态与写权提交测试 |
| Godot | `out/install/win-x64-msvc-mt/Release/godot/addons/netcode` | `929c4f1663907b23/cbd0dce8d269280e`。原生 Shared 双端和 sync-lab 1024 对象无窗口通过 |
| Unity | `out/install/win-x64-msvc-mt/Release/unity` | `929c4f1663907b23/d155c48545cdd908`。Release SDK 构建通过 |
| Cocos2d-x | `out/install/win-x64-msvc-mt/Release/cocos` | `929c4f1663907b23/4af80b708c71ecbb`。Release SDK 构建通过 |

复建开发版时，在框架仓库根目录运行以下命令。Godot 的原生与兼容构建共用同一个安装目录。

```powershell
python -B tools/build.py core
python -B tools/build.py godot --core-prefix out/install/win-x64-msvc-mt/Release/core --godot-native-only
python -B tools/verify_godot_native.py --case shared
```

在同级示例仓库根目录用 `--sdk` 指向开发安装。已核实 1024 档客机持续推进 1023 个远端对象，AOI 可见数依次为 1024、524、524、1024。2026-11-26 另用随附插件和表中 Godot 开发插件跑了四间全量可见的无窗口双人房，记录了同步字节和节点耗时。双窗口帧率需在渲染负载相同的条件下另测。[每球 8 属性与 1024 球开销](../task/godot-platform/验收/sync-lab单球同步开销.md) · [sync-lab 运行说明](../../examples/sync-lab/README.md#验证逐-word-开发版)

```powershell
python -B tools/verify_sync_lab.py 1024 --sdk ../netcode/out/install/win-x64-msvc-mt/Release/godot/addons/netcode --seconds 36 --require-proxy-changes
```

## 2026-11-25 随附 Godot 原生包与 sync-lab

2026-11-25 取样时，本机 `out/packages/godot-native-win-x64/current/netcode-godot-native-win-x64.zip` 的构建标识为 `5ce4ee1e67e62812/cbd0dce8d269280e`。归档的八个文件与 sync-lab 随附插件逐字节一致，均无 `.gd`。这组字段格式基线已验证缺 DLL 补装、原生双端回归和 Godot 4.7.1 导出的 Windows 双端。示例依赖锁的 `godot` 条目指向旧脚本兼容包，原生与兼容归档分别轮换。[部署、校验值与复跑](../task/godot-platform/验收/T5.md#sync-lab-随附插件部署)

## 2026-11-24 审核修复

Core、TCP、Godot、Unity 和 Cocos 的 Windows x64 Release 验收已归档。[验收结果与包校验值](archive/2026-11-24-netcode-audit.md)

## 2026-11-22 历史验收

验收对象为 0.1.0 Release、MSVC v145、C++17、静态 CRT、IPv4 直连。Godot 使用 4.7.1，原生绑定使用 godot-cpp 4.5-stable。Unity 使用 6000.4.0f1 Windows Player。Cocos2d-x 使用工作区 4.x 无窗口分支，宿主启用 Chipmunk 与 Bullet。

| 交付物 | 实际验证 | 结果 |
| --- | --- | --- |
| Core | 从 ZIP 解压到工作区外带空格的新路径，空白 CMake 项目仅通过 `Netcode::Direct` 编译和链接。两个进程验证房间回合、锁房和超时，另用 /MD 工程故意链接 /MT SDK | 双端场景通过。错误 CRT 在编译时报 `NetcodeCore CRT mismatch`。原生 Core CTest 9/9 |
| Godot | 空项目安装插件 ZIP，导出 Windows 可执行程序，两个独立进程通过原生扩展建立房间 | 两端构建标识一致。八个字段、对象引用、二维与三维刚体、双向 RPC、分数 42 均通过 |
| Cocos2d-x | 解压 Core、Cocos 两个 ZIP，新 CMake 工程复用已有的引擎库，单独编译并启动两个无窗口客户端 | 两端运行时返回期望的构建标识。同一字段、刚体与 RPC 用例通过 |
| Unity | 空 Unity 工程经 Package Manager 安装本地 tgz，创建单场景 Windows Player，独立启动房主与客人 | 两端构建标识、八个字段、引用、刚体、双向 RPC 和分数均通过 |

| Core 构建标识 | Godot 平台 | Cocos 平台 | Unity 平台 |
| --- | --- | --- | --- |
| `3f89f0ca652f25a1` | `fe127bfe8e3c902d` | `7eb957298a76cb34` | `4b2480e12f80bbb7` |

| 历史归档 | SHA-256 |
| --- | --- |
| Core | `3e09437389ee31678fd34dde6c74f28c9c30a7ff25f43098c2fc4117fa87a8e6` |
| Godot | `a0c96092acdd9b88e8f9a19274c3efb262f4b3a33c55b07ac8b3ca18d874da92` |
| Cocos | `eaad6d2e7257f5024e153cb53f1b0cb6b9c1ab30c9df307f0f2f88b22513addf` |
| Unity | `8da0024644caa40cc620941c513b05f685d8eb3b8e41b620a3b3b81294af212f` |

## 复现

在 SDK 仓根执行以下命令。工具默认使用本机 VS 2026，可显式提供 CMake、Cocos 源码或 godot-cpp 路径。

```powershell
python -B tools/build.py all --package
python -B tests/integration/verify_install.py out/packages/core-win-x64/current/netcode-core-win-x64.zip
python -B tests/integration/test_artifacts.py
```

示例消费脚本按[当前示例依赖锁](../../examples/sdk-lock.json)查找归档。Core 与 Cocos 必须来自同一 Core 构建。Cocos 根目录的 `build-id.txt` 包含其依赖的 Core 标识，可与 Core 根目录的同名文件核对。

在[示例仓](../../examples/README.md)根目录执行 `python -B tools/verify_sdk_install.py godot`、`python -B tools/verify_sdk_install.py unity` 和 `python -B tools/verify_cocos_install.py`。最后一条使用已配置的 Cocos 宿主引擎目标，需先构建带两种物理引擎的 Cocos 车场。可用 `--engine-build` 指定已有的宿主构建目录。

示例仓还已通过车场的 match/prediction 两个 CTest、三组同引擎玩法双端、Godot 驾驶、插件选择器与场景绑定，以及 Shiftline 的八人入房、关卡移动、对局、重开和动作回归。旧的单仓构建树仍保存于本地，原始验收文档中的历史构建路径按当时环境理解。新游戏包由示例仓管理。

另外用安装好的 Godot SDK 跑了 `python -B research/fusion/probes/rigid_scale.py native 2`。两个刚体的标签、位置和休眠状态均收敛。房主日志中还有两条读取树外节点全局变换的引擎报错，当前保留在复验记录中。
