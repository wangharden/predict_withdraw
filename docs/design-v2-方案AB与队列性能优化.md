# 设计说明：predict_withdraw（方案 A/B + 行情回调性能） v2
> 目标环境：`C++14 / gcc 4.8`（Linux 服务器）
> 说明：本文只给出设计与落地路径，不直接修改代码。

## 0. 目标与非目标

### 本次目标

- 方案 A（不做“先撤后挂”改动）：
  - 保持现有“先挂后撤”（挂新单→新单确认→撤旧单→撤单确认）的闭环不变。
  - 增加“撤挂（撤旧+挂新）组合操作”的**耗时统计与打印**，给出“平均空窗时长预估（网络 RTT + 柜台处理 + 交易所处理）”的可观测指标（用于评估未来是否值得做先撤后挂），输出到time_spend.log。
  - 补齐状态机：旧单在撤单途中被**全部成交**时，不能等待撤单回报（可能永远等不到），应**终止该股票的功能实例**（仅该 symbol 停止，其他股票继续正常运行）。
- 方案 B（`flag_order` 更新规则优化）：
  - 50w 触发后不再立即刷新 `flag_order`，而是等待“我们触发的那笔 100 股涨停卖单”在行情流中出现后再更新。
  - 在**逐笔委托流**（`processOrder`）中匹配该 100 股卖委托
- 性能与正确性：
  - TDF 回调线程里 `MsgQueue::push()` 目前是“mutex + 深拷贝 + 频繁分配”，需要设计成“低阻塞/有界/可丢弃策略”的结构。
  - 行情分发热路径的第二热点：`StockDataManagerFactory::getStockManager()` 的 mutex，需要给出优化方案（白名单模式可只读化）。
  - `TdfMsgData` 深拷贝后 `pCodeInfo` 悬空指针风险需要在设计里明确修复点。

### 非目标

- 不在本次设计中做“大重构”（比如完全重写事件系统、全面改锁、引入复杂第三方并发库）。
- 不改变策略参数含义（阈值、触发条件、交易规则仅做必要的可观测性与一致性修正）。

## 1. 当前实现概览（便于对齐代码）

### 线程与数据流

- TDF 回调线程：
  - `MarketDataApi::OnDataReceived()` 过滤 `MSG_DATA_TRANSACTION/ORDER/MARKET` 后调用 `MsgQueue::push()`。
- 行情处理线程：
  - `MarketDataProcessor` 循环 `MsgQueue::pop()`，按 `szWindCode` 分发：
    - `StockDataManagerFactory::getStockManager(code)->processOrder/processTransaction/processMarketData(...)`
- 交易线程（worker thread）：
  - `OrderManagerWithdraw`：`post_limitup_trigger()` 投递事件，`workerThread()` 串行推进 per-symbol 状态机（下单/撤单）。
- 交易回报线程：
  - `TradeReturnMonitor`：接收柜台回报，回调 `OrderManagerWithdraw::handleMessage()`，驱动状态机推进。

### OrderManagerWithdraw 的 per-symbol 状态机（现有枚举）

`OrderManagerWithdraw::LimitUpOrderState::Phase`：

- `IDLE`
- `WAIT_SEND`（已接纳触发，等待 worker thread 发送新单）
- `WAIT_NEW_ACK`（已发送新单，等待委托确认）
- `WAIT_CANCEL_ACK`（已发送撤单，等待撤单确认）
- `STOPPED`（该 symbol 停止运行）

本文所有方案都以此枚举为准，不再引入新的命名体系，避免“方案名/代码名不一致”。

## 2. 方案 A：撤挂闭环耗时观测 + 撤单途中全量成交终止

### 2.1 背景：为什么要观测“撤挂耗时/空窗预估”

你已决定**暂不改为先撤后挂**，但仍需要量化评估：

- “撤挂闭环”在真实环境下的延迟分布（网络 RTT、柜台处理、交易所处理、SDK/本机排队）。
- 若未来尝试“先撤后挂”，潜在**空窗期**（交易所上无卖单挂着的时间）大致会有多长。

因此方案 A 的核心是：在不改变当前下撤顺序的前提下，把关键时间点打点、统计、输出。

### 2.2 关键时间点定义（建议统一使用 steady_clock 计算耗时）

建议使用 `steady_clock` 纳秒时间戳（代码里已有 `signal_steady_ns`、`send_steady_ns` 等字段）：

