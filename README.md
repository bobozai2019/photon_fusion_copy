# 跨引擎联机框架

本项目分为可移植 C++ `core` 和引擎接入层 `engineplatform`。首个接入引擎是 Cocos2d-x 4，在 Windows x64 上运行同引擎房主和客人。双方加载声明式 Replicator 配置，可视化工具留到后续。属性同步、RPC 和底层框架参照 Fusion；Godot Racing 样例用于研究原版。[范围与访谈记录](task/cross-engine-sync/总纲.md)

正式框架全部自研，可在无 Photon SDK 的环境交付。首个连接后端通过 TCP 管房间、UDP 传游戏数据，连接模块可独立替换。房主运行游戏逻辑并广播权威状态，客人按快照对显示位置插值。首版覆盖 2D/3D 变换，优先可玩样例，在同一台 Windows 主机上运行房主和客人。目标负载为 4 人、100 个复制对象。房主退出或失联后本局结束。

当前 Windows x64 车场支持双窗口驾驶、开门、路障和断线接回原车。车场和轻量客人走同一房间与游戏协议，本机已验证四人百对象。进展和平台边界见[执行状态](task/cross-engine-sync/执行状态.md)。

## 构建和运行

在仓库根目录运行 PowerShell。Windows 构建脚本从 Visual Studio 2026 定位 MSVC、CMake 和 vcpkg，使用 Python 3.11+，Cocos 源码默认为同级的 `cocos2d-x-4`。

```powershell
python -B tools/build.py
python -B tools/build.py --cocos
python -B tools/verify_yard.py play --capture
python -B tools/verify_yard.py stress --seconds 10 --fault
python -B tools/package_yard.py --verify-play
```

独立核心也可直接以 `core/` 为 CMake 源目录。Cocos 应用的启动命令和键位见[车场说明](examples/car-yard/README.md)。原版研究入口独立使用本地 Godot 和 Photon SDK。[基线复现](research/fusion/probes/README.md)

## 仓库导航

[架构技术文档](docs/architecture.md)涵盖模块、协议、对象生命周期、RPC、重连和验收。

第二套引擎接入使用 Godot 4 GDExtension。Godot 与 Cocos 样例从同一 C++ 目标获取房间、TCP/UDP、模拟和复制，沿用同一验收脚本。[Godot 构建和操作](examples/godot-yard/README.md) · [Godot 接入任务](task/godot-platform/总纲.md)

| 路径 | 在这里维护什么 |
| --- | --- |
| [AGENTS.md](AGENTS.md) | 接续任务、判断证据、纠正漂移的约定 |
| [docs/specs/](docs/specs/) | 跨端行为、可观察结果和验收条件 |
| [docs/decisions/](docs/decisions/) | 自研取舍、适用范围，以及新证据到来时需要重审的边界 |
| [Fusion 研究资料](research/fusion/README.md) | Godot Shared Authority 与 Fusion Core 官方文档入口、构建指纹、研究记录和探针 |
| `core/{state,session,transport}/` | 通用核心层：状态和编码、会话调度、TCP 房间与 UDP 游戏通道 |
| `engineplatform/cocos2dx/` | 首发引擎接入层：声明式配置、节点绑定、寿命管理、引擎取值与应用 |
| `tests/{core,conformance,integration}/` | 无引擎测试、与基准的行为比较、跨端集成 |
| `examples/`、`tools/`、`out/` | 可运行示例、复现工具、本地输出。[产物路径与清理规则](AGENTS.md#产物路径与清理) |

当前工作区使用同级目录 `photon-fusion-godot-starter-kit-racing_3.0.0/`、`position-replicator-example/` 和 `cocos2d-x-4/`。前两者是基线输入，教学示例的结果单独登记。新仓库可独立维护文档和规格；运行现有探针时需要这些本地依赖中的相应部分。
