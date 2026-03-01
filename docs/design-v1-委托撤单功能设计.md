# 涨停价卖单委托/撤单新增功能设计（含落地实现） v1

> 目标环境：`C++14 / gcc 4.8`（Linux 服务器）；本设计面向高性能、可回放验证、可观测（日志/耗时）落地。  
> 背景：本项目已实现“涨停价位卖侧累计新增金额，每新增 50 万打印一次日志”的跟踪能力，当前验证可正常跟踪。  
> 本文在此基础上，新增“自动挂涨停价卖单 + 回报驱动撤上一单 + 覆盖式节流（忙时不排队）”的委托行为设计。
>
> 实现落位（本仓库当前版本）：`predict_withdraw/stock_data_manager.*`（触发信号产生） + `predict_withdraw/order_manager_withdraw.*`（回报驱动下单/撤单闭环） + `predict_withdraw/main.cpp`（账户信息注入）。

---

## 0. 现有代码脉络（为设计落位）

### 0.1 行情线程（TDF ORDER/TRANSACTION/MARKET）

- 生产：`MarketDataApi::OnDataReceived()` 过滤后 `MsgQueue::push()`
- 消费：`MarketDataProcessor` 单线程 `processMsgThreadFunc()` FIFO 串行处理
- 分发：
  - `handleOrderData()` → `StockDataManager::processOrder()`
  - `handleTransactionData()` → `StockDataManager::processTransaction()`
  - `handleMarketData()` → `StockDataManager::processMarketData()`

### 0.2 交易回报线程（SEC ITPDK stStructMsg）

- 入口：`OrderManagerWithdraw::staticAsyncCallback()`（SDK 回调线程）
- 入队：`OrderManagerWithdraw::handleMessage()` → `m_msgQueue`
- 消费：`OrderManagerWithdraw::workerThread()` 单线程处理 `NOTIFY_PUSH_ORDER / MATCH / WITHDRAW / INVALID`

> 参考工程（`D:\\work\\sell3\\result`）的关键点：**SDK 回调只入队**，重逻辑在工作线程处理；并维护 **本地订单ID↔柜台sys_id 映射** 来识别“本程序下的单”。

### 0.3 50 万触发点（当前已存在、已验证）

- `StockDataManager::processOrder()`：09:30 后、涨停价、卖委托，累加 `m_sumAmountRaw`，达到阈值触发：
- `StockDataManager::onSellSumThresholdHit()`：打印 `[LIMITUP_SELL_50W] ...`，并刷新 `flag_order/sum/trigger_count`

---

## 1. 新需求拆解（严格按业务描述）

### 1.1 触发规则（09:30 之后）

满足以下任一条件，触发“挂一笔涨停价卖单”：

1) **价格触发**：逐笔成交价 `price_tick > base_price * 1.07`  
2) **首次 50 万触发**：`flag_order` 第一次确认后，第一次累计新增达到 50 万（也就是 `trigger_count_50w == 1` 的那次阈值命中）

此后：

3) **每次 50 万触发**：每次“涨停价卖出档口新增 50 万”（即当前日志触发条件）都要再挂一笔涨停价卖单

### 1.2 委托/撤单规则

- 每次触发都**发送**一笔“涨停价卖单（固定 100 股）”
- 当收到该笔卖单的 **委托成功回报**（`NOTIFY_PUSH_ORDER`）后：
  - 对“本程序发出的上一笔涨停价卖单”发送撤单
  - 必须检查是否收到 **撤单委托成功回报**（`NOTIFY_PUSH_WITHDRAW`）
  - 若收到 **废单回报**（`NOTIFY_PUSH_INVALID`），则再次尝试撤单（需要有重试上限/超时保护，避免卡死）
- **密集触发约束（覆盖式）**：同一股票同一时刻最多推进一个“下单→确认→撤上一单→撤单确认”的闭环；闭环执行中发生的 50 万触发**不累计、不排队**（待执行次数保持 0），闭环结束后必须等待**新的** 50 万触发信号才会再执行下一次闭环
- **封板后停止**：一旦判断“封板/涨停已成立”（见 4.3/9），停止本策略的后续触发与下单闭环

