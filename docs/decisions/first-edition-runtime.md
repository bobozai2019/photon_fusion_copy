# 首版运行时契约

本文保留早期车场 Direct 后端和首版字段协议的约定。新游戏的默认传输见 TCP 接入，当前状态布局和编码见属性同步规格。[TCP 接入](../../README.md#新游戏使用纯-tcp) · [状态布局与差量](../specs/property-binding.md#状态布局和差量)

## 连接、房间和恢复

Windows 后端在同一个可配置 IPv4 端口开 TCP 和 UDP。TCP 提供长度帧、房间操作及 250 ms 心跳，默认失联阈值为 3 秒。UDP 报文最多 1196 字节，其中最多 1176 字节为游戏层负载。房主通过 TCP 为每条连接发放随机值，确认同一 IPv4 的 UDP 端点后才向房间层报告已连接。重新连接生成新句柄和随机值。游戏包还携带房间与局次，旧端点和旧局数据无法进入新会话。

房间、玩家、局次和连接分别管理。开局锁定最多 4 个席位并发放本局恢复凭证。断线时清零旧输入，保留身份和原车并继续模拟惯性和碰撞。重连校验房间、局次、身份、凭证和在线状态。客人从空视图应用当前基线，房主收到确认后开放输入。正常结束一局清除当前基线、RPC 流和恢复资格，并释放离线席位。

Fusion Core 的兴趣区进入也发送完整对象状态，适用范围是原版兴趣区进入事件。项目采用当前世界基线。另一种恢复做法是持久化断线状态并重放差量，离线期间销毁和再创建的回归能区分重放遗漏与重新基线的结果。[房间规格](../specs/room-rejoin.md) · [Fusion Core 兴趣区](https://doc.photonengine.com/fusion-core/v3/manual/interest-area)

## 输入、采集与复制

房主物理目标为 30 Hz，网络发送为 20 Hz，Cocos 显示为 60 Hz。输入通过实际连接关联玩家，带房间、局次、递增序号和两个有限的归一化浮点数。客人持续发最新控制量，连续 8 个模拟步没有更新时房主清零。一次性动作使用可靠 RPC。Fusion Preview 555 的预测输入逐条保存序号与发送计数，每条尚在队列中的输入最多三次进入待发缓冲，接收端过滤旧序号。该版车场使用最新控制量和失效期限。后续车场按步确认输入，恢复检查点并重演未确认输入。[现行预测契约](../specs/physics-prediction.md#已实现的房主预测) · [原生输入链路](../../research/fusion/findings/native-runtime.md) · [Fusion Core Ownership](https://doc.photonengine.com/fusion-core/v3/manual/ownership)

schema 定义模板 ID、版本和严格递增的字段 ID。每个 schema 至多 64 字段。浮点为小端 IEEE 754，布尔为单字节。线上记录带字段清单指纹和位掩码。采集拷贝值并更新对象与字段版本，发送按每条连接的对象确认进度选取字段。最终变化和销毁在确认前继续发送。客人按对象版本拒绝旧状态，重复包仍可确认，完整数据报解析成功后才提交。

Cocos 层按路径解析根或子节点。业务字段直接应用，变换显示插值窗口为 100 ms，旋转取最短角路径。Fusion 原版的 Windows AUTO 刚体布局实测为 15 words，每添加一个赛车浮点字段增加一个 word。其公开帧循环区分接收、逻辑和发送。Godot 预览包可能每物理步采集，也可能只在发送前采集，可分别改变两类频率并观察计数 getter 来区分。[本地布局证据](../../research/fusion/findings/property-binding.md) · [Fusion Core Frame Loop](https://doc.photonengine.com/fusion-core/v3/manual/frame-loop)

## RPC

对象方法可用 Server、Client、NetMulticast 和 Remote。房间方法可用 Server、指定成员 Client 或房主广播。来源身份取实际连接。可靠方法按发送端、接收端和房间或对象独立排序，单流最多 64 条在途，采用累计确认，每 100 ms 重试。尽力投递方法独立传输。属性应用和不同 RPC 流允许交错。连接结束或跨局时清除待投递数据。

门的状态写入权威属性，业务拒绝使用独立 Client 方法。路障的出生和死亡由状态复制交付。若仅通过 RPC 发动作，断线期间生成和删除后从当前世界恢复的结果会有区别。[UE 调用方向](https://dev.epicgames.com/documentation/en-us/unreal-engine/remote-procedure-calls-in-unreal-engine) · [Fusion Notify 通道](https://doc.photonengine.com/fusion-core/v3/reference/notify-api)