- （可选）`t_tdf_cb`：TDF 回调 `OnDataReceived()` 收到该笔行情的时刻（用于观测“从 TDF 回调到触发/下单”的全链路延迟）。
- `t_signal`：行情侧识别触发时刻（`LimitUpTrigger::signal_steady_ns`）。
- `t_worker_dequeue`：交易线程取到该 trigger 的时刻（可新增或复用内部记录点）。
- `t_send_new_req`：调用 `SECITPDK_OrderEntrust` 之前（或之后立刻）记录。
- `t_new_ack`：收到“新单委托确认回报”（对应现有 `NOTIFY_PUSH_ORDER` 分支里，匹配 `pending_sys_id` 的那次）。
- `t_send_cancel_req`：调用 `SECITPDK_OrderWithdraw` 之前（或之后立刻）记录（代码里已有 `last_cancel_send_ns` 类字段）。
- `t_cancel_ack`：收到“撤单确认回报”（`NOTIFY_PUSH_WITHDRAW` 或等价回报，匹配 `to_cancel_sys_id` / `CXOrderId`）。
- （可选）`t_new_seen_in_md`：在 TDF 逐笔委托流里首次看到“我们那笔 100 股涨停卖委托”的时刻（用于估计交易所可见时延，方案 B 会用到）。

### 2.3 需要输出的指标（打印 + time_spend.log）

每个 symbol 每次撤挂闭环建议输出一行（CSV 或结构化日志），至少包含：

- `symbol`
- `trigger_type` / `trigger_nTime`
- `seq`（状态机序号）
- （可选）`dt_cb_to_signal` = `t_signal - t_tdf_cb`（TDF 回调→策略触发的链路延迟）
- `dt_signal_to_send` = `t_send_new_req - t_signal`
- `dt_send_to_new_ack` = `t_new_ack - t_send_new_req`
- `dt_new_ack_to_cancel_send` = `t_send_cancel_req - t_new_ack`
- `dt_cancel_send_to_cancel_ack` = `t_cancel_ack - t_send_cancel_req`
- `dt_end_to_end` = `t_cancel_ack - t_signal`（撤挂闭环总耗时）

并做滚动统计（均值/分位数/最大值），用于你说的“平均空窗时长预估”。

### 2.4 “平均空窗时长”的预估口径（不改流程也能估）

因为当前是“先挂后撤”，严格意义上没有“无卖单”的空窗期；但我们可以给出一个**未来若改先撤后挂**时的空窗期预估：

- 推荐口径（更贴近“交易所可见”）：  
  `空窗期预估 ≈ t_new_seen_in_md - t_send_new_req`  
  这段包含了网络 RTT + 柜台处理 + 交易所处理 + 行情回传/本地排队（更保守、更接近真实风险）。
- 次优口径（只到柜台确认）：  
  `≈ t_new_ack - t_send_new_req`  
  这更像“柜台确认延迟”，未必等价于“交易所已经挂上”。

这两个口径都建议输出，让你对“柜台视角 vs 交易所可见视角”的差异有数。

### 2.5 关键补齐：撤单途中旧单被全部成交（避免状态机死锁）

问题现象（设计必须覆盖）：

- 旧单在撤单途中被全部成交时，系统可能**只收到成交回报**（`NOTIFY_PUSH_MATCH`），却**收不到撤单确认**（`NOTIFY_PUSH_WITHDRAW`）。
- 若状态机在 `WAIT_CANCEL_ACK` 只等撤单确认，会出现“永远等不到”的**死锁**。

设计要求：

- 在 `handle_limitup_trade_msg` 的 `NOTIFY_PUSH_MATCH` 分支中，增加对 `LimitUpOrderState::to_cancel_sys_id` 的匹配检查：
  - 若该成交回报对应“待撤旧单”，并且已经满足全量成交（例如 `OrderQty == TotalMatchQty`），则视为旧单生命周期结束。
- 处置策略（按你当前的业务约束）：
  - 该 symbol 的功能实例进入 `STOPPED`，并记录原因（“撤单途中全量成交”）。
  - 只停该 symbol：`limitup_states_` 是 per-symbol map，其他 symbol 的状态不受影响，继续运行。

建议的状态转换（仅描述行为，不涉及具体代码实现）：

- `WAIT_CANCEL_ACK` + `NOTIFY_PUSH_MATCH`(fill-to-cancel) → `STOPPED`
  - 清理：`to_cancel_sys_id/pending_sys_id/active_sys_id` 按需置零，避免后续误匹配。
  - 输出：一行 time_spend.log / spdlog，带 `seq`、`to_cancel_sys_id`、成交数量等。