### 1.3 日志与耗时

- 每次委托成功回报，打印一次其“交易所时间”（不是机器 local_time）
- **本期不在程序内做耗时计算**：仅把“可离线计算耗时”的原始信息写入 `time_spend.log`，待程序结束后再统一计算与分析
  - 原始时间点建议使用本机 `steady_clock` 的 `time_since_epoch`（ns/us 均可），只用于同一次运行内做差
  - 同时记录交易所/柜台时间字段（`ConfirmTime/pTime/nTime`），用于与行情时间对齐与排查延迟来源

---

## 2. 核心设计：回报驱动的“单股票状态机 + 事件队列”

### 2.1 为什么要状态机（解决密集触发 + 高性能）

触发点在行情线程，回报在交易线程；密集触发会导致：

- 行情线程如果直接下单/撤单：容易阻塞行情处理（SDK 调用、IO、锁）
- 如果不做门控：会产生“多个下单与撤单并发在途”，状态难以收敛，日志与撤单匹配变复杂

因此采用：

- **单股票单状态机**：保证每只股票同一时刻只有一个闭环在推进
- **事件队列解耦**：行情触发只投递轻量事件；交易回报工作线程推进状态机

### 2.2 建议的模块边界（不改现有线程模型）

新增一个逻辑组件（命名可选）：

- `LimitUpSellOrderEngine`（仅逻辑，不直接跑线程）
  - 输入 A：来自行情线程的触发事件（50 万、价格 1.07）
  - 输入 B：来自交易回报线程的回报事件（order/match/withdraw/invalid）
  - 输出：调用 ITPDK 下单/撤单接口（在交易工作线程执行）

投递通道建议复用 `OrderManagerWithdraw::workerThread()`：

- 行情线程 → 调用 `OrderManagerWithdraw::post_trigger(event)` 入队（线程安全）
- 交易回报线程在 `workerThread()` 中：
  - 先处理 `stStructMsg` 回报事件
  - 再处理触发事件
  - 在同一个线程推进状态机 → 无需对状态机加锁（每只股票状态天然串行）

> 这一点与参考工程的“回调入队 + 单线程 dispatcher”思想一致。

---

## 3. 关键数据定义（高性能口径）

### 3.1 价格与阈值统一用 raw 整数

- 行情：`TDF_ORDER.nPrice / TDF_TRANSACTION.nPrice` 都是 `__int64` raw（*10000）
- 阈值：50 万金额用 `kThreshold50wRaw = 500000 * 10000`（当前代码已采用）
- 1.07 触发建议用整数比较避免 double：

```
tick_raw * 100 > base_raw * 107
```

### 3.2 TriggerEvent（行情触发事件）

建议字段（尽量 POD，避免 string 分配）：

- `uint32_t symbol_id`：白名单内股票编号（启动时建立 string→id 映射；事件里只传 id）
- `int event_time_hhmmssmmm`：TDF 的 `nTime`（交易所时间口径）
- `enum TriggerType { PRICE_107, SELL_SUM_50W }`
- `uint32_t seq`：本股票触发序号（用于日志串联）
- `int64_t limitup_raw`：触发时的涨停价 raw（避免跨线程读 `StockDataManager`）

> 如果短期不想引入 `symbol_id`，可先传 `char[32] wind_code`；但要注意频繁 `std::string` 哈希会增加热点开销。

### 3.3 SymbolState（每只股票的订单状态）

最小状态集合：

- 基础：
  - `int64_t base_raw`：基准价 raw（见 4.1 取值）
  - `bool base_ready`
  - `bool price107_fired`：是否已发生过 1.07 触发（只触发一次）
