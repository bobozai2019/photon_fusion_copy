# Windows x64 SDK 验收

2026-11-22。验收对象为 0.1.0 Release、MSVC v145、C++17、静态 CRT、IPv4 直连。Godot 使用 4.7.1，原生绑定使用 godot-cpp 4.5-stable。Unity 使用 6000.4.0f1 Windows Player。Cocos2d-x 使用工作区 4.x 无窗口分支，宿主启用 Chipmunk 与 Bullet。对应的四个发布包 SHA-256 记录在[示例依赖锁](../../examples/sdk-lock.json)。

| 交付物 | 实际验证 | 结果 |
| --- | --- | --- |
| Core | 从 ZIP 解压到工作区外带空格的新路径，空白 CMake 项目仅通过 `Netcode::Direct` 编译和链接。两个进程验证房间回合、锁房和超时，另用 /MD 工程故意链接 /MT SDK | 双端场景通过。错误 CRT 在编译时报 `NetcodeCore CRT mismatch`。原生 Core CTest 9/9 |
| Godot | 空项目安装插件 ZIP，导出 Windows 可执行程序，两个独立进程通过原生扩展建立房间 | 两端构建标识一致。八个字段、对象引用、二维与三维刚体、双向 RPC、分数 42 均通过 |
| Cocos2d-x | 解压 Core、Cocos 两个 ZIP，新 CMake 工程复用已有的引擎库，单独编译并启动两个无窗口客户端 | 两端运行时返回期望的构建标识。同一字段、刚体与 RPC 用例通过 |
| Unity | 空 Unity 工程经 Package Manager 安装本地 tgz，创建单场景 Windows Player，独立启动房主与客人 | 两端构建标识、八个字段、引用、刚体、双向 RPC 和分数均通过 |

| Core 构建标识 | Godot 平台 | Cocos 平台 | Unity 平台 |
| --- | --- | --- | --- |
| `3f89f0ca652f25a1` | `fe127bfe8e3c902d` | `7eb957298a76cb34` | `4b2480e12f80bbb7` |

## 复现

在 SDK 仓根执行以下命令。工具默认使用本机 VS 2026，可显式提供 CMake、Cocos 源码或 godot-cpp 路径。

```powershell
python -B tools/build.py all --package
python -B tests/integration/verify_install.py out/packages/core-win-x64/current/netcode-core-win-x64.zip
python -B tests/integration/test_artifacts.py
```

在[示例仓](../../examples/README.md)根目录执行 `python -B tools/verify_sdk_install.py godot`、`python -B tools/verify_sdk_install.py unity` 和 `python -B tools/verify_cocos_install.py`。最后一条使用已配置的 Cocos 宿主引擎目标，需先构建带两种物理引擎的 Cocos 车场。

示例仓还已通过车场的 match/prediction 两个 CTest、三组同引擎玩法双端、Godot 驾驶、插件选择器与场景绑定，以及 Shiftline 的八人入房、关卡移动、对局、重开和动作回归。旧的单仓构建树仍保存于本地，原始验收文档中的历史构建路径按当时环境理解。新游戏包由示例仓管理。

另外用安装好的 Godot SDK 跑了 `python -B research/fusion/probes/rigid_scale.py native 2`。两个刚体的标签、位置和休眠状态均收敛。房主日志中还有两条读取树外节点全局变换的引擎报错，当前保留在复验记录中。
