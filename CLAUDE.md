# CLAUDE.md — 项目开发约束

本文件供 AI 编程助手（Claude Code / GitHub Copilot 等）和开发者快速了解项目约定。
修改代码前请先通读本文件。

---

## 项目概述

**predict_withdraw** — A 股涨停板卖侧委托/撤单策略系统。

核心功能：监控白名单股票的涨停价卖侧逐笔数据，累计金额达阈值（50 万）或成交价突破 1.07 倍基准价时，
自动挂出 100 股涨停价卖单并在新单确认后撤掉上一笔旧单（先挂后撤闭环）。

---

## 构建

- **目标环境**：Linux（`gcc 4.8+`，C++14）；Windows 下用 MSVC (VS 2022) 交叉开发验证
- **构建系统**：CMake ≥ 3.10
- **C++ 标准**：C++14（CMakeLists.txt 中应为 `CMAKE_CXX_STANDARD 14`，目前标注为 11 待修正）
- **构建命令**：
  ```bash
  # Linux
  mkdir -p build && cd build && cmake .. && make -j

  # Windows (VS 2022)
  cmake -S . -B build -G "Visual Studio 17 2022"
  cmake --build build --config Debug --target limitUpHittingMarket
  ```

---

## 架构与线程模型

```
TDF SDK 回调线程（单线程，SPSC 假设）
    │  MsgQueue::push()  ← lock-free SPSC ring buffer
    ▼
MsgQueue（65536 slots × 1024B，预分配）
    │  MsgQueue::pop()
    ▼
MarketDataProcessor 处理线程（单线程串行）
    │  StockDataManagerFactory::getStockManager(code)
    │  → processOrder / processTransaction / processMarketData
    │  → onSellSumThresholdHit() → post_limitup_trigger()
    ▼
OrderManagerWithdraw worker 线程（单线程驱动状态机）
    │  SECITPDK 下单 / 撤单
    ▼
TradeReturnMonitor（接收回报，回调到 OrderManagerWithdraw）
```

### 关键约束

- **MsgQueue 是 SPSC**：单生产者（TDF 回调线程）、单消费者（MarketDataProcessor）。
  不要在多线程中并发调用 `push()`。
- **StockDataManagerFactory** 在 `init_factory()` 后视为只读（白名单模式下不再创建新 manager），
  `getStockManager()` 运行期跳过 mutex。
- **OrderManagerWithdraw 状态机单线程推进**：所有状态转换在 `workerThread()` 中串行执行。

---

## 代码规范

### 命名

- 类名：`PascalCase`（`StockDataManager`, `MsgQueue`）
- 成员变量：`m_camelCase`（`m_flagOrder`, `m_sumAmountRaw`）
- 方法：`camelCase`（`processOrder`, `setWhitelist`）
- 常量：`kCamelCase` 或 `UPPER_SNAKE`（`kCapacity`, `kThreshold50wRaw`）
- 文件名：`snake_case.cpp/h`（`stock_data_manager.cpp`）

### 价格与金额

- **所有价格使用 raw 整数**（`price * 10000`），避免浮点误差
- 金额 raw = `price_raw * volume`
- 阈值 50 万 raw = `500000LL * 10000LL`
- 1.07 触发用整数比较：`tick_raw * 100 > base_raw * 107`

### 日志

- 日志库：spdlog（通过 `spdlog_api.h` 中的全局 `s_spLogger`）
- 前缀规范：
  - `[LIMITUP_SELL_50W]` — 50 万触发
  - `[LUP_ORDER_SEND/ACK]` — 下单 / 确认
  - `[LUP_CANCEL_SEND/ACK]` — 撤单 / 确认
  - `[MSG_QUEUE_STATS]` — 队列统计
  - `[MSG_QUEUE_DROP]` — 丢包告警
- 热路径避免 flush；关键节点（下单/撤单/触发）可 flush

### 线程安全

- `StockDataManager` 内部有 `m_mutex`，当前保留（即使处理线程是单线程），防止与外部读取竞争
- 跨线程共享的原子变量使用 `std::atomic`，优先用 `memory_order_relaxed` 除非需要同步保证
- **绝不在行情回调线程中调用交易 API**

### 沪深差异（重要）