- 订单与状态机：
  - `enum Phase { IDLE, WAIT_NEW_ACK, WAIT_CANCEL_ACK }`
  - `int64_t active_sys_id`：当前仍保留的“本程序涨停卖单”柜台单号（已确认）
  - `int64_t pending_sys_id`：刚发出的新卖单柜台单号（等待确认）
  - `int64_t to_cancel_sys_id`：新单确认后需要撤掉的上一单
  - `int cancel_attempts`
  - `uint32_t suppressed_signals_while_busy`：忙时被抑制的触发次数（仅用于观测，不用于排队执行）
- 耗时采样：
  - `steady_clock::time_point t_signal`
  - `steady_clock::time_point t_send`
  - `steady_clock::time_point t_ack`
  - `std::string last_ack_exch_time`（从回报里抽取的交易所时间字段）

---

## 4. 触发信号的产生与基准价定义

### 4.1 基准价 `base_price` 的定义（已确认）

基准价使用**快照中的涨停价倒推**得到（按你指定的 `round(limitup/1.1 + 1e-6)` 口径；默认 10% 涨跌停）：

- 取快照涨停价 raw：`limitup_raw = TDF_MARKET_DATA.nHighLimited`（价格 * 10000）
- 将价格对齐到股票最小跳动单位（通常 `0.01`，即 raw 的 `100`）：
  - `limitup_tick = limitup_raw / 100`（单位：分/cent，对应 0.01 元）
- 倒推出基准价（你的口径在 tick 单位上做 round）：
  - `base_tick = round(limitup_tick / 1.1 + 1e-6)`
  - `base_raw = base_tick * 100`

实现建议（更贴近高性能/可控误差）：

- 全流程用 raw 整数价（`price_raw = price * 10000`）存储与比较
- 仅在首次拿到 `limitup_raw>0` 时计算一次 `base_raw`（后续无需反复计算）
- `base_tick` 的计算可用二选一：
  - double 口径：`base_tick = llround(limitup_tick / 1.1 + 1e-6)`
  - 纯整数口径（避免浮点误差/更快）：`base_tick = (limitup_tick * 10 + 5) / 11`

> 注意：该倒推只适用于 10% 涨跌停规则；若存在 20%（创业板/科创板等）或 5%（ST）标的，需要引入“涨跌停比例”参数，否则基准价会系统性偏差。

### 4.2 1.07 触发点落位

建议落位在 `StockDataManager::processTransaction()`：

- 条件：`trans.nTime >= 09:30` 且 `base_ready`
- 判断：`trans.nPrice`（逐笔成交价 raw）是否首次满足 `> base_raw * 1.07`
- 命中后发出 `TriggerType::PRICE_107`（仅一次）

> 注意：这与 50 万触发是“或”的关系；状态机层保证只要触发事件到达且允许推进，就会下第一笔涨停卖单。

### 4.3 字段校验：不要用 Transaction 的 `FunctionCode` 判买卖（已对照 `predict_withdraw/doc/字段说明`）

你提到“成交价=涨停价，FunctionCode='B' / 或看 OrderType”的记忆点，这里按本项目文档与头文件口径澄清：

- **逐笔委托 `TDF_ORDER`**
  - 买卖方向：`chFunctionCode`（`'B'`=买，`'S'`=卖）
  - 上海撤单：`chOrderKind=='D'`
- **逐笔成交 `TDF_TRANSACTION`**
  - 买卖方向：`nBSFlag`（`'B'`=买，`'S'`=卖，`' '`=不明/集合竞价等）
  - `chFunctionCode` 在 **SH 通常为空**；在 **SZ 常见为 `'0'`(正常成交) / `'C'`(撤单)**，不是 `'B'/'S'`

结论：若要以“成交方向”判定条件（例如封板/终止），应使用 `TDF_TRANSACTION.nBSFlag`；若看到 `FunctionCode='B'`，更可能来自 `TDF_ORDER` 流而非 `TDF_TRANSACTION` 流。

---

## 5. 状态机细节（同一股票的门控与撤单）

### 5.1 状态迁移概览

