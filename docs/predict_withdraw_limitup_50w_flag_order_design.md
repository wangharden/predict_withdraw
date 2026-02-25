# predict_withdraw 涨停价卖侧 50 万累计打印功能设计（澄清版，仅文档）

> 范围：仅输出设计、线程行为说明、函数逻辑与代码实体位置，不修改源码。  
> 本稿已按你最新澄清改为“卖侧逻辑”。  
> 结合了 `predict_withdraw/doc/示范.md`（逐条分发模式）与 parquet 验证结论（沪深撤单位置不同）。

---

## 1. 代码实体位置（你问的 `StockDataManager` 在哪）

- `StockDataManager` 声明：`predict_withdraw/stock_data_manager.h`
- `StockDataManager` 实现：`predict_withdraw/stock_data_manager.cpp`
- 行情统一分发线程入口：`predict_withdraw/market_data_processor.cpp`

当前空实现位置：

- `predict_withdraw/market_data_processor.cpp:102` (`handleOrderData`)
- `predict_withdraw/market_data_processor.cpp:109` (`handleTransactionData`)
- `predict_withdraw/stock_data_manager.cpp:26` (`processOrder`)
- `predict_withdraw/stock_data_manager.cpp:31` (`processTransaction`)

---

## 2. 本次功能的正确理解（按你最新描述）

统计对象：**涨停价卖盘累计金额（卖侧）**

### 2.1 累计增加（Add）

读取逐笔委托（`order`）：

- 判断是否为目标股票（白名单）
- 判断是否为涨停价
- 判断是否为卖出委托
- 满足则将 `price * volume` 计入累计金额 `sum`

### 2.2 累计扣减（Subtract）

1. 撤单扣减
- 判断是否为涨停价卖出撤单
- 对比委托号 `order`（或对应卖侧号）与 `flag_order`
- 若 `order > flag_order` 则扣减累计金额，否则跳过

2. 成交扣减
- 读取逐笔成交（`trade`）
- 判断是否为目标股票、涨停价成交
- 对比 `askOrder` 与 `flag_order`
- 若 `askOrder > flag_order` 则扣减累计金额，否则跳过

### 2.3 触发条件

- 当累计金额 `sum >= 500000`（或你写的 `sum - 500000 >= 0`）时：
  - 打印一次信息
  - 刷新 `flag_order`（用当前触发事件对应订单号）
  - 刷新 `sum`

关于“刷新 sum”的建议：

- 方案 A（更贴近你的表述）：`sum = 0`
- 方案 B（更稳，不丢超额）：`sum -= 500000`（可循环触发）

建议先按方案 A 实现，后续若发现单笔跨阈值较多再改方案 B。

---

## 3. 数据来源约束（你补充的两点）

### 3.1 目标股票范围

目标股票来自：

- `predict_withdraw/white_list.txt`

现有程序已经通过 `SettingsManager::load_white_list()` 加载白名单，并传给 `StockDataManagerFactory::init_factory(...)` 初始化管理器。

### 3.2 涨停价来源

你明确要求：**涨停价从快照字段获得**（不是仅靠启动时查询）。

当前代码现状：

- 行情模块在 `main` 中被注释，未启用：`predict_withdraw/main.cpp:57`
- `MarketDataApi` 只推送 `ORDER/TRANSACTION` 入队：`predict_withdraw/market_data_api.cpp:78`
- `MarketDataProcessor` 对 `MSG_DATA_MARKET` 当前直接忽略：`predict_withdraw/market_data_processor.cpp:81`

因此若要按本需求落地，需要后续同时补：

1. 启用行情模块（`main.cpp` 取消注释）
2. 接收 `MSG_DATA_MARKET`
3. 在 `StockDataManager` 中更新 `m_limitUpPrice`

---

## 4. 沪深撤单位置差异（已用 parquet 验证）

已检查样本：