## 3. 方案 B：`flag_order` 延迟更新（在逐笔委托流匹配）

### 3.1 目标回顾

50w 触发后：

- `m_sumAmountRaw` 清零并进入下一轮统计。
- `flag_order` 不再直接等于“触发 50w 的那笔 orderId”，而应等到“我们触发挂出的 100 股涨停卖委托”在行情流中出现后，再把 `flag_order` 刷到它（作为新的基准线）。



### 3.3 匹配规则（

在 `onSellSumThresholdHit(...)` 触发时：

- 不立即改 `m_flagOrder`，而是设置一个 pending 状态（例如 `m_pendingFlagOrderUpdate = true`），并记录：
  - `pending_event_time`（触发时的 `nTime`）
  - `pending_limitup_raw`
  - `pending_seq`（触发计数）
- 同时将 `m_sumAmountRaw` 清零。

在 `processOrder` 中：

当 `m_pendingFlagOrderUpdate == true` 时，寻找**第一笔**满足条件的委托作为“新的 flag_order”：

- 同一 symbol
- 卖方向
- 价格 == 涨停价
- 数量 == 100
- orderId > flag order

匹配到后：

- `m_flagOrder = orderId`（该笔委托的订单号字段）
- `m_pendingFlagOrderUpdate = false`
- 重要：**该笔匹配委托的 delta 不应累加进 `m_sumAmountRaw`**（避免把“我们自己的基准单”算进触发统计）。

### 3.4 pending 窗口内的 sum 归属（必须在设计里明确）

从 50w 触发到 `flag_order` 真正更新之间，中间可能会到达多笔涨停价卖委托。


- `m_pendingFlagOrderUpdate == true` 期间，**不累加**任何 `m_sumAmountRaw`（冻结统计）。
- 等 `flag_order` 确认更新后，再开始正常累加。


上海撤单（chOrderKind == 'D'）在 pending 期间的处理：冻结 m_sumAmountRaw 意味着不累加也不扣减。但如果 pending 期间有上海撤单到达（orderId > m_flagOrder，isShCancel），按当前代码逻辑会走扣减分支。冻结设计需要明确：pending 期间撤单扣减也冻结，否则 m_sumAmountRaw 可能被不该归入本轮的撤单减到负值。建议在方案里补一句："pending 期间 processOrder 对 orderId > m_flagOrder 的所有 delta 操作一律跳过（无论是新增还是撤单扣减），直到 flag_order 确认更新后才恢复。"

深圳撤单在 processTransaction 中的扣减：同理，pending 期间 processTransaction 中 askOrder > m_flagOrder 的成交/撤单扣减也应冻结。方案 3.4 节只提到了 processOrder，应补充 processTransaction 中同样需要检查 m_pendingFlagOrderUpdate。

## 4. 行情入口性能与正确性：MsgQueue + Factory

### 4.1 瓶颈 1：TDF 回调线程里的 `MsgQueue::push()`

现状：

- `MsgQueue::push()` 在 callback 线程里执行 `mutex + 深拷贝 + new[] + memcpy`，吞吐上来后会把压力反推到回调线程，引发排队与延迟抖动。
- `std::queue` 底层通常是 `deque`，push 过程中可能触发分配新 chunk，进一步放大抖动。

建议方案（优先级高）：

- 使用**有界 ring buffer**（预分配容量）替换 `std::queue`。
- 使用对象池/缓冲池，避免回调线程频繁 `new[]`。
- 明确溢出策略：
  - 优先丢 `MSG_DATA_MARKET`（快照）保 `ORDER/TRANSACTION`，或
  - 设定硬时间预算（例如自旋 N 微秒，仍满则丢弃并计数）。
- 增加计数器与观测：
  - `dropped_market/order/transaction`
  - `max_depth`
  - `callback_enqueue_ns`（push 耗时）
  - `queue_delay_ns`（从 push 到 pop 的排队时延）

### 4.2 Ring buffer 设计要点（slot/容量/SPSC-MPSC）

> 先说明：TDF 回调里一个 `TDF_MSG` 可能携带多条记录（`pAppHead->nItemCount > 1`）。  
> ring buffer 的 slot 设计显式选择“拆包入队”。
把白名单检查提到入队之前

