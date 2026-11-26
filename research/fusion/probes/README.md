# Fusion 客户端探针与复现命令

## 纯属性数量与变更比例

在 `netcode` 仓根目录运行。从头执行历史矩阵需将 `$oldZip`、`$candidateZip` 分别设为与[构建指纹](../../../task/godot-platform/验收/属性同步性能.md#构建与运行环境)相符的 Windows Godot SDK ZIP 路径。`$matrixJsonl` 指向该轮完成后输出的 `property-matrix.jsonl`。设置 `FUSION_PROBE_APP_ID` 后，以下命令轮换旧版、候选版和 Fusion 的运行次序，每格每版本启动五个独立房间。省略 `--widths 20` 可同时测量固定 250 对象的 4／12 字段布局。矩阵将每次尝试写入 `out/runs/current/property-matrix.jsonl`，结束后按运行结果轮换到 `last-pass` 或 `last-fail`。发布门槛由 `property_verdict.py` 判定。

```powershell
python -B research/fusion/probes/property_matrix.py --old $oldZip --candidate $candidateZip --fusion --widths 20
python -B research/fusion/probes/property_verdict.py $matrixJsonl
python -B research/fusion/probes/property_contract.py $candidateZip
python -B research/fusion/probes/property_scale.py native 1000 --sdk-zip $oldZip --workload sparse --omit-wave 12
```

只复算已归档的 15 个主矩阵用例时，可直接读取随仓保存的 JSONL。本轮以两个命令分别复算原主矩阵和替换整数候选后的矩阵，结果均为 15/15 通过。

```powershell
$evidence = 'task/godot-platform/验收/属性同步性能'
python -B research/fusion/probes/property_verdict.py "$evidence/主矩阵逐场.jsonl"
python -B research/fusion/probes/property_verdict.py "$evidence/修复整数逐场.jsonl" --reference "$evidence/主矩阵逐场.jsonl"
```

固定先前参考时，只运行候选并在计分时指定原矩阵。每条新记录都带独立房间标识。

```powershell
python -B research/fusion/probes/property_matrix.py --candidate $candidateZip --widths 20
python -B research/fusion/probes/property_verdict.py $candidateJsonl --reference $referenceJsonl
```

写权与八人混合负载从同一候选 ZIP 安装。以下试验通过本机 TCP 中转统计每连接的帧字节，延迟为每方向的基值，另有 0～16 ms 抖动。权威试验保留客人已经发出的旧代更新，在房主收权后投递。混合试验在第 30 轮切换 AOI，停写后恢复全部可见并逐字段查验。

```powershell
python -B research/fusion/probes/property_multiplayer.py $candidateZip authority --one-way-ms 15
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 128
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 128 --one-way-ms 40
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 512
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 512 --one-way-ms 40
```

原版 Fusion 555 与自研可用共同的八字段布局复跑。两边都使用 PackedInt32Array 和 PackedFloat32Array，保留多写端和 AOI 切换。原版跑 Photon Cloud Asia 的 Chrome 客户端，自研跑本机原生 Godot 和 TCP 中转。设置 `FUSION_PROBE_APP_ID` 后再运行原版。采集分项用自研完整混合布局单独测量。

```powershell
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 512 --fusion
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 512 --portable
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 512 --profile
```

下列命令只在业务写入期间限制客人 B 的 TCP 下行。该连接用小接收缓冲把拥塞传回房主，其余客人在恢复带宽前必须完成第 30 轮的 AOI 切换。恢复后核对 B 完成切换，以及八端的全部终值和版本顺序。限带宽组固定使用 65,538 字节 TCP 模块队列，运行前后使用同一参数比较。

```powershell
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 512 --slow-down-kib 8
python -B research/fusion/probes/property_multiplayer.py $candidateZip mixed --count 512 --slow-down-kib 8 --one-way-ms 40
```

最后一条应拒收只有 59 轮和 5,900 次写入的场次。观察端先开始记录固定步，两端使用写端公布的起止时间计分。属性版本按毫秒汇总，计入停写前结束的完整毫秒，并单列停写所在毫秒应用的版本数。写端计数来自实际执行的业务赋值。正常压测不打开 profiler。

对同一 5,000 字段负载采集两端 profile 时，在 netcode 根目录串行运行以下命令。自研记录分段墙钟和进程 CPU。Fusion 使用同一环境变量 `FUSION_PROBE_APP_ID`，记录原版 Web 双端的 CPU 栈、游戏时间窗口和渲染进程 CPU。仅计数和计时插入临时副本，对照额外替换根节点整数的采集路径。每行 `RESULT` 给出双方终值和分角色的详细读数。[架构对照和 profile](../findings/property-profile.md)

```powershell
foreach ($mode in 'idle','sparse','dense') {
    python -B research/fusion/probes/property_profile.py native --workload $mode
    python -B research/fusion/probes/property_profile.py fusion --workload $mode
}
foreach ($mode in 'idle','sparse') {
    python -B research/fusion/probes/property_profile.py native --sdk-zip $oldZip --workload $mode --cached-scalar
    python -B research/fusion/probes/property_profile.py native --sdk-zip $oldZip --workload $mode --packed-scalar
}
```

从 netcode 仓库运行。自研使用本机 Release 安装前缀中的 Godot 插件和两个 headless 进程。Fusion 使用原版 Preview 555 Web 模板、Chrome 双客户端和 Photon Cloud Asia，读取环境变量 `FUSION_PROBE_APP_ID`。每房一个玩家写、一个玩家观察；相邻命令串行运行以减少 CPU 争用。

```powershell
foreach ($backend in 'native','fusion') {
    python -B research/fusion/probes/property_scale.py $backend 1000 2000 3000 4000 5000
    python -B research/fusion/probes/property_scale.py $backend 1000 3000 5000 --workload sparse
    python -B research/fusion/probes/property_scale.py $backend 1000 5000 --workload idle
    python -B research/fusion/probes/property_scale.py $backend 1000 --fields-per-object 4
    python -B research/fusion/probes/property_scale.py $backend 3000 --fields-per-object 12
}
```

每个字段均为独立声明、独立配置的 Godot 64 位 `int`。测试值落在有符号 32 位范围，自研采集值为 8 字节，Fusion 对应两个 32 位 word。每组使用普通 Node3D，关掉根节点复制，场景没有刚体或碰撞形状。默认每对象 20 个字段；改成 4 或 12 个可在 250 个对象内改变总属性数。双端先逐字段对齐有唯一身份的初值，再按墙钟每 100 ms 写一轮，连续 60 轮，单次回调最多执行一轮。默认每轮写全部，`sparse` 每轮轮换修改十分之一，`idle` 保持基线 6 秒。每个写入值混合属性身份和版本，远端实际 setter 去重、校验版本和身份，停写后逐字段读取最终值。结束通知只用于限定等待期限，全部数值经正式场景同步通道传输。[实测结果](../findings/property-scale.md) · [场景与统计生成](property_scale.py) · [计时与采样](property_scale.gd)

从 `netcode/` 根目录运行 PowerShell。Windows 离线基线使用已导入的 Racing 项目、同级教学示例和 Godot 4.7.1 控制台程序。Web 多人实验另用随包引擎模板和 Chromium。下列路径取自本机。

```powershell
$godot = 'D:/godot/Godot_v4.7.1-stable/Godot_v4.7.1-stable_win64_console.exe'
$racing = (Resolve-Path '../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0').Path
$demo = (Resolve-Path '../references/experiments/position-replicator-example').Path
$script = (Resolve-Path '../references/experiments/position-replicator-example/run_racing.gd').Path
```

## 原版 · 刚体布局

```powershell
& $godot --headless --path $racing --script $script -- --sdk
```

入口是 [run_racing.gd 的 verify_sdk](../../../../references/experiments/position-replicator-example/run_racing.gd)。它实例化 Photon 原生类，在测试刚体上逐个加入赛车的三个浮点字段。已观测到 `Photon native AUTO + 0/1/2/3 custom fields -> [15, 16, 17, 18]`，退出码 0。SDK 指纹见[构建记录](../builds/racing-3.0.0.md)。

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

同日后续两轮复测中，A 依次报告 `Connection error: 1043 (code: -1)`、`Connection error: 1040 (code: -1)`。两轮均在 A 入房前结束，A 退出 3，启动器退出 1。A 入房后启动器才会启动 B。以上输出记录的是三次独立结果。

Windows 入口也接受进程环境变量 `FUSION_PROBE_APP_ID` 和 `FUSION_PROBE_REGION`，未指定 ID 时沿用样例配置。已验证的测试 ID 在本机 Windows 选择 EU 和 Asia 均于查询区域时出现 `1040`。同一 ID 的原版 Web 客户端用 WSS 进入 Asia 房间。以下是成功入房的复现方式。

## 原版 Web · Shared、玩家 Host 和会话

需要 Python 的 `playwright` 包、一个已安装的 Chromium 系浏览器、相邻 Racing 中的 Web debug 模板和 WASM 扩展，以及可用的 Fusion 3 App ID。本机使用 Google Chrome。工具校验两个原版文件的 SHA-256，为每次运行生成独立房间名，仅在回环 HTTP 地址提供引擎和测试场景。各参加者使用独立的浏览器上下文。测试 ID 从进程环境传入页面，项目文件中无需填写。[Photon 后台创建和 Godot 配置](https://doc.photonengine.com/fusion-godot/v3-shared-authority/getting-started/quick-start-guide)

```powershell
$env:FUSION_PROBE_APP_ID = Read-Host 'Fusion 3 App ID'
python -B research/fusion/probes/cloud_web.py --mode shared --region asia
python -B research/fusion/probes/cloud_web.py --mode host --region asia
python -B research/fusion/probes/cloud_web.py --mode session --region asia
Remove-Item Env:FUSION_PROBE_APP_ID
```

`--browser` 指定浏览器可执行文件，`--addon` 指定已核对构建的插件目录。Shared 检查晚加入、字段值及稳定计数、节点替换、RPC、兴趣键、写权拒绝/转移和销毁。Host 由玩家 A 在同一普通客户端担任主机，核对 60 个独特输入、重复处理和重置计数。Session 测刚体、地图、Master 更替、普通重新加入和直接关闭浏览器。运行输出按观察端记录玩家号和本地物理帧。两端的帧号属于各自的时钟。[完整结果与适用条件](../findings/client-audit.md)

## 2026-11-22 三维刚体联机对照

Fusion 用上一节的 `cloud_web.py --mode session --region asia`，两个独立浏览器客户端连接 Photon Cloud。自研用 Godot 4.7.1 两个 Windows 进程、回环直连和实际 NetcodeReplicator。两边均使用半径 0.25 m 的球、中心位于 x=3 m 且厚 0.25 m 的静态墙、零重力、零线性阻尼。持权端以 3 m/s 发球，然后传送至 x=25 m，以 0.5 m/s 唤醒并最终休眠。传送阈值设为 10 m、90°。自研分别保留默认速度校正和显式改用 Blend。

| 本轮观测 | Fusion Shared / Blend | 自研 / Velocity | 自研 / Blend |
|---|---:|---:|---:|
| 代理运动触发时的 x 速度（m/s） | 3.0000 | 3.8270 | 3.0000 |
| 持权端观察墙接触时 x（m） | 2.63512 | 2.63800 | 2.63800 |
| 代理观察墙接触时 x（m） | 2.63703 | 2.64060 | 2.63040 |
| 代理观察传送后 x（m） | 25.000 | 25.000 | 25.000 |
| 代理观察唤醒时的 x 速度（m/s） | 0.5000 | 0.6110 | 0.5000 |
| 代理最终休眠且速度为零 | 是 | 是 | 是 |

Fusion 的 Web 双端、原生 Windows 参数探针和两轮自研双端均退出 0。各行取各端满足事件条件的本地样本。两端时钟和采样时机不同，表中的位置差不能当作同 tick 误差。实验只涉及一个活动刚体与静态墙。Fusion Windows Racing 原车读取到 PlayerAttached、Auto、Forecast、Blend、0.25 s，重力外推关闭；此项离线测量用下一节命令复核。

自研复现时，从两个终端运行同一个[刚体探针](netcode_body_comparison.gd)。`$project` 指向已导入且含原生插件的 Godot 工程，`$probe` 为脚本绝对路径，`$trial` 为两端共享、每轮单独使用的空临时目录。先启动房主，再启动客人。将两条命令末尾同时改成 `blend` 即可测显式位置混合校正。

```powershell
& $godot --headless --path $project --script $probe -- host 43658 $trial default
& $godot --headless --path $project --script $probe -- guest 43658 $trial default
```

两端分别运行本地 Godot 物理。临时目录里的标记只用于等对方确实观察到某个阶段，再发起下一次操作。刚体状态走实际网络连接。

### 活动刚体数量与成对碰撞

从 netcode 仓库运行。Fusion 命令读取环境变量 `FUSION_PROBE_APP_ID`，默认选用工作区 Racing 3.0.0 的 Preview 555 插件、Chrome 和 Asia 房间。原版 Web 模板和插件在加载前均校验指纹。自研命令使用车场样例实际安装的插件和两个本机 Godot 进程。各档创建新房间。逐条运行可以避免各组争用同一台机器。

```powershell
python -B research/fusion/probes/rigid_scale.py native 10 100 300 1000
python -B research/fusion/probes/rigid_scale.py native 10 100 300 1000 --workload pairs
python -B research/fusion/probes/rigid_scale.py fusion 1 10 100 300
python -B research/fusion/probes/rigid_scale.py fusion 10 100 300 1000 --workload pairs
```

每轮由一个玩家持续驱动全部刚体 6 秒，另一个玩家每个物理步采样。默认各球分道，无碰撞。`pairs` 把两个动态球放进同一条道，相邻两道间隔 2 m，统计两端实际发生过接触的刚体数。球半径 0.25 m，重力与阻尼为零，每 18 个物理步反转驱动方向，每 6 步更新一次整型时间标记。结束时传送到指定位置并清零速度，半秒后要求睡眠。观察端在收到第一个结束标记后最多等 8 秒，逐体核对结束标记、x 误差小于 5 cm 和睡眠状态。

输出中的 `hz` 是真实时间内推进的物理步数。`stamp_hz` 是观察端看到的不同活动标记数除以总刚体数和观测时间。`worst_age_p95_ms` 是每个采样步取所有对象中最旧标记的年龄，再对这些样本取 P95。首尾从首次活动标记到首次结束标记；缺少活动标记的步单独计数。年龄包含持权端两次写入的间隔。双端共用同一台计算机的系统时钟。运行退出表示完成采样，收敛情况读取 `all_three`。[测量脚本](rigid_scale.gd) · [运行器](rigid_scale.py) · [实测记录](../findings/rigid-body-scale.md)

## 原版 · 编辑器类、类型布局与 getter 对照

沿用页首的 `$godot` 和 `$racing`。探针直接加载 Racing 的 Windows 编辑器扩展，以当前项目的浮点压缩配置 `0` 运行。

```powershell
$layoutProbe = (Resolve-Path 'research/fusion/probes/native_layout.gd').Path
& $godot --headless --path $racing --script $layoutProbe
```

2026-11-20 退出 0。探针检查已注册的 21 个 `Fusion*` 类、七种根节点的 None/AUTO 布局、六种单字段、字符串长度和三组浮点数组容量。显式计算五次布局时，关闭/启用浮点字段得到 0/5 次 getter 调用，随后等待 30 个离线物理帧得到 0 次。实例化原车与全新 SharedReplicator 后，两者的校正模式都为 2，ClassDB 将其注册为 `CORRECTION_BLEND`。原车最大外推时间为 0.25 秒。[逐项结果与适用条件](../findings/native-runtime.md)

## 原版 · 已命名函数和原生调用点

```powershell
$llvm = 'D:/godot/emsdk/upstream/bin'
python -B research/fusion/probes/native_evidence.py --llvm-bin $llvm
python -B research/fusion/probes/native_evidence.py --llvm-bin $llvm --inventory
```

默认从工作区 `references/photon/photon-fusion-godot-starter-kit-racing_3.0.0` 读取 Linux ELF，`--sdk` 可指定同一构建的其他副本。脚本先验证 SHA-256，再将选定调用和尾跳的目标经 `.plt.sec` 和 `R_X86_64_JUMP_SLOT` 解析为函数名。2026-11-20 复核了 35/35 个位置，退出 0。`--inventory` 同时列出 221 个 `FusionCore::` 具名文本符号条目。查看指令条件、偏移及常量时使用原函数反汇编，例如复核每条输入的计数检查、递增和网络时钟分段。

```powershell
$elf = Join-Path $racing 'addons/fusion/bin/libfusion.linux.editor.x86_64.release.so'
& "$llvm/llvm-objdump.exe" -d --no-show-raw-insn --start-address=0x459090 --stop-address=0x4591ff $elf
& "$llvm/llvm-objdump.exe" -d --no-show-raw-insn --start-address=0x459470 --stop-address=0x459500 $elf
```

Forecast 的入口属于 Godot 层的非导出方法。下面的重定位依次关联虚表中的类型信息、处理入口和类型名。三个反汇编区间可分别检查外推上限、3D 校正模式分派和 Blend 对速度的属性写入。Force 的两个方法名引用位于 `0x15988f / 0x159b1d`。[对应分支解释](../findings/native-runtime.md)

```powershell
& "$llvm/llvm-objdump.exe" -R $elf | Select-String '00000000006bfc18|00000000006bfc30|00000000006c4660'
& "$llvm/llvm-objdump.exe" -d --no-show-raw-insn --start-address=0x15ac40 --stop-address=0x15ac60 $elf
& "$llvm/llvm-objdump.exe" -d --no-show-raw-insn --start-address=0x1593c4 --stop-address=0x1593e3 $elf
& "$llvm/llvm-objdump.exe" -d --no-show-raw-insn --start-address=0x1595e0 --stop-address=0x159713 $elf
```

## 原版 · 全部注册方法和两个原生命名空间

```powershell
python -B research/fusion/probes/native_index.py --llvm-bin $llvm --godot $godot --racing $racing
```

程序先校验原版 Windows DLL 与 Linux ELF 指纹，用原版 Godot 导出运行时类、方法签名、属性、信号与枚举，再将所有 295 个方法名与 Linux 方法绑定地址核对。最后重新解码 Core 和 Realtime 的每个具名函数，与仓库索引逐行比对。预期为 21 个类，Core 210 个起点和 21,308 条指令，Realtime 150 个起点和 22,638 条指令。默认只读，`--write-index` 可重新生成类型和两张函数表。[方法地址](../findings/godot-bindings.tsv) · [类型和签名](../findings/godot-types.tsv) · [Core](../findings/core-functions.tsv) · [Realtime](../findings/realtime-functions.tsv)

## 原版 Windows · 编辑器导出、绑定失效与单机房

```powershell
python -B research/fusion/probes/editor_fixture.py --godot $godot --racing $racing
$preview684 = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Downloads/Photon-Fusion-Godot4.6-3.0.0-Preview-684.7z'
python -B research/fusion/probes/editor_fixture.py --godot $godot --preview-684 $preview684
```

第一条命令用 555 的原版编辑器插件生成 JSON，对照 None/AUTO 两套字段偏移、运行时占用和配置复制，还检查 C# 导出报告缺少配套 CLI 的阶段。第二条使用独立指纹的 684 和 7-Zip 检查单机房完整生命周期。临时 Godot 项目由运行器在 `out/tmp/` 隔离创建并回收。[导出布局和单机观测](../findings/client-audit.md)

Preview 684 的单机房探针还会创建一个受本地玩家控制的对象，在 600 个物理帧写入递增整数、80 字节字符串和 40 项整数数组。输入和最终状态摘要与 [Core 编码负载](../../../tests/core/codec_workload.cpp) 相同。终端分别输出运行时布局字数、标量 getter 调用次数、shadow 标志及有无远端。两端都打印 1124070639 才能核对最终数据。单机房无法提供远端确认、压缩消息字节或独立的编码 CPU 样本。