```
IDLE
  └─(trigger)→ send_new_order → WAIT_NEW_ACK

WAIT_NEW_ACK
  ├─(PUSH_ORDER for pending)→ on_new_ack
  │        ├─ no previous → active=pending → READY(=IDLE with active)*
  │        └─ has previous → active=pending; send_cancel(previous) → WAIT_CANCEL_ACK
  └─(PUSH_INVALID for pending)→ pending 清理，回到可触发状态（保留 active 不变），并记录失败

WAIT_CANCEL_ACK
  ├─(PUSH_WITHDRAW for to_cancel)→ cancel done → phase=IDLE/READY
  └─(PUSH_INVALID for to_cancel)→ cancel_attempts++ → retry cancel（有上限/告警）
```

说明：

- “READY”在实现上可以仍用 `Phase::IDLE` 表示“空闲可接新触发”，并用 `active_sys_id!=0` 表示当前持有一笔本程序挂单。
- **原子闭环**：一次“挂单（新单确认）→撤前一单（撤单确认）”视为一个原子闭环，闭环执行中不允许并行推进下一次闭环。
- **密集触发（覆盖式）**：任何触发在 `WAIT_NEW_ACK/WAIT_CANCEL_ACK` 时不立即下单，且**不做排队计数**（待执行次数保持 0）。闭环结束后，必须等待新的触发事件才会再执行下一次闭环。

### 5.2 下单与撤单调用点（必须在交易工作线程）

为避免阻塞行情线程，建议：

- 所有 `SECITPDK_OrderEntrust()`、`SECITPDK_OrderWithdraw/WithdrawEx()` 均在 `OrderManagerWithdraw::workerThread()` 内执行
- SDK 回调线程只入队（保持现状）

### 5.3 “上一笔涨停卖单”的定义

上一笔指：**本程序发出的、仍处于可撤状态的涨停价卖单**。

落地建议：

- 对每个 symbol 仅维护一个 `active_sys_id`（已经确认的那笔）
- 每次触发发送新单得到 `pending_sys_id`：
  - 在 `pending` 确认成功后，将当时的 `active_sys_id` 复制到 `to_cancel_sys_id` 并发撤单
  - 然后将 `active_sys_id = pending_sys_id`

这样可保证：

- 新单确认前：旧单仍在（满足“不断档”）
- 新单确认后：旧单进入撤单流程

### 5.4 覆盖式触发投递（避免队列被密集触发打爆）

为了让“闭环忙时触发不排队”不仅是**业务语义**，也能在实现上避免触发事件在队列中堆积，建议在触发产生侧做一次轻量门控：

- 为每个 symbol 维护一个可跨线程读取的轻量状态（推荐 `std::atomic<uint8_t> phase_atomic`，值域同 `Phase`）
  - **交易线程**在进入 `WAIT_NEW_ACK/WAIT_CANCEL_ACK` 时写 `phase_atomic = BUSY`，闭环结束写回 `IDLE`
- **行情线程**在产生 `SELL_SUM_50W` 触发前做一次 `phase_atomic.load(relaxed)`：
  - 若非 `IDLE`：直接丢弃该触发（可用原子计数/采样日志做观测），符合“待执行次数保持 0”
  - 若为 `IDLE`：才投递 trigger 事件到交易线程

> 这样实现后，即便出现“连续密集 50 万触发”，队列长度也不会被触发事件本身拖垮；下一次闭环只会由**闭环结束后的新触发**启动。

---

## 6. 回报匹配与“交易所时间”的提取

### 6.1 识别“本程序的单”

建议以 `sys_id`（`SECITPDK_OrderEntrust` 返回值）为主键：

- 下单成功：记录 `pending_sys_id = sys_id`，并加入 `orderid->symbol_state` 映射
- 回报到达（`stStructMsg`）：
  - **优先使用 `stMsg.OrderId`** 与映射匹配（参考工程就是这样做的）
  - 若遇到“撤单废单/撤单回报使用 CXOrderId”的特殊柜台口径，则在上线前通过日志验证后增加兼容（见 6.3）

