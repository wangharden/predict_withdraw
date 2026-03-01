# design-v3 — 已知缺陷修复（4 项）

> 日期：2026-03-01
> 前置：design-v2（方案 AB 与队列性能优化）之后的补丁轮

---

## 概述

本轮修复 CLAUDE.md 待修复列表中的 4 项代码缺陷，均为非功能回归但影响正确性或稳定性的问题。

---

## 修复 1：TradeReturnMonitor 白名单过滤不生效

### 现状

- `filter_by_whitelist_` 初始化为 `false`，且构造函数与所有方法中均未将其设为 `true`。
- `is_watch_stock()` 因此总是返回 `true`，白名单形同虚设。

### 方案

在构造函数中，若 `watch_codes_` 非空则将 `filter_by_whitelist_` 设为 `true`。

```cpp
// trade_return_monitor.cpp — 构造函数末尾
if (!watch_codes_.empty()) {
    filter_by_whitelist_ = true;
}
```

### 影响

- 白名单为空时行为不变（监控全部股票）。
- 白名单非空时 `is_watch_stock()` 开始生效，仅匹配白名单股票。

---

## 修复 2：orderManagerWithdraw 全局指针静态初始化段错误风险

### 现状

```cpp
// order_manager_withdraw.cpp
OrderManagerWithdraw * orderManagerWithdraw = OrderManagerWithdraw::get_order_manager_withdraw();
```

- 此全局变量在 `main()` 之前由 C++ 静态初始化执行。
- `get_order_manager_withdraw()` 内部调用 `startWorkerThread()`，worker 线程若输出日志，此时 `s_spLogger` 尚未初始化（为空指针），会导致段错误。

### 方案

将全局指针初始化改为 `nullptr`，延迟到 `main()` 中日志初始化之后再调用 `get_order_manager_withdraw()`。

```cpp
// order_manager_withdraw.cpp
OrderManagerWithdraw * orderManagerWithdraw = nullptr;  // 延迟初始化

// main.cpp — 在 create_logger() 之后
orderManagerWithdraw = OrderManagerWithdraw::get_order_manager_withdraw();
```

### 影响

- 确保 worker 线程启动时 `s_spLogger` 已就绪。
- `orderManagerWithdraw` 全局指针仍在 `main()` 中完成交易账户配置之前就绑定，使用顺序不变。

---

## 修复 3：get_codes_string() 返回值以 `;` 开头

### 现状

```cpp
std::string SettingsManager::get_codes_string() const {
    std::string stock_codes_string;
    for (const auto& stock_code : stock_codes) {
        stock_codes_string += ";" + stock_code;
    }
    return stock_codes_string;  // => ";600383.SH;000001.SZ;..."
}
```

- TDF SDK 的订阅接口以 `;` 分隔股票代码，前导 `;` 会产生空串，可能导致订阅失败或异常。

### 方案

使用索引判断，首个元素不加前导 `;`：

```cpp
std::string SettingsManager::get_codes_string() const {
    std::string stock_codes_string;
    stock_codes_string.reserve(stock_codes.size() * 10);
    for (size_t i = 0; i < stock_codes.size(); ++i) {
        if (i > 0) stock_codes_string += ';';
        stock_codes_string += stock_codes[i];
    }
    return stock_codes_string;  // => "600383.SH;000001.SZ;..."
}
```

### 影响

- 仅影响返回值格式，无逻辑变更。

---

## 修复 4：m_limitUpPrice 数据竞争

### 现状

- `m_limitUpPrice` 为 `public`，`StockDataManagerFactory::updateLimitupPrice()` 在 `main()` 线程中直接赋值：
  ```cpp
  it->second->m_limitUpPrice = zqdmRecord.HighLimitPrice;
  ```
- `StockDataManager::processMarketData()` 在处理线程中持 `m_mutex` 写入同一变量。
- `getLimitUpPriceRaw()` 在处理线程中持 `m_mutex` 读取。
- Factory 的赋值绕过了 `m_mutex`，与处理线程的读/写存在数据竞争。

### 方案

1. 将 `m_limitUpPrice` 从 `public` 移到 `private`。
2. 增加 `public` setter：`setLimitUpPrice(double price)`，内部加锁。
3. `StockDataManagerFactory::updateLimitupPrice()` 改用 setter。

```cpp
// stock_data_manager.h
public:
    void setLimitUpPrice(double price);
    double getLimitUpPrice() const;
private:
    double m_limitUpPrice;  // 移入 private

// stock_data_manager.cpp
void StockDataManager::setLimitUpPrice(double price) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_limitUpPrice = price;
}

double StockDataManager::getLimitUpPrice() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_limitUpPrice;
}
```

### 影响

- 当前 `updateLimitupPrice()` 仅在启动阶段调用（行情线程尚未启动），实际竞争概率极低，但修复后消除了潜在 UB。
- `processMarketData()` 内部赋值无需修改（已在 `m_mutex` 保护下）。

---

## 附：修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `trade_return_monitor.cpp` | 构造函数末尾设置 `filter_by_whitelist_` |
| `order_manager_withdraw.cpp` | 全局指针改为 `nullptr` |
| `main.cpp` | 日志初始化后调用 `get_order_manager_withdraw()` |
| `settings_manager.cpp` | `get_codes_string()` 消除前导 `;` |
| `stock_data_manager.h` | `m_limitUpPrice` 移入 `private`，新增 setter/getter |
| `stock_data_manager.cpp` | 实现 `setLimitUpPrice()`、`getLimitUpPrice()` |
| `stock_data_manager_factory.cpp` | 改用 `setLimitUpPrice()` 和 `getLimitUpPrice()` |
| `CLAUDE.md` | 更新待修复列表，标记 1-4 已修复 |
