# 通用游戏同步框架

项目目标是用一套引擎无关的 C++ 核心支持多种引擎。每种引擎通过自己的 `engineplatform` 适配层运行多人游戏，分别支持 Godot ↔ Godot、Unity ↔ Unity、Cocos2d-x ↔ Cocos2d-x。[同引擎联机验收标准](docs/specs/game-session.md#同引擎客户端同步)

框架可独立于 Photon SDK 构建和运行。普通玩家开房后，房主可以模拟对象，也可以把某个对象的状态写权授给客人。多个玩家可以分别持有不同对象。启用热备并满足接替条件时，其他玩家可以接任房主。玩法决定谁能操作对象以及如何更新，C++ Core 负责授权、复制和恢复。[对象写权](docs/specs/object-authority.md) · [房主接替条件](docs/specs/game-session.md#玩家接替房主与本地找房)

## 接入一个新玩法

Godot 玩法可以直接使用原生 `@rpc` 注解，并在 Replicator 上选择要同步的属性。场景接入路径会生成字段和方法布局。[Godot 声明式接入](docs/specs/godot-scene-sync.md)

1. **声明要同步的数据。** 在模板中给字段分配稳定编号。当前支持 16 类字段，包括位置、旋转、颜色、房内对象引用和有容量上限的集合。[字段类型与容量](docs/specs/property-binding.md#配置与字段值)
2. **登记客户写权。** 在各端开局前注册对象模型，列出允许客人提交的字段号。控制台同步八个字段，只把分数字段 1 列为可写。客人持续推进会话，取得授权后才修改分数。[控制台接入顺序](../examples/puzzle/README.md#通用玩法控制台)
3. **注册玩法方法。** 在开局前为 RPC 声明名称、方法号、参数模板和调用权限。控制台示例中，客人获权后把分数从 7 改为 42，并调用 `Report`。房主读到新分数、核对参数后，用 `Done` 确认处理完成。[可运行的控制台示例](../examples/puzzle/README.md#通用玩法控制台)
4. **挂载组件并绑定对象。** 在 Godot、Unity 或 Cocos2d-x 中把场景实例绑定到同一局的对象号。组件在写者一侧采集变化，在观察者一侧应用状态；写权交接时先恢复基线。[三端组件入口](docs/specs/game-session.md#玩法字段命名调用与挂载组件)

Windows x64 本机已分别验收三种引擎的房主与客人。控制台覆盖八个实际字段、对象引用解析、二维与三维刚体，以及双向命名 RPC。车场另有写权交接、断线接回原车和四人百对象用例。[控制台验收](task/gameplay-components/验收/本机.md) · [车场验收](task/cross-engine-sync/验收/M6.md)

### 新游戏使用纯 TCP

安装对应的平台 SDK，使用下表的会话入口。房主传入本机监听地址，客人传入房主地址。同机双开可在两端使用 `127.0.0.1:29000`，通过 `host` 或 `options.host` 区分角色。省略 provider 参数即可装载 SDK 自带的 TCP 模块。当前候选及默认发布进度见[传输验收](task/transport-providers/执行状态.md)。

| 平台 | 默认 TCP 会话入口 |
| --- | --- |
| Godot | `game.open_with_provider(layout_text, host, endpoint, 4, false)`，导出器会拷贝 TCP DLL |
| Unity | `NetcodeGame.OpenWithProvider(layoutText, host, endpoint)`，UPM 包将 TCP DLL 安装为 Win64 原生插件 |
| Cocos2d-x | `GameComponent::create_with_provider(endpoint, options, layout)`，将 SDK 的 `bin/netcode_transport_tcp.dll` 随宿主可执行文件部署 |

要换用自有传输，使用 SDK 中的 `include/netcode/transport_provider.h` 编译 v1 DLL，再将 provider 标识和模块路径交给同一会话入口。房主和客人使用同款驱动。接回时使用相同配置，并传入原会话的接回凭证。[传输接口与配置](task/transport-providers/传输接口.md)

## 独立构建与外部接入

Windows x64 首版为 0.1.0，使用 MSVC v145、C++17 和静态 CRT。Release 用 /MT，Debug 用 /MTd。Core、Godot、Cocos、Unity 各有独立 CMake 工程。平台以安装后的 Core 作为构建输入。运行 Godot 或 Unity 游戏只需相应平台插件。Cocos 宿主需要匹配的 Core SDK 和自己的 Cocos2d-x 4 引擎目标。

从本仓库根目录执行。构建驱动使用 VS 2026 或显式传入的 CMake，先增量构建 Core，再构建选定平台。仅在提供 `--package` 时创建发布包。

```powershell
python -B tools/build.py core --package
python -B tools/build.py transport-tcp --package
python -B tools/build.py godot --core-prefix out/install/win-x64-msvc-mt/Release/core --package
python -B tools/build.py cocos --core-prefix out/install/win-x64-msvc-mt/Release/core --package
python -B tools/build.py unity --core-prefix out/install/win-x64-msvc-mt/Release/core --package
```

| 产品 | 外部接入入口 | 发布包 |
| --- | --- | --- |
| Core | `find_package(NetcodeCore CONFIG REQUIRED)`，链接 `Netcode::Core` 或含直连的 `Netcode::Direct` | [Core ZIP](out/packages/core-win-x64/current/netcode-core-win-x64.zip) |
| 纯 TCP | 独立 DLL 与 v1 C 头文件 | [传输 ZIP](out/packages/transport-tcp-win-x64/current/netcode-transport-tcp-win-x64.zip) |
| Godot | 将 ZIP 解入项目，使用 `addons/netcode` 与 `NetcodeGame` | [Godot 插件](out/packages/godot-win-x64/current/netcode-godot-win-x64.zip) |
| Cocos2d-x | 定义宿主 `cocos2d` 目标，指定 Core 和 Cocos 的安装前缀，`find_package(NetcodeCocos CONFIG REQUIRED)` 后链接 `Netcode::Cocos` | [Cocos ZIP](out/packages/cocos-win-x64/current/netcode-cocos-win-x64.zip) |
| Unity | Package Manager 中选择从本地 tarball 添加，C# 组件位于 `Netcode` 命名空间 | [Unity UPM 包](out/packages/unity-win-x64/current/netcode-unity-win-x64.tgz) |

默认使用匹配的 Windows x64 Release。可以使用 `--config Debug` 生成独立的调试 SDK。Godot 编译可用 `--godot-cpp` 指向已解压的 godot-cpp 4.5，省略时使用已校验的依赖归档。Cocos 编译可用 `--cocos-root` 指定引擎头文件树，`--physics2d`、`--physics3d` 与宿主开关一致。可选 Steam 传输通过 `core --steam-sdk <SDK路径>` 启用，使用该组件的外部工程另行提供 Steamworks。

开发安装都在 `out/install/win-x64-msvc-mt/<配置>/<产品>/`。平台开发时运行 `python -B tools/build.py godot` 等命令即可联动 Core。已有 Core 或外部 Core 使用 `--core-prefix` 指向解压目录。原生构建树按产品隔离，`current/previous` 保留当前与上一发布包。同机多份 SDK 可通过平台的 build ID 与 Core build ID 识别。SDK 版本描述接口，CRT、位宽和配置决定二进制可用性，房间握手另行检查联网协议版本。

外部消费与迁移结果见[验收记录](docs/sdk-validation.md)。游戏源码、启动、验收和游戏包见[示例仓库](../examples/README.md)。

## 仓库导航

[架构技术文档](docs/architecture.md)涵盖模块、协议、对象生命周期、RPC、重连和验收。

第二套引擎接入使用 Godot 4 GDExtension。Godot 与 Cocos 样例从同一 C++ 目标获取房间、TCP/UDP、模拟和复制，沿用同一验收脚本。[Godot 构建和操作](../examples/yard/godot/README.md) · [Godot 接入任务](task/godot-platform/总纲.md)

| 路径 | 在这里维护什么 |
| --- | --- |
| [AGENTS.md](AGENTS.md) | 接续任务、判断证据、纠正漂移的约定 |
| [docs/specs/](docs/specs/) | 跨端行为、可观察结果和验收条件 |
| [docs/decisions/](docs/decisions/) | 自研取舍、适用范围，以及新证据到来时需要重审的边界 |
| [Fusion 研究资料](research/fusion/README.md) | Godot Shared Authority 与 Fusion Core 官方文档入口、构建指纹、研究记录和探针 |
| `core/{state,session,transport}/` | 通用核心层：状态和编码、会话调度、TCP 房间与 UDP 游戏通道 |
| [engineplatform/](engineplatform/) | Godot、Unity、Cocos2d-x 适配器，负责组件、节点寿命及引擎属性的采集与应用 |
| [tests](tests/) | 独立核心用例、安装包重定位、双进程房间与存储轮换验证 |
| [tools](tools/) | SDK 构建、打包和本地输出用量管理 |
| [外部示例](../examples/) | 三引擎车场、机关、Shiftline、运行脚本及游戏交付包 |

当前工作区的 Cocos 引擎位于同级目录。Photon 原版基线保存在 [references/photon](../references/photon/)，教学实验位于 [references/experiments](../references/experiments/)。[研究探针](research/fusion/probes/README.md)