- `E:\dm\data\l2_order_data\2021-08-11\002136.parquet`（SZ）
- `E:\dm\data\l2_trade_data\2021-08-11\002136.parquet`（SZ）
- `E:\dm\data\l2_order_data\2021-08-11\600383.parquet`（SH）
- `E:\dm\data\l2_trade_data\2021-08-11\600383.parquet`（SH）

### 4.1 深圳 `002136.SZ`

- `order` 流：
  - `functionCode` 主要是 `B/S`（方向）
  - 未看到撤单标志
- `trade` 流：
  - `functionCode='0'`：成交
  - `functionCode='C'`：撤单

结论（SZ）：

- **撤单在 `trade` 流识别，字段看 `functionCode == 'C'`**

### 4.2 上海 `600383.SH`

- `order` 流：
  - `functionCode` 是方向 `B/S`
  - `orderKind='A'`：普通委托
  - `orderKind='D'`：撤单
- `trade` 流：
  - `functionCode/orderKind` 基本为空，不适合做撤单判断

结论（SH）：

- **撤单在 `order` 流识别，字段看 `orderKind == 'D'`**

---

## 5. 字段映射（卖侧逻辑必须统一）

### 5.1 卖出委托方向（`order` 流）

`TDF_ORDER` 没有 `bsFlag`，方向用：

- `TDF_ORDER.chFunctionCode`

因此“卖出委托”应写成：

```cpp
order.chFunctionCode == 'S'
```

### 5.2 卖侧订单号链路（`flag_order` 对应字段）

本轮是卖侧逻辑，所以 `flag_order` 应绑定卖侧订单号：

- 卖委托新增（order）：`TDF_ORDER.nOrder`
- 成交扣减（trade）：`TDF_TRANSACTION.nAskOrder`（卖方委托号）
- 深圳撤单扣减（trade）：优先用 `TDF_TRANSACTION.nAskOrder`
- 上海撤单扣减（order）：用 `TDF_ORDER.nOrder`（且 `orderKind=='D'`）

---

## 6. 调用链与线程模型（回答你“是否统一分发/是否多线程”）

## 6.1 TDF 行情（ORDER/TRADE）在你们程序内的处理模型

当前代码（若行情模块启用）是：

1. TDF SDK 回调 `MarketDataApi::OnDataReceived(...)`
2. 将 `ORDER/TRANSACTION` 深拷贝后放入 `MsgQueue`
3. `MarketDataProcessor` 的单个线程从 `MsgQueue` 串行消费
4. 在 `handleOrderData/handleTransactionData` 中逐条分发到 `StockDataManager`

关键点：

- `OnDataReceived` 只做过滤 + 入队：`predict_withdraw/market_data_api.cpp:69`
- `MsgQueue` 是线程安全队列：`predict_withdraw/msg_queue.cpp:220`
- `MarketDataProcessor` 只有一个消费线程：`predict_withdraw/market_data_processor.cpp:59`

结论：

- **在业务处理层面，TDF 的 `ORDER`/`TRADE` 是统一分发、单线程串行处理的。**

## 6.2 `trade_return_monitor` 是否并行

是并行的，而且是另一条链路（交易回报，不是 TDF 行情）：

- `OrderManagerWithdraw` 注册 ITPDK 回调后，把消息放自己的队列：`predict_withdraw/order_manager_withdraw.cpp:69`
- `OrderManagerWithdraw::workerThread()` 单线程消费：`predict_withdraw/order_manager_withdraw.cpp:89`
- `TradeReturnMonitor::on_match()` 在这个 worker 线程中被调用：`predict_withdraw/order_manager_withdraw.cpp:126`
- `TradeReturnMonitor` 自己还有 `snapshot_thread_`：`predict_withdraw/trade_return_monitor.cpp:63`

结论：

- **`trade_return_monitor` 与行情处理线程（若启用）是并行线程。**
- **两边不是同一个队列，不是同一个分发线程。**

---

## 7. 如果生产者更快（一个包没处理完又来了），当前程序怎么处理

你问的这个情况，在当前设计里是会发生的，而且是“正常可发生”。

### 7.1 当前行为（实际机制）

当 `MarketDataProcessor` 还在处理上一个包时，新的 TDF 回调到来：