### 6.2 “委托成功回报的交易所时间”

`stStructMsg` 中与时间相关字段：

- `ConfirmTime[13]`：确认时间（最可能是交易所/柜台确认时刻）
- `MatchTime[13]`：成交时间（仅成交推送有意义）
- 回调入参 `pTime`：SDK 额外时间字符串（需验证其语义）

建议落地步骤（先可观测再收敛）：

1) 首版同时打印：`pTime`、`ConfirmTime`、`OrderStatus`、`ResultInfo`
2) 线上验证哪个字段是“交易所时间”后，将日志固定为该字段（通常是 `ConfirmTime`）

### 6.3 撤单回报与废单回报的语义核对（必须做一次验证）

由于本项目现有 `OrderManagerWithdraw::updateRevocableOrders()` 对 `CXOrderId/OrderId` 的使用方式存在歧义可能，
建议在开发时增加一次性诊断日志（不影响性能的前提下）：

- 对每次撤单请求，记录：
  - `cancel_req_seq`、`target_sys_id`、`local_send_time`
- 对每条回报，打印（仅对本策略关心的 symbol）：
  - `nType`、`OrderId`、`CXOrderId`、`WithdrawQty`、`OrderStatus`、`ResultInfo`、`ConfirmTime`

验证点：

- `NOTIFY_PUSH_WITHDRAW` 到达时，哪个字段等于被撤目标单号？
- `NOTIFY_PUSH_INVALID` 到达时，是否存在“撤单废单”场景？其字段如何标识？

在语义确认后再把“废单触发撤单重试”的匹配条件写死，避免误判导致无意义重试。

---

## 7. 日志设计（建议格式）

### 7.1 触发日志（行情侧）

保持现有 `[LIMITUP_SELL_50W]` 不变；新增两类辅助日志：

- `[LUP_ORDER_SIGNAL] code=... type=PRICE_107 time=... base_raw=... tick_raw=...`
- `[LUP_ORDER_SIGNAL] code=... type=SELL_SUM_50W time=... seq=... limitup_raw=...`

> 注意：行情线程尽量只打“少量且结构化”的日志；更重的耗时统计在交易线程回报处输出。

### 7.2 下单/回报/撤单日志（交易侧）

**运行日志（server_logs）**：每笔闭环输出 1 组关键日志（建议都带 `symbol/seq/sys_id` 方便 grep）：

- `[LUP_ORDER_SEND] code=... seq=... sys_id=... price=... qty=... reason=...`
- `[LUP_ORDER_ACK]  code=... seq=... sys_id=... exch_time=...`
- `[LUP_CANCEL_SEND] code=... seq=... target_sys_id=... attempt=...`
- `[LUP_CANCEL_ACK]  code=... seq=... target_sys_id=...`
- `[LUP_CANCEL_INVALID] code=... seq=... target_sys_id=... attempt=... result=... (then retry)`

**离线耗时原始数据（time_spend.log）**：不在程序内计算耗时，只输出原始字段，离线脚本按 `sys_id/seq` join 后做差。

建议每行 CSV（字段可按需要增减，但保持版本号便于兼容）：

- `v1,ORDER_SEND,symbol,seq,reason,trigger_nTime,signal_steady_ns,send_steady_ns,limitup_raw,base_raw,sys_id`
- `v1,ORDER_ACK,symbol,seq,sys_id,pTime,ConfirmTime,ack_steady_ns,OrderStatus,ResultInfo`
- `v1,ORDER_INVALID,symbol,seq,sys_id,pTime,ConfirmTime,invalid_steady_ns,OrderStatus,ResultInfo`（新单废单/拒单）
- `v1,CANCEL_SEND,symbol,seq,target_sys_id,attempt,cancel_send_steady_ns`
- `v1,CANCEL_ACK,symbol,seq,target_sys_id,pTime,ConfirmTime,cancel_ack_steady_ns,OrderStatus,ResultInfo`
- `v1,CANCEL_INVALID,symbol,seq,target_sys_id,attempt,pTime,ConfirmTime,invalid_steady_ns,OrderStatus,ResultInfo`

