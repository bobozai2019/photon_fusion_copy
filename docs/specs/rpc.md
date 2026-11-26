# RPC · 方法、路由与投递

Godot 可通过 `@rpc` 和本局 `NetcodeRoom` 调用对象方法或房间方法。[声明式 RPC 语法与路由](godot-scene-sync.md#声明和调用-rpc)


## 命名方法与参数契约

两端注册相同的方法名、稳定方法号、参数模板、方向、可靠性、作用域和写者限制。调用方传入方法名与参数，接收方核对签名、解码参数后调用引擎回调。模板号 0 表示零参数。注册时按参数模板最大容量检查，每次调用可用的参数编码预算为 252 字节。[会话实现](../../core/session/script_game.cpp)

在开局前注册方法。名称的 UTF-8 编码长度为 1–96 字节，方法号为 1–65535，名称和方法号各自唯一。房间作用域接受 `Server`、`Client` 和 `NetMulticast`。`StateWriter` 只适用于对象作用域的 `Server`、`Client` 或 `Remote`。该选项还决定投递方向：客人发往房主的对象调用校验当前写者，房主发往客人的调用按当前写者选接收方。接收网络调用时还会核对授权代次。房主本地执行 `Server`，以及客人本地执行 `Client`，遵循下表的本地执行规则。[注册约束与路由](../../core/session/rpc.cpp) · [签名规则](../../core/session/script_game.cpp)

控制台的 `Report` 是客人上行时要求当前写权的对象方法，参数为整数、最多 16 个 UTF-8 字节的字符串和二维向量。房主核对发送者、对象和参数后，向该玩家发送零参数房间方法 `Done`。可靠层回执用于判断接收进度，玩法用 `Done` 表示处理完成。Godot 的 Callable、Unity 的委托和 Cocos2d-x 的类型化回调均在本机双进程验收中实际执行。[参数模板](../../../examples/puzzle/components.cfg) · [验收断言](../../task/gameplay-components/验收/本机.md)

## 对象 RPC 如何路由

对象方法可以按业务所属玩家或当前状态写者校验。下表描述业务归属路由；登记为仅写者的方法会按当前授权关系检查调用者，因此客人接管控制台前后请求同一个方法会得到不同结果。方向设计参考 UE 的执行矩阵。[路由实现](../../core/session/rpc.cpp) · [Epic RPC 执行矩阵](https://dev.epicgames.com/documentation/en-us/unreal-engine/remote-procedure-calls-in-unreal-engine)

| 方法方向 | 房主调用 | 客人 B 调用 |
| --- | --- | --- |
| Server | 房主本地执行 | 在 B 自己连接所属的对象上调用，到房主执行；在其他玩家、房主或无所属连接的对象上调用，丢弃 |
| Client | 有所属客人时在该客人执行；房主所属或无所属连接时在房主执行 | 在 B 本地执行，目标对象属于 C 也不会送到 C |
| NetMulticast | 房主本地和该对象对其可见的客人执行 | 在 B 本地执行 |
| Remote（UE 文档另列） | 有所属客人时在该客人执行，其他情况丢弃 | 在 B 所属对象上调用，到房主执行，其他情况丢弃。该方向用于对象方法 |

**开门验收场景**：公共门归房主管理。B 在自己的玩家对象上调用 `ServerTryOpen(doorId)`；房主检查调用者和门的状态。成功时房主写 `door.is_open`；拒绝时可以在 B 的玩家对象上调 `ClientOpenDenied(reason)`。用 B 在公共门上直接调用 Server RPC 作反例，检查房主没有执行。客人 C 在开局前加入，可用于检验 B 本地调用组播不会让 C 执行。

## 房间级接收器与投递保证

- 房间级接收器使用对象号 0。房主可处理合法的房间请求，按方法方向向指定成员单发或广播。客户端上行身份取自实际连接。
- 同一会话中，可靠方法的验收覆盖丢包重传、重复包去重，以及同一发送端到同一接收端在同一网络对象上的调用顺序。跨对象、混合可靠性及属性与 RPC 的先后关系要分别定义。UE 也分别描述同一 Actor（含其复制子对象）和不同 Actor 的顺序边界。[Epic 复制执行顺序](https://dev.epicgames.com/documentation/en-us/unreal-engine/replicated-object-execution-order-in-unreal-engine)
- 对象终结时释放经典可靠 RPC 在该对象上的发送与接收流，未确认调用以 `ObjectGone` 结束。兴趣隐藏期间保留流的序号，重新进入视野后继续使用。
- 尽力投递的方法允许丢失和乱序。传输确认表示协议投递进展。房主是否批准某个业务动作，通过业务回应或权威属性观察。