- **上海撤单**：在 ORDER 流，`chOrderKind == 'D'`
- **深圳撤单**：在 TRANSACTION 流，`chFunctionCode == 'C'`
- 成交方向：`TDF_TRANSACTION.nBSFlag`（`'B'`/`'S'`），**不是** `chFunctionCode`

---

## 目录结构

```
predict_withdraw/
├── CLAUDE.md               ← 本文件（开发约束）
├── CMakeLists.txt           ← 构建配置
├── main.cpp                 ← 入口：初始化顺序 = 日志→配置→白名单→Factory→交易→回报→涨停价→队列过滤→行情
├── msg_queue.h/cpp          ← SPSC ring buffer + 白名单源头过滤
├── market_data_api.h/cpp    ← TDF SDK 连接 & 回调
├── market_data_processor.h/cpp ← 消费线程：分发 ORDER/TRANSACTION/MARKET
├── stock_data_manager.h/cpp ← per-symbol 行情状态 & 触发逻辑
├── stock_data_manager_factory.h/cpp ← symbol→manager 映射（启动后只读）
├── order_manager_withdraw.h/cpp ← 回报驱动的 per-symbol 状态机
├── trade_return_monitor.h/cpp ← 交易回报接收 & 白名单过滤
├── trader_api.h/cpp         ← SECITPDK 交易连接 & 登录
├── settings_manager.h/cpp   ← 配置 & 白名单加载
├── utility_functions.h/cpp  ← 工具函数
├── time_span_manager.h/cpp  ← 时间段管理（暂未使用）
├── spdlog_api.h/cpp         ← 日志封装
├── stdafx.h                 ← 预编译头
├── account.ini              ← 账户配置（不入库）
├── itpdk.ini                ← ITPDK 配置（不入库）
├── white_list.txt           ← 白名单股票列表
├── tools/                   ← 辅助工具（tdf_slot_probe）
├── include/                 ← 第三方头文件（TDF SDK, SECITPDK, spdlog, rapidjson, zmq）
├── lib/                     ← 第三方库文件
├── server_logs/             ← 运行日志（.gitignore）
└── docs/                    ← 设计文档 & 参考资料
    ├── design-v0-50w累计功能设计.md     ← 初版设计：50 万累计打印功能
    ├── design-v1-委托撤单功能设计.md     ← 第 1 轮：委托/撤单状态机设计
    ├── design-v2-方案AB与队列性能优化.md ← 第 2 轮：flag_order 延迟更新 + SPSC 队列
    ├── 字段说明.md                      ← 沪深逐笔数据字段差异对照
    ├── 延迟计算思路.md                  ← 锁延迟 cycle 级分析
    ├── 构建修复.md                      ← MSVC 构建修复记录
    └── 代码示范-逐条分发模式.md          ← 分发模式参考代码
```

---

## 已知待修复项（不阻塞运行）

1. ~~`TradeReturnMonitor::filter_by_whitelist_` 从未设为 `true`，白名单过滤不生效~~ ✅ design-v3 已修复
2. ~~`orderManagerWithdraw` 全局指针在 `main()` 前初始化，logger 可能尚未就绪~~ ✅ design-v3 已修复
3. ~~`get_codes_string()` 返回值以 `;` 开头，可能影响 TDF 订阅~~ ✅ design-v3 已修复
4. ~~`m_limitUpPrice` 为 public 且被 Factory 直接赋值，绕过内部 mutex~~ ✅ design-v3 已修复
5. `m_orderHistory` / `m_transHistory` 从未填充（死代码）
6. CMakeLists.txt 标准应从 C++11 改为 C++14
7. `main.cpp` 无优雅退出逻辑

---

## 设计文档索引

按开发顺序阅读：

1. [design-v0](docs/design-v0-50w累计功能设计.md) — 50 万累计打印的初始设计与沪深字段验证
2. [design-v1](docs/design-v1-委托撤单功能设计.md) — 委托/撤单状态机、回报驱动闭环、密集触发节流
3. [design-v2](docs/design-v2-方案AB与队列性能优化.md) — flag_order 延迟更新（方案 B）、SPSC ring buffer（方案 C）、Factory 只读化
4. [字段说明](docs/字段说明.md) — TDF 沪深逐笔 ORDER/TRANSACTION 字段差异
5. [design-v3](docs/design-v3-已知缺陷修复.md) — 白名单过滤、全局指针延迟初始化、前导分号、m_limitUpPrice 竞争修复