#### 4.2.1 slot 数据布局（建议）

slot 建议固定长度（便于预分配 + O(1) 访问），由“自定义 header + payload”组成：

- `header`（建议 24~40B）：`data_type`、`server_time`、`item_size`、`payload_len`、`cb_steady_ns`（可选，用于测排队延迟）
- `payload`：存一条记录的原始 bytes（例如 1 条 `TDF_MARKET_DATA` / `TDF_ORDER` / `TDF_TRANSACTION`）

说明：

- 不建议在 slot 里保留 `TDF_MSG*` / `pCodeInfo` 等 SDK 指针字段（回调返回后可能失效）。
- 若 payload 是结构体 memcpy，拷贝后需要把结构体里的 `pCodeInfo` 置空（见 4.3）。

#### 4.2.2 两种入队策略：A / B

TDF 回调的 `TDF_MSG` 可以理解为：

- `nItemCount` 条记录（数组）
- 每条记录大小 `nItemSize`
- `nDataLen` 通常为 `nItemCount * nItemSize`

**A) 1 个 slot 存整个 `TDF_MSG`（含所有 items）**

- slot payload 需容纳：`sizeof(TDF_APP_HEAD) + nDataLen`
- 需要估算（或观测）`MAX_BATCH_SIZE = max(nItemCount)`，否则 slot 容量不可控
- 优点：push 侧一次入队（常数次写入）
- 缺点：slot 变长/变大，最坏情况下需要为“批量补发/批量打包”留出很大冗余；一旦低估会溢出

**B) 在 push 时拆包：1 条记录 = 1 个 slot（推荐）**

- slot payload 固定为：`max(nItemSize)`（只跟“你接收的数据类型结构体大小”有关）
- push 侧需要循环写 `nItemCount` 次（拆包开销：循环 + 多次 memcpy）
- 优点：slot 固定、内存可控；不需要 `MAX_BATCH_SIZE` 的先验估计；对 “批量推送” 天然鲁棒
- 缺点：一个回调要写多个 slot；需要定义“是否要求原子性（all-or-none）”

**原子性建议（针对 B）**

- 若希望“同一个回调里的 items 要么全部入队、要么全部丢弃”，则在写入前先检查剩余空槽：
  - `free_slots >= nItemCount` → 才开始写入；写完后一次性推进 `write_idx`
  - 否则整包丢弃并计数（drop 批量丢弃）
- 这样可保证消费者不会看到“半包数据”，且同一包内的 items 顺序保持一致。

#### 4.2.3 per-slot 大小（实测 + 工程取值）

你当前程序入队的数据类型是：`ORDER / TRANSACTION / MARKET`（`ORDERQUEUE` 当前不入队）。

用历史回放订阅 1500 只股票观测到（来自回调 `pMsgHead->pAppHead`）：

- `MARKET`：`nItemSize = 942`
- `TRANSACTION`：`nItemSize = 130`
- `ORDER`：`nItemSize = 118`

同时观测到的批量条数（历史回放订阅 1500 只股票，等待数据时间到 09:30 后观测约 10 秒；仅供参考，不应当作为硬假设）：

- `ORDER / TRANSACTION / MARKET`：`max(nItemCount) = 1`
- `ORDERQUEUE`：`max(nItemCount) = 2`（虽然当前程序不入队该类型）

因此若采用策略 B（单条记录一个 slot）：

- payload 最小需要 `>= 942B`
- 之前提到的 `954B` 是按“`TDF_APP_HEAD(12B)` + `MARKET item(942B)`”得到：`12 + 942 = 954`
  - 但实际 slot 不一定要存 `TDF_APP_HEAD`，通常只需存 `item_size/payload_len` 即可

工程上建议直接把 **slot 总大小定为 1024B**（含 header），payload 留足余量，便于算内存与对齐优化。

#### 4.2.4 ring buffer 总容量（slot 数）如何选

slot 数的选择取决于：

- 峰值吞吐（例如集合竞价 + 开盘瞬间，全市场逐笔可达 `~100k records/s`）
- 消费线程实际处理速度
- 你希望系统能“抗住”多长的瞬时积压（tolerate backlog）

估算公式（保守口径）：

- `slots_needed ≈ peak_rate * tolerate_seconds`
- 若消费速度明显低于生产速度，则改用：`(peak_rate - consume_rate) * tolerate_seconds`

建议先取一个保守的 2^n：

