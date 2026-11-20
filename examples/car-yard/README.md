# Windows x64 联机车场

解压后在包内打开两个 PowerShell 窗口，依次运行下列命令。游戏窗口与各自的控制台同时打开。端口可修改，两端使用同一个端口。

```powershell
.\netcode-yard.exe --host --port 43210
```

```powershell
.\netcode-yard.exe --join 127.0.0.1 --port 43210
```

房主等待人数显示 `2/4 seats`，在 Host 窗口按 F2。各窗口中绿车是本地控制的车辆。点击需要操作的窗口，使用方向键或 WASD 驾驶。靠近闸门按 E 请求开门。按 B 在本车前放置路障，按 X 移除自己的路障，按 H 鸣笛。

客人在局中按 F4 断开。房主可继续驾驶和修改路障。客人在原窗口按 R，基线应用完成后继续操控原车。房主按 F3 回到同一房间的等待阶段，按 F2 再开一局，按 Q 结束房间。

同机负载模式给两个车场命令都追加 `--scenario stress --seconds 10`，再启动两个轻量客人。四个进程使用相同的端口和时长。三位客人就绪后自动驾驶，房主冻结最终世界后各端输出摘要并退出。给各命令追加 `--fault` 可按固定规则注入 UDP 丢失、重复和乱序。

```powershell
.\netcode-guest.exe --join 127.0.0.1 --port 43210 --scenario stress --seconds 10
```

默认监听回环地址。局域网配置时房主传入 `--bind` 和本机 IPv4，客人用该地址替换回环地址。Windows 跨设备联机待实测。
