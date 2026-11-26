# Preview 555 随包源码逐文件对照

范围为 Racing 目录的 5 个 GDScript、Starter ZIP 的 25 个 GDScript 及两包共用的 17 个 C# 文件。表内的职责来自源码及场景引用。网络运行结果另用原版扩展和受控场景取得。[Racing](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0) · [Starter 原包](../../../../references/photon/distributions/photon-fusion-godot-starter_3.0.0.zip) · [客户端验证记录](client-audit.md)

## Racing

| 文件 | 调用和数据归属 |
| --- | --- |
| `shared/fusion_connect_ui.gd` | 单独处理连接和加入房间两个按钮，订阅连接、失败、进出房和状态变化信号，退出界面时断开连接。 |
| `scripts/main.gd` | 预注册汽车和摩托场景，入房后按选项生成其中一种，按玩家号放置，交给跟随相机。 |
| `scripts/vehicle.gd` | 权威车取得操控轴并写 `net_input_x/z` 与模型偏航 `net_yaw`，代理车读复制字段，模型旋转另做平滑。根刚体的 Forecast 由原生组件执行。 |
| `scripts/vehicle-motorcycle.gd` | 继承 Vehicle，覆盖车身倾斜、前叉和轮子表现，沿用父类的网络状态。 |
| `scripts/view.gd` | 物理帧内追随目标位置，按车速平滑相机距离，数据只影响本机视图。 |

## Starter

文件名位于表内对应场景的 `scripts/` 下。最后三项位于 `shared/scripts/utilities/`。

| 场景 | 文件 | 主体职责与网络边界 |
| --- | --- | --- |
| 第三人称 | `game.gd` | 入房生成本地角色，随机化出生点，Master 调用 `register_current_scene()`。 |
| 第三人称 | `player.gd` | 权威端执行角色移动和跳跃，复制着地状态，以允许本地执行的 RPC 播放跳跃。相机、步音和动画由各端驱动。 |
| 平台跳跃 | `game.gd` | 权威端按金币和终点裁决胜者，复制玩家 ID，定时广播重生；每端重置自己的角色和金币。 |
| 平台跳跃 | `player.gd` | 自己模拟角色、二段跳和重生。着地、金币、起跳与落地计数、昵称由场景声明复制，setter 驱动对应表现。 |
| 平台跳跃 | `falling_platform.gd` | 权威端写平台有效位和网络时间截止值。代理用相同网络时钟推导倒计时，逐帧更新碰撞与下落表现。 |
| 平台跳跃 | `coin.gd` | 本地先隐藏金币并向 owner 请求拾取。Master 所有的金币验证有效位，启动冷却，定向 RPC 为采集者记分。 |
| 平台跳跃 | `flag.gd` | 仅 Master 将角色触碰交给游戏的胜负判定。 |
| 平台跳跃 | `cloud.gd` | 本地随机相位和速度控制装饰云浮动。 |
| 平台跳跃 | `ui_platformer.gd` | 订阅本地金币及胜者变化，按复制的玩家 ID 找对应昵称。 |
| 射击示例 | `game.gd` | 管理生成、本地得分、最佳玩家与回合状态。权威端计算领先者和结束条件，广播重新开始。 |
| 射击示例 | `player.gd` | 本地角色执行移动与游戏命中检测，写开火计数、命中位置、法线、俯仰等字段。代理以计数推进效果，生命值另由 Health 组件处理。 |
| 射击示例 | `health.gd` | 受击请求发给目标 owner。权威端更新生命值及命中数，可定向通知得分。非权威端先播放受击效果，用本地计数避免重复触发。 |
| 射击示例 | `chicken_manager.gd` | 入房后的 Master 补足 NPC 数量，按生命组件的结束状态让原 NPC 重生。 |
| 射击示例 | `chicken.gd` | 权威端移动 NPC，重生时设置起点、速度、行程和生命值，并调用复制器传送。 |
| 射击示例 | `impact.gd` | 命中特效在动画结束后自行释放。 |
| 射击示例 | `ui_shooter.gd` | 订阅角色、生命值、分数与回合变化，在本机更新 HUD。 |
| 公用 | `scene_context.gd` | 保存当前游戏和本地玩家引用，替换本地玩家时发信号。 |
| 公用 | `ui_connection.gd` | 管理连接状态、取消和进出房。加入时设置最多 8 人，把 `game_mode` 放入房间自定义属性和大厅可见属性。昵称本地保存。 |
| 公用 | `ui_menu.gd` | 根据菜单项切换本地示例场景。 |
| 公用 | `environment.gd` | 调整兼容渲染器的太阳和阴影强度。 |
| 公用 | `ui_audio_button.gd` | 切换本地音量并更新图标。 |
| 公用 | `ui_button.gd` | 点击时播放公用按钮音效。 |
| 公用 | `utilities/audio_effect.gd` | 管理本地 3D 音频播放器池，按可用性与强制播放参数复用。 |
| 公用 | `utilities/effect.gd` | 启动粒子及声音，定时释放本地效果。 |
| 公用 | `utilities/window_memory.gd` | 按窗口标识在本地保存显示器、位置和尺寸，支持多窗口开发。 |