- `slots = 65536`
- 若 `slot_bytes = 1024B`，则内存约 `65536 * 1024 = 64MiB`（量级与“≈60MB”一致）

上线后根据指标调整：

- `max_depth`（观测到的最大深度）
- `dropped_*`（丢包计数，按类型/按批次）

#### 4.2.5 SPSC / MPSC 的选择

- 若确认 TDF 数据回调只有一个线程（单生产者），优先选择 **lock-free SPSC ring buffer**（实现简单、性能最好）。
- 若回调可能多线程并发（多生产者），需要 MPSC 方案或“回调侧先分片成多个 SPSC”（例如按数据类型拆）。

### 4.3 正确性：`TdfMsgData` 深拷贝后的悬空指针

`TDF_ORDER`/`TDF_TRANSACTION` 内部包含 `pCodeInfo` 等指针字段：

- memcpy 后这些指针仍指向 SDK 的原始内存，回调返回后可能失效。
- 当前代码虽然未使用该字段，但属于“埋雷”。

设计要求：

- 深拷贝后将 `pCodeInfo` 等 SDK 内部指针字段置为 `nullptr`（或做真正的深拷贝）。
- 作为硬约束写进设计与 code review checklist，避免未来有人使用它导致 crash。

### 4.4 瓶颈 2：`StockDataManagerFactory::getStockManager()` 的 mutex

现状：

- `MarketDataProcessor` 分发每条消息（甚至每个 item）都要 `getStockManager()`，该函数带 mutex。
- 白名单模式下 symbol 集合在启动时已知（`init_factory(white_list)` 已预创建），这把锁在热路径上属于不必要的固定成本。

建议改法（两档，按改动量选择）：

1) 小改动：`shared_mutex` 读写分离
   - 读路径 `shared_lock`（行情处理热路径）
   - 写路径 `unique_lock`（初始化/极少数新增）
2) 更稳的白名单模式：init 后只读化
   - 启动期预创建完所有 symbol 后，运行期不再 lazy create。
   - 运行期把容器视为只读，查找不加锁（或把 `unordered_map` 换成 `vector<ptr>` + 索引映射）。

### 4.5 其他并发/锁建议（来自 review，是否做取决于压测结果）

- `StockDataManager` 内部 `m_mutex`：
  - 若 `MarketDataProcessor` 确认是单线程串行调用 `processOrder/processTransaction/processMarketData`，且没有其他线程并发读写这些状态，则这把锁可能是冗余的固定开销。
  - 建议先通过压测与线程访问审计确认，再决定“保留但缩小粒度 / 去掉 / 只保护跨线程读写的字段”。
- `OrderManagerWithdraw::m_mutex`：
  - 当前一把锁保护 `m_msgQueue + m_limitupTriggerQueue + limitup_states_ + m_revocableOrders + 账户信息`，回报回调线程与行情线程会竞争该锁。
  - 若回报量大，建议把“消息队列”与“策略状态”拆分锁，或把回报消息先入无锁队列再由 worker thread 单线程消费（维持状态机单线程推进的优势）。

## 5. 验证计划（不改策略，仅验证一致性/可观测性/性能）

- 功能验证：
  - 撤单途中旧单全量成交：该 symbol 进入 `STOPPED`，不会死锁等待撤单确认；其他 symbol 正常。
  - 撤挂耗时日志：每次闭环都有完整打点，字段齐全，可计算均值/分位数。
  - `flag_order` 延迟更新：pending 期间冻结 `m_sumAmountRaw`，匹配到 100 股涨停卖委托后再恢复统计。
- 性能验证：
  - 全市场/高频逐笔压测时，TDF 回调线程不被 `push()` 长时间阻塞（drop/queue depth 可控）。
  - Factory 查找锁争用显著下降（或消失）。

## 6. 附录：design_review 里提到但未纳入本次主线的事项

- `StockDataManagerFactory::updateLimitupPrice()` 存在连续 `return` 的死代码（建议清理，避免误导）。
- `StockDataManager::m_limitUpPrice` 为 public 且被 Factory 直接写入：封装性与线程安全风险（建议提供 setter + 内部同步策略）。
- `orderManagerWithdraw` 为全局裸指针且缺少优雅退出/资源释放：生产环境收盘后应能停止线程并释放资源。
- `processOrder` 中 `m_isLimitUp` 检查与撤单扣减路径的交互：封板后仍可能扣减 `m_sumAmountRaw`（需确认是否为预期）。
