# Fusion 研究资料

## 本地原版拆解进度

Preview 555 的 47 个随包脚本、295 个 Godot 注册方法以及 Linux 编辑器库两个命名空间的 360 个具名函数起点均有逐项索引。原版 Web 双端已验证属性采集、节点重绑、RPC、兴趣、权威交接、输入重演、刚体 Forecast、地图增卸与 Master 更替。Windows 原版另测编辑器导出与类型布局。架构差距、平台、构建及实验边界见[客户端拆解与项目差距](findings/client-audit.md) · [脚本逐文件清单](findings/sample-scripts.md) · [原生关键分支](findings/native-runtime.md) · [探针命令](probes/README.md)。

## 官方文档入口

- 研究 Godot 端的对象权威、Replicator 配置、属性同步和 RPC 时，从 Shared Authority 版手册进入相应章节。[Fusion Godot 3 · Shared Authority](https://doc.photonengine.com/fusion-godot/v3-shared-authority/fusion-intro)
- 研究跨引擎 C++ 核心的状态复制、RPC 和引擎绑定职责时，从 Core 手册进入相应章节。[Fusion Core 3](https://doc.photonengine.com/fusion-core/v3/fusion-core-intro)

记录结论时引用支持该主张的具体章节，并标注“官方文档”。涉及本地 Racing 插件的内存偏移或调用频率时，另附对应构建的静态证据或运行观测。[参考构建指纹](builds/racing-3.0.0.md) · [属性绑定证据](findings/property-binding.md) · [探针复现说明](probes/README.md)
