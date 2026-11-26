# Netcode Core 与平台审核结项

2026-11-24 已完成。使用 `python -B tools/build.py all --package` 重建 Windows x64 Release 的 Core、TCP 和三个平台包，并更新示例依赖锁。构建和外部消费命令见 [SDK 验收入口](../sdk-validation.md#复现)。

| SDK | 构建标识 | Windows x64 Release 验证 |
| --- | --- | --- |
| Core | `7c86097d5ebf426f` | CTest 9/9。覆盖依赖销毁后的检查点、房间属性与生成序号恢复、对象 RPC 清理和入房超时。重定位消费通过，错误 CRT 被拒绝 |
| TCP | `da48c35a5903ea05` | v1 C ABI 双客端、分段与合并读取、队列重试及重连通过。Godot 与 Unity 开房入口拒绝 65 席 |
| Godot | `07323cf307cecc3f` | 4.6.1 和 4.7.1 原生节点全套通过。空间兴趣覆盖 82 格超限、移除观察区及恢复订阅。4.7.1 原生导出与外部工程双端字段和 RPC 均通过 |
| Cocos | `4af80b708c71ecbb` | 重定位解压后使用匹配 Core 编译。双端分数 42、八个字段和 RPC 通过。回调中移除会话组件及属性 getter 中移除复制组件均通过。配错旧 Core 时配置失败 |
| Unity | `d155c48545cdd908` | 6000.4.0f1 新工程导出双端，分数 42、八个字段和 RPC 通过。验证错误字段号、类型错误和首个快照前登记错误绑定 |

关闭 Direct 后 Core 标识为 `14eeaf5c59d5e4b7`，关闭传输宿主后为 `61599d4a4495db15`。CMake 同时追踪 Direct、传输宿主和传输 API 源文件的变化。公网、Steam 与非 Windows 平台沿用各自已有的验收边界。

三个平台的通用会话分别通过 TCP 和命名管道模块的双进程验收。两轮均核对八个字段、双向 RPC、凭原票据接回同一玩家，以及缺失 DLL 和错误 ABI 的诊断。[传输消费验证](../../tests/transport/verify_players.py)

sync-lab 用本轮 Godot ZIP 运行双端。128 对象的可见数依次为 128、76、76、128，512 对象依次为 512、268、268、512。两档各端退出码均为 0，客人分别持续收到 127 和 511 个远端字段组。[用例与命令](../../../examples/sync-lab/README.md)

## 归档校验值

| 产品 | SHA-256 |
| --- | --- |
| core | `e2e75a95262718688d7e2f2b61a7e6e4c72a4f7376635c3429875341e9b0f38c` |
| transport-tcp | `67a5bf712ec56518a1149e7521b56d76311c28f8cd0357878350a89867f42194` |
| godot | `360a8393bbfe995746111a329de98f649192e4780275de107ae26b69ca357705` |
| cocos | `0949f9f3ef9a8f835e7c578d6d48a16801aba20f10675f54c8978977574cfeba` |
| unity | `ecedff3ab29af0006ceeb4effb8d4a4cc1e2e156de724afe867e910f802c7d7d` |

## 收尾

表中记录的是清理前的验收结果。本轮专用用例和仓库内临时产物已清理。现有 Godot 编辑器夹具保留等待目标场景和面板就绪的修正。清理后复跑原有 Core CTest，9/9 通过。

### 本机清理记录

本轮 Godot SDK 消费验证留下一个约 104 MiB 的导出文件，路径为 `C:/Users/18205/AppData/Local/Temp/netcode-consumer-godot-q6dc_526/player/Consumer.exe`。拟删除该文件及随后为空的 `player` 与 `netcode-consumer-godot-q6dc_526` 目录。自动审批返回 `blocked by policy`，文件仍在。