- `OnDataReceived()` 继续执行
- 新包被深拷贝并 `push` 到 `MsgQueue`
- 消费线程处理完当前包后再按 FIFO 顺序取下一个包

也就是说：

- **不会覆盖旧包**
- **不会并行处理两个包**
- **不会自动丢包（代码层面没有主动丢弃逻辑）**
- **队列会积压增长**

### 7.2 当前 `MsgQueue` 的性质（风险点）

`MsgQueue` 目前是无界 `std::queue<TdfMsgData>`（没有容量上限）。

当生产速度长期 > 消费速度时：

1. 队列长度持续增长
2. 内存占用持续增长（每个包都深拷贝）
3. 处理延迟变大（你看到的数据越来越“晚”）
4. 极端情况下可能触发分配失败/异常（`push` 内部 catch 后静默吞掉）

### 7.3 对你这个功能的影响

你的统计是“逐笔顺序累加/扣减 + flag_order 比较”，因此顺序非常关键。

当前设计的优点：

- 单线程消费保证顺序一致（FIFO）

当前设计的风险：

- 高峰期延迟大，打印时点会明显滞后真实市场时间

### 7.4 后续若要抗压（非本轮实现）

可选方案（后续优化）：

1. 增加队列长度监控日志（先观测）
2. 对非白名单股票在回调侧尽早过滤（当前已经通过订阅列表部分过滤）
3. 引入有界队列 + 丢弃策略（需业务允许）
4. 批量处理/减少日志
5. 避免不必要的深拷贝（需重构回调生命周期管理）

---

## 8. 与 `示范.md` 对齐的实现方式（本轮建议）

你给的 `predict_withdraw/doc/示范.md` 是逐条分发模式，这与当前需求兼容（因为你已明确“不需要消息包最后一笔”）。

因此本轮设计建议：

- `MarketDataProcessor::handleOrderData(...)`：逐条 `processOrder(order)`
- `MarketDataProcessor::handleTransactionData(...)`：逐条 `processTransaction(trans)`

不需要新增包级接口。

---

## 9. `StockDataManager` 需要新增的状态（卖侧版本）

建议新增到 `predict_withdraw/stock_data_manager.h` 私有区：

```cpp
bool        m_flagOrderInitialized = false;  // 是否已初始化flag_order（卖侧）
OrderIdType m_flagOrder = 0;                 // 当前卖侧基准委托号
int64_t     m_sumAmountRaw = 0;              // 累计金额（raw: price_raw * volume）
int         m_triggerCount50w = 0;           // 触发次数
```

建议常量（`cpp` 匿名命名空间）：

```cpp
constexpr int kStartTime0930 = 93000000;                    // HHMMSSmmm
constexpr int64_t kThreshold50wRaw = 500000LL * 10000LL;    // 元 -> raw
```

---

## 10. `StockDataManager` 完整函数逻辑（卖侧版）

## 10.1 辅助判断（建议）

```cpp
bool isSH() const;
bool isSZ() const;
bool isLimitUpRawPrice(int64_t raw_price) const;
inline bool isAfter0930(int t) const { return t >= kStartTime0930; }

void onThresholdHit(OrderIdType current_order_id, int event_time, const char* reason) {
    // 方案A：sum清零
    // 方案B：sum减阈值
    // 同时刷新 flag_order，并打印日志
}
```

## 10.2 `processOrder(const TDF_ORDER& order)`（卖委托新增 + 上海撤单）

职责：

1. 初始化 `flag_order`（09:30 后第一笔涨停价卖委托）
2. 涨停价卖委托新增累计金额
3. 上海撤单扣减（`orderKind=='D'`）

建议伪代码：

