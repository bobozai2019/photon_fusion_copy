# 离线基线与探针入口

从 `netcode/` 根目录运行 PowerShell。需要已导入且能加载 Windows x86-64 原生插件的 Racing 项目、同级教学示例，以及 Godot 4.7.1 控制台程序。命令里给出当前机器的可执行文件路径；换机只需调整 `$godot` 和相应的依赖位置。两项都可离线运行。

```powershell
$godot = 'D:/godot/Godot_v4.7.1-stable/Godot_v4.7.1-stable_win64_console.exe'
$racing = (Resolve-Path '../photon-fusion-godot-starter-kit-racing_3.0.0').Path
$demo = (Resolve-Path '../position-replicator-example').Path
$script = (Resolve-Path '../position-replicator-example/run_racing.gd').Path
```

## 原版 · 刚体布局

```powershell
& $godot --headless --path $racing --script $script -- --sdk
```

入口是 [run_racing.gd 的 verify_sdk](../../../../position-replicator-example/run_racing.gd)。它实例化 Photon 原生类，在测试刚体上逐个加入赛车的三个浮点字段。已观测到 `Photon native AUTO + 0/1/2/3 custom fields -> [15, 16, 17, 18]`，退出码 0。SDK 指纹见[构建记录](../builds/racing-3.0.0.md)。

## 已有教学实现 · 丢包与重传

```powershell
& $godot --headless --path $demo --script res://run_racing.gd
```

该入口使用本地 C++ 教学扩展和进程内模拟链路。已观测到 tick 2 输出 `[0, 400, 12, 75]`，B 在 tick 3 得到 `position.x = 4`、`net_input_x = 0.75`，tick 4 为空，退出码 0。上述整数对属于演示格式。

本次复核使用的 `run_racing.gd` SHA-256 为 `1BF15AAAAAA611479731D530EBBB6019383727829D348A071B3F30478CB25116`。后续探针在此目录按能力命名，并写明它针对原版还是项目实现、输入、观察点和复测命令。

## 原版 · 双端浮点标记

```powershell
python -B research/fusion/probes/shared_float.py --godot $godot --racing $racing
```

该命令使用 Racing 自带配置连接 Fusion，为本次运行生成独立房间名。A 生成原版赛车并确认自身权威，暂停赛车业务脚本后依次写 0.75 和 −0.5。B 保留原场景配置中的关闭插值设置，记录收到的值和本地物理帧。标记文件由启动器在 B 入房时写入，双端时钟分别保留。

2026-11-20，原配置在 A 的区域查询收到 `32767 ApplicationNotFound`，入口退出 1。尚未进入房间或采集标记。离线布局命令和自研双端命令均独立通过。
