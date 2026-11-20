# Godot 4 联机车场

解压 Windows x64 包后，在包目录启动房主和客人。两端用同一端口。

```powershell
.\netcode-godot.exe -- --host --port 43240
.\netcode-godot.exe -- --join 127.0.0.1 --port 43240
```

在房主窗口等待 `2/4 seats` 后按 F2。两个窗口各自的绿车为本地玩家。方向键或 WASD 驾驶，E 请求开闸门，B 放路障，X 移除路障，H 鸣笛。客人按 F4 断线，按 R 回原局、接回原车。房主按 F3 结束本局并返回房间，按 Q 结束房间。房主可用 `--bind` 指定 IPv4 监听地址。

车场、复制节点和输入界面运行在 Godot。房间、模拟、状态复制、RPC 与 Winsock 直连来自两个引擎共用的 C++ 目标。

包内四人测试在两个 Godot 命令的用户参数中追加 `--scenario stress --seconds 10`，再启动两次下列命令。四端可同时追加 `--fault`，自动验证 UDP 丢包、重复和乱序后的收敛。

```powershell
.\netcode-guest.exe --join 127.0.0.1 --port 43240 --scenario stress --seconds 10
```

## 从仓库构建和验收

需要 Visual Studio 2026 的 x64 C++ 工具、Python 3.11+ 及 Godot 4.7.1 Windows 版。构建脚本校验并提取固定版本的 godot-cpp。可用 `--godot-exe` 或 `NETCODE_GODOT_EXE` 指定 Godot 图形程序。

```powershell
python -B tools/build.py --godot
python -B tools/verify_yard.py play --platform godot --port 43240 --capture
python -B tools/verify_yard.py stress --platform godot --port 43241 --seconds 10
python -B tools/verify_yard.py stress --platform godot --port 43242 --seconds 10 --fault
```

两种引擎可以用 `python -B tools/build.py --godot --cocos` 一次构建。`verify_yard.py` 的 `--platform` 选房主，`--guest-platform` 可单独选客人引擎。轻量客人使用共用的 `netcode-guest.exe`。

开发启动时，把 `--path out/build/win-x64/Release/bin/Release/godot` 放在 Godot 的 `--` 之前。打包使用 `python -B tools/package_godot.py --verify-play`，需要安装同版 Windows x64 导出模板。ZIP、依赖和运行日志写到本地 `out/`。