场景文件还能解释同一套复制器怎样支撑不同玩法。第三人称玩家声明 1 个自定义字段，平台跳跃玩家 5 个，射击玩家 11 个，其中两个绑定在 Health 子节点。金币、下落平台、全局胜负和 NPC 选择 MasterClient，角色场景选择 PlayerAttached。配置覆盖的字段、脚本中的本地变量和发出的事件可以沿这条线区分。[Starter 原包](../../../../references/photon/distributions/photon-fusion-godot-starter_3.0.0.zip)

## C# 包装层

| 文件 | 底层职责或桥接行为 |
| --- | --- |
| `Core/Fusion.cs` | 缓存单例，提供连接、房间、地图、兴趣和 RPC 入口。调用通过 GodotObject 和 Variant 转发。 |
| `Core/FusionWrapper.cs` | 持有底层对象。需要参数转换的托管事件在节点元数据中共享原生 Callable，最后一个订阅者退出时释放桥接。 |
| `Core/FusionEnums.cs` | 定义连接状态、RPC、拓扑、所有权、插值、物理校正、兴趣等 18 个枚举。 |
| `Core/NodeFusionExtensions.cs` | 将现有节点或按 NodePath 查得的节点包装为指定复制器、生成器或兴趣区。 |
| `Core/VariantExt.cs` | 将包含对象的 Variant 或数组转换为包装类型，数组转换跳过空对象。 |
| `Handles/FusionRoomOptions.cs` | 包装创建或加入用的参数，包括可见性、开放、人数、TTL、房间和大厅属性。 |
| `Handles/FusionRoom.cs` | 包装已进入房间的视图、玩家列表、属性和可修改设置。 |
| `Handles/FusionRoomListing.cs` | 包装大厅列表条目，与已加入房间的对象视图区分。 |
| `Handles/FusionPlayer.cs` | 包装玩家编号、昵称、User ID、Master 标志及非活跃状态。 |
| `Handles/FusionInterestArea.cs` | 包装形状、坐标平面、网格、半径、更新速率、衰减与调试绘制。 |
| `Handles/FusionRpcInfo.cs` | 包装当前 RPC 的发送玩家、目标值和目标节点。 |
| `Handles/FusionRpcResult.cs` | 通过 `OnFail` 注册给定 TTL 内的失败回调，返回结果句柄供继续调用。 |
| `Handles/FusionStateResetInfo.cs` | 读取 reset 原因，区分接收远端、预测重置和覆盖。 |
| `Replication/FusionReplicator.cs` | 通用配置、字段及根节点平滑、权威查询、时间戳、传送、状态重置与信号。 |
| `Replication/FusionSharedReplicator.cs` | 所有权意愿和应答。原生请求信号声明一个玩家 ID，随包托管事件使用两个参数且无返回值。 |
| `Replication/FusionServerReplicator.cs` | 指定输入玩家、入队、消费队列、读取累计时长与数量、接收处理输入回调。 |
| `Replication/FusionSpawner.cs` | 生成资产注册、预生成回调、销毁、地图模式、根/子对象绑定与通知。 |

13 个包装类的名称常量与实际 ClassDB 逐项比对，共 242 个方法名、87 个属性名、22 个信号名，全部命中。接口名核对和委托参数/返回值是两项验证。`FusionSharedReplicator` 的请求信号就是名称一致而签名有差异的实例。[随包 C# 源目录](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/cs) · [核验脚本](../probes/native_index.py)

