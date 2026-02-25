#ifndef ORDERMANAGER_WITHDRAW_H
#define ORDERMANAGER_WITHDRAW_H

#include "stdafx.h"
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

#include <queue>
#include <memory>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <cstdint>
#include <cstdio>

class TradeReturnMonitor;

enum class LimitUpTriggerType : uint8_t {
    PRICE_107 = 1,
    SELL_SUM_50W = 2,
    SEALED_STOP = 3,
};

struct LimitUpTrigger {
    LimitUpTriggerType type = LimitUpTriggerType::SELL_SUM_50W;
    std::string symbol;               // "600000.SH" / "000001.SZ"
    int event_time = 0;               // 交易所时间 (HHMMSSmmm)，来自TDF nTime
    int64_t limitup_raw = 0;          // 涨停价 raw (*10000)
    int64_t base_raw = 0;             // 基准价 raw (*10000)，由涨停价倒推
    int64_t tick_raw = 0;             // 逐笔成交价 raw (*10000)，PRICE_107触发时填充
    int64_t signal_steady_ns = 0;     // 触发被识别时的本机steady时间(ns)
    int trigger_count_50w = 0;        // 卖侧累计50万触发次数（仅SELL_SUM_50W触发时有效）
};

// 可撤委托数据结构
struct OrderWithdraw {
    int64 CXOrderId;
    int64 OrderId;               // 委托号
    std::string Market;             // 交易所
    std::string StockCode;         // 证券代码
    std::string AccountId;         // 客户号
    std::string SecuAccount;       // 股东号
    int64 OrderQty;              // 委托数量
    int64 TotalMatchQty;         // 总成交数量
    int32_t OrderStatus;           // 申报状态
    // 可根据需要添加其他字段

    // 从stStructMsg构造
    OrderWithdraw(const stStructMsg& msg)
        :
        CXOrderId(msg.CXOrderId),
        OrderId(msg.OrderId),
        Market(msg.Market),
        StockCode(msg.StockCode),
        AccountId(msg.AccountId),
        SecuAccount(msg.SecuAccount),
        OrderQty(msg.OrderQty),
        TotalMatchQty(msg.TotalMatchQty),
        OrderStatus(msg.OrderStatus) {}
};

class OrderManagerWithdraw
{
public:
    OrderManagerWithdraw();
    ~OrderManagerWithdraw();

    // 启动工作线程
    void startWorkerThread();
    // 停止工作线程
    void stopWorkerThread();

    // 获取指定证券的可撤委托ID列表
    // 参数: stockCode-证券代码, outIds-输出的委托ID向量
    // 返回: 找到的可撤委托数量
    int64 getRevocableOrderIds(const std::string& stockCode, std::vector<OrderWithdraw>& outIds);

    // 更新可撤订单列表
    void updateRevocableOrders(const stStructMsg& msg);

    void set_trade_return_monitor(TradeReturnMonitor* monitor) { m_trade_return_monitor = monitor; }

    // 配置交易账户信息（用于本策略下单/撤单）
    void set_trading_account_info(const std::string& khh,
                                  const std::string& sh_gdh,
                                  const std::string& sz_gdh);

    // 行情侧触发事件投递（线程安全）。忙时会被覆盖式抑制（不入队）。
    void post_limitup_trigger(LimitUpTrigger trigger);

    static OrderManagerWithdraw * get_order_manager_withdraw();

private:
    // 静态回调函数
    static void staticAsyncCallback(const char* pTime, stStructMsg &stMsg, int nType);
    // 处理消息的实际函数
    void handleMessage(const char* pTime, const stStructMsg &stMsg, int nType);
    // 工作线程函数
    void workerThread();
    // 判断订单是否可撤
    bool isOrderRevocable(const stStructMsg& msg);

    // ====== 涨停卖单策略：回报驱动状态机（在workerThread内串行推进） ======
    struct LimitUpOrderState {
        enum Phase : uint8_t {
            IDLE = 0,
            WAIT_SEND = 1,       // 已接纳触发，等待workerThread处理并发送新单（覆盖式：busy不排队）
            WAIT_NEW_ACK = 2,    // 已发送新单，等待委托确认回报
            WAIT_CANCEL_ACK = 3, // 已发送撤单，等待撤单确认回报
            STOPPED = 4,         // 封板后停止
        };

        Phase phase = IDLE;
        bool stop_after_done = false;
        uint32_t seq = 0;
        uint32_t suppressed_while_busy = 0;

        int64_t active_sys_id = 0;      // 上一笔已确认的涨停卖单sys_id
        int64_t pending_sys_id = 0;     // 新发卖单sys_id（等待确认）
        int64_t to_cancel_sys_id = 0;   // 待撤的上一笔sys_id
        int cancel_attempts = 0;
        int64_t last_cancel_send_ns = 0;

        // 当前闭环上下文（用于time_spend.log原始信息）
        std::string reason;
        int trigger_nTime = 0;
        int64_t signal_steady_ns = 0;
        int64_t send_steady_ns = 0;
        int64_t limitup_raw = 0;
        int64_t base_raw = 0;
        int64_t tick_raw = 0;
        int trigger_count_50w = 0;
    };

    static int64_t steady_now_ns();
    static std::string normalize_symbol(const std::string& symbol);
    static bool split_symbol(const std::string& symbol, std::string& out_code, std::string& out_market);
    static void sanitize_csv_field(char* dst, size_t dst_len, const char* src);

    void ensure_time_spend_log_open();
    void time_spend_write_line(const char* line);

    void handle_limitup_trigger(const LimitUpTrigger& trigger);
    void handle_limitup_trade_msg(const char* pTime, const stStructMsg& stMsg, int nType);
    void handle_limitup_timeouts();
    bool send_limitup_sell_order(const std::string& symbol, LimitUpOrderState& st);
    bool send_cancel_order(const std::string& symbol, LimitUpOrderState& st, int64_t target_sys_id);

private:
    // 线程同步相关
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_workerThread;
    std::atomic<bool> m_isRunning;
    std::queue<std::tuple<std::string, stStructMsg, int>> m_msgQueue;
    std::queue<LimitUpTrigger> m_limitupTriggerQueue;

    // 可撤委托存储容器
    std::vector<OrderWithdraw> m_revocableOrders;

    TradeReturnMonitor* m_trade_return_monitor = nullptr;

    // 交易账户信息（运行期由main配置）
    std::string khh_;
    std::string sh_gdh_;
    std::string sz_gdh_;

    // 涨停卖单策略状态（worker线程串行访问）
    std::unordered_map<std::string, LimitUpOrderState> limitup_states_;

    // 离线耗时原始信息输出文件（worker线程写）
    FILE* time_spend_fp_ = nullptr;
};

extern OrderManagerWithdraw * orderManagerWithdraw;

#endif // ORDERMANAGER_WITHDRAW_H