---

## 8. 性能实现要点（结合本项目已有“锁延迟”分析）

> C++ 的高性能来源不仅是“更贴近硬件”，更关键是：**可控的内存布局/分配、可预测的开销模型、编译器优化空间大、避免隐藏分配与虚调用、减少跨核共享写**。

结合本项目与 `延迟计算思路.md` 的结论（热点往往是临界区内容器/分配，而不是 mutex 指令本身），建议：

1) **行情热路径最小化**：触发点只构造 POD 事件并入队；不要在行情线程调用交易 API
2) **避免 string 哈希热点**：优先用 `symbol_id`（uint32）或预分配的固定缓冲；必要时对 `unordered_map` 预 `reserve()`
3) **密集触发用覆盖式节流**：闭环忙时触发直接抑制（不排队、不累计），只做观测计数（如 `suppressed_signals_while_busy`）；事件队列只承载“有新触发到达”这一事实
4) **整数运算优先**：1.07 判断与涨停价比较尽量用 raw 整数；double 只在 ITPDK 接口参数处转换
5) **日志别 flush 过频**：只在关键节点 flush；否则 IO 可能成为真实瓶颈，掩盖“下单→确认”的实际延迟

---

## 9. 边界情况与降级策略（避免状态机卡死）

必须显式处理的异常/边界：

1) `limitup_price` 缺失（0 或未初始化）：
   - 无法下涨停价卖单；同时 `base_price` 也无法按倒推法计算，因此 `PRICE_107` 触发不启用
   - 建议：记录一次告警并直接抑制（不排队），等待后续快照更新出 `nHighLimited` 后自然恢复
2) `base_price` 缺失：
   - `base_price` 由 `limitup_price` 倒推得到，因此与 1) 同源；不单独处理
3) 新单下单失败（`OrderEntrust<=0`）：
   - 记录错误原因；不推进撤单；保持 `active_sys_id` 不变；等待下一次触发再尝试
4) 撤单回报缺失：
   - 增加超时（如 2s/5s）告警，并允许放行下一次触发（或主动 QueryOrders 校验后放行）
5) 撤单废单反复出现：
   - 重试上限（如 3 次）+ 进入“撤单失败降级态”（停止继续下新单，避免扩大风险）
6) 封板后停止：
   - 触发停止条件建议以 `StockDataManager::get_LimitUpStatus()`（`m_isLimitUp`）为准（现代码在 `TDF_TRANSACTION.nPrice==limitup_raw` 且 `static_cast<char>(nBSFlag)=='S'` 时置位；方向字段见 4.3）
   - 若封板信号在闭环执行中到达：允许当前闭环完成（保证状态一致），但闭环结束后不再接纳新触发/不再下新单

---

## 10. 建议的落地步骤（实现顺序）

1) **只做事件通路**：把 50 万触发与 1.07 触发投递到交易线程（不真正下单），验证不会引入明显延迟
2) **加入下单但不撤单**：验证委托成功回报的时间字段（ConfirmTime/pTime）
3) **加入“确认后撤上一单”**：先不做 invalid 重试，仅做 withdraw 成功闭环
4) **加入 invalid 重试与超时保护**：用真实回放/线上少量股票灰度验证
5) **开启密集触发压测**：用历史数据模拟 50 万触发密集场景，检查“忙时触发抑制（覆盖式）”是否符合预期，并统计 `suppressed_signals_while_busy`

---

## 11. 已确认的业务参数（按你最新回复固化）

1) `base_price`：用快照涨停价倒推（按 4.1 的 tick 口径）`base_tick = round(limitup_tick/1.1 + 1e-6)`
2) 涨停价卖单委托数量：固定 `100` 股
3) 每日最多下单/撤单次数：不需要（不做额外限制）
4) 封板后：停止本策略（不再继续触发/下单闭环）
