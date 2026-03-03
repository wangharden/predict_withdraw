# design-v5 — 预撤单唤醒延迟修复

> 日期：2026-03-03
> 前置：design-v4（白名单读写优化）之后的补丁轮

---

## 问题描述

### 背景

`post_limitup_trigger()` 在 09:30 后首次触发时，若该股票存在 09:24 预挂单（`pre0924_sys_id > 0`），
会进入 `WAIT_PRE_CANCEL_ACK` 状态，将原触发保存到 `deferred`，等撤单完成后再恢复执行。

### 缺陷

进入 `WAIT_PRE_CANCEL_ACK` 后虽然调用了 `m_cv.notify_one()` 唤醒 worker 线程，但 worker 的
`wait_for` 谓词仅检查 `m_msgQueue` 和 `m_limitupTriggerQueue` 是否非空：

```cpp
m_cv.wait_for(lock, std::chrono::milliseconds(kWorkerWakeupMs), [this] {
    return !m_isRunning.load() || !m_msgQueue.empty() || !m_limitupTriggerQueue.empty();
});
```

由于 `WAIT_PRE_CANCEL_ACK` 路径未向任何队列推送消息，谓词返回 `false`，worker 被虚假唤醒后
重新等待，直到 100ms 超时才继续执行 `handle_limitup_timeouts()` 发送撤单。

**实际影响**：09:24 预挂单的撤单请求延迟约 100ms（一个 `kWorkerWakeupMs` 周期）。

---

## 修复方案

在 `WAIT_PRE_CANCEL_ACK` 路径中，向 `m_limitupTriggerQueue` 推入一个**空标记**
（`LimitUpTrigger{}`，`symbol` 为空字符串），使谓词立即返回 `true`，worker 被唤醒后：

1. 从队列取出空标记，传给 `handle_limitup_trigger()`
2. `handle_limitup_trigger()` 检测到 `trigger.symbol.empty()` 立即返回（已有的保护逻辑）
3. 随后执行 `handle_limitup_timeouts()`，发现 `WAIT_PRE_CANCEL_ACK` 状态且 `last_cancel_send_ns == 0`，立即发送撤单

### 代码变更

```cpp
// order_manager_withdraw.cpp — post_limitup_trigger()
// WAIT_PRE_CANCEL_ACK 分支末尾，accepted = true 之前新增：

// 推入空标记唤醒 worker，使其立即执行 handle_limitup_timeouts() 发送撤单
// handle_limitup_trigger() 对 symbol.empty() 直接返回，不会产生副作用
m_limitupTriggerQueue.push(LimitUpTrigger{});
accepted = true;
```

### 安全性分析

| 关注点 | 分析 |
|--------|------|
| 空标记副作用 | `handle_limitup_trigger()` 首行 `if (trigger.symbol.empty()) return;`，无任何状态变更 |
| 队列膨胀 | 每次预撤单仅推入 1 个空标记，且立即被消费，无累积风险 |
| 谓词正确性 | 空标记使 `!m_limitupTriggerQueue.empty()` 为 true，符合 `wait_for` 语义 |
| 向后兼容 | 不修改 LimitUpTrigger 结构体、不新增状态、不影响其他路径 |

---

## 附：修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `order_manager_withdraw.cpp` | `post_limitup_trigger()` 中 `WAIT_PRE_CANCEL_ACK` 分支推入空标记 |
| `CLAUDE.md` | 新增设计文档索引 design-v5 |