```cpp
void StockDataManager::processOrder(const TDF_ORDER& order)
{
    std::lock_guard<std::mutex> lock(m_mutex); // 是否保留锁见第12节

    if (!isAfter0930(order.nTime)) return;

    const bool is_sell = (order.chFunctionCode == 'S');
    if (!is_sell) return;

    // 上海撤单在order流，撤单记录也可能带价格/数量
    const bool is_sh_cancel = isSH() && (order.chOrderKind == 'D');

    // 撤单/新增都限定涨停价
    if (!isLimitUpRawPrice(order.nPrice)) return;

    const OrderIdType order_id = static_cast<OrderIdType>(order.nOrder);
    if (order_id == 0) return;

    // 1) 初始化flag_order：首笔涨停价卖委托（基准本身不计入sum）
    if (!m_flagOrderInitialized && !is_sh_cancel) {
        m_flagOrder = order_id;
        m_flagOrderInitialized = true;
        m_sumAmountRaw = 0;
        return;
    }

    if (!m_flagOrderInitialized) return;

    const int64_t delta = static_cast<int64_t>(order.nPrice) * static_cast<int64_t>(order.nVolume);
    if (delta <= 0) return;

    // 2) 上海撤单扣减（只扣flag之后）
    if (is_sh_cancel) {
        if (order_id > m_flagOrder) {
            m_sumAmountRaw = (m_sumAmountRaw > delta) ? (m_sumAmountRaw - delta) : 0;
        }
        return;
    }

    // 3) 普通卖委托新增（只统计flag之后）
    if (order_id > m_flagOrder) {
        m_sumAmountRaw += delta;

        if (m_sumAmountRaw >= kThreshold50wRaw) {
            // 方案A：m_sumAmountRaw = 0;
            // 方案B：m_sumAmountRaw -= kThreshold50wRaw;
            m_flagOrder = order_id;
            ++m_triggerCount50w;
            // print
        }
    }
}
```

## 10.3 `processTransaction(const TDF_TRANSACTION& trans)`（成交扣减 + 深圳撤单）

职责：

1. 成交扣减（卖侧，比较 `askOrder > flag_order`）
2. 深圳撤单扣减（`functionCode=='C'`）

建议伪代码：

```cpp
void StockDataManager::processTransaction(const TDF_TRANSACTION& trans)
{
    std::lock_guard<std::mutex> lock(m_mutex); // 是否保留锁见第12节

    if (!m_flagOrderInitialized) return;
    if (!isAfter0930(trans.nTime)) return;

    const OrderIdType ask_order = (trans.nAskOrder > 0)
        ? static_cast<OrderIdType>(trans.nAskOrder) : 0;
    if (ask_order == 0) return;
    if (ask_order <= m_flagOrder) return;

    const bool is_sz_cancel = isSZ() && (trans.chFunctionCode == 'C');
    const bool is_trade = !is_sz_cancel; // SH trade functionCode/orderKind常为空，仍按成交流处理

    int64_t price_raw = 0;

    if (is_sz_cancel) {
        // 深圳撤单在trade流里，样本显示tradePrice常为0/空
        // 本策略统计“涨停价位”撤单，直接使用涨停价raw作为扣减价格
        price_raw = static_cast<int64_t>(std::llround(m_limitUpPrice * 10000.0));
        if (price_raw <= 0) return;
    } else {
        // 成交必须是涨停价成交
        if (!isLimitUpRawPrice(trans.nPrice)) return;
        price_raw = static_cast<int64_t>(trans.nPrice);
    }

    const int64_t delta = price_raw * static_cast<int64_t>(trans.nVolume);
    if (delta <= 0) return;

    // 卖侧成交/深圳卖侧撤单扣减
    m_sumAmountRaw = (m_sumAmountRaw > delta) ? (m_sumAmountRaw - delta) : 0;
}
```

---

## 11. 行情统一分发实现草图（按 `示范.md`）

### 11.1 `MarketDataProcessor::handleOrderData(...)`

```cpp
void MarketDataProcessor::handleOrderData(const TdfMsgData& data)
{
    const TDF_MSG& msg = data.msg;
    if (msg.pAppHead == nullptr || msg.pData == nullptr) return;

    const TDF_APP_HEAD* pAppHead = msg.pAppHead;
    TDF_ORDER* pOrders = static_cast<TDF_ORDER*>(msg.pData);
    int itemCount = pAppHead->nItemCount;

    for (int i = 0; i < itemCount; ++i) {
        const TDF_ORDER& order = pOrders[i];
        auto* mgr = StockDataManagerFactory::getInstance().getStockManager(order.szWindCode);
        if (mgr) mgr->processOrder(order);
    }
}
```

### 11.2 `MarketDataProcessor::handleTransactionData(...)`

```cpp
void MarketDataProcessor::handleTransactionData(const TdfMsgData& data)
{
    const TDF_MSG& msg = data.msg;
    if (msg.pAppHead == nullptr || msg.pData == nullptr) return;

    const TDF_APP_HEAD* pAppHead = msg.pAppHead;
    TDF_TRANSACTION* pTrans = static_cast<TDF_TRANSACTION*>(msg.pData);
    int itemCount = pAppHead->nItemCount;

    for (int i = 0; i < itemCount; ++i) {
        const TDF_TRANSACTION& trans = pTrans[i];
        auto* mgr = StockDataManagerFactory::getInstance().getStockManager(trans.szWindCode);
        if (mgr) mgr->processTransaction(trans);
    }
}
```

---

## 12. `processTransaction(...)` 有必要加锁吗（你问的点）

结论先说：

- **当前建议保留加锁**（`processOrder` / `processTransaction` 都加）

### 12.1 为什么从“行情分发线程”角度看似乎可以不加锁

如果只看 TDF 行情链路：

- `MarketDataProcessor` 只有一个消费线程
- 对同一个 `StockDataManager` 的 `processOrder/processTransaction` 调用是串行的

在这个前提下，单纯写这几个状态变量确实可以不加锁。

### 12.2 但在当前类设计里，仍建议加锁的原因

`StockDataManager` 不是只被一个线程访问，类里已有很多并发访问迹象：

- `getT1()/getBuyOrderVolume()/isLimitUp()` 本身就用了 `m_mutex`
- `calcRemainingVol/calcSpeed/calcRemainingTime/reset` 也会访问同一批状态
- 后续若快照更新涨停价、策略线程读取统计值，都可能并发触达

如果 `processTransaction` 不加锁，而其它线程在遍历/读取相关容器或状态：

- 可能产生数据竞争（UB）
- 可能出现读到中间态

### 12.3 性能角度怎么折中

建议策略：

1. 先保留锁，保证正确性
2. 加入轻量监控（队列长度、处理耗时）
3. 若确认锁是瓶颈，再做“单线程所有权化”重构（统一只在一个线程读写）

### 12.4 一个额外注意点（当前代码已有潜在竞态）

`StockDataManagerFactory::updateLimitupPrice(...)` 当前直接写 `it->second->m_limitUpPrice`，没有使用 `StockDataManager` 内锁。  
如果未来行情线程运行时同时更新涨停价，这里会有竞态风险。

更稳妥做法（后续）：

- 提供 `StockDataManager::setLimitUpPrice(...)` 并在内部加锁
- 或统一在行情处理线程内更新涨停价（来自快照）

---

## 13. 日志打印建议（卖侧版本）

建议前缀：

```text
[LIMITUP_SELL_50W]
```

建议字段：

- `code`
- `time`
- `reason`（`order_add` / `sh_cancel` / `sz_cancel` / `trade`）
- `flag_order_old`
- `flag_order_new`
- `sum_before`
- `sum_after`
- `trigger_count`

示例：

```text
[LIMITUP_SELL_50W] code=600383.SH time=093015120 reason=order_add flag_order_old=123450 flag_order_new=123999 sum_before=6123000000 sum_after=0 trigger=3
```

---

## 14. 本轮实现建议（按你现在的问题排序）

1. 先确认“sum刷新”采用 `清零` 还是 `减阈值`
2. 再把设计按卖侧逻辑落到 `processOrder/processTransaction`
3. 启用/补齐快照涨停价链路（否则只能继续依赖启动时查询价）
4. 增加队列积压监控（至少日志打印 queue size 或处理耗时）

