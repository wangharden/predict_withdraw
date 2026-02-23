#ifndef MARKET_DATA_PROCESSOR_H
#define MARKET_DATA_PROCESSOR_H

#include <thread>
#include <atomic>

#include "msg_queue.h"
#include "trader_api.h"

class MarketDataProcessor {
public:
    MarketDataProcessor();
    ~MarketDataProcessor();

    // 启动/停止数据处理线程
    void startProcessThread(TraderApi &tr, SettingsManager &settings_manager);
    void stopProcessThread();

    // 检查线程是否运行
    bool isThreadRunning() const { return m_threadRunning; }

private:
    // 数据处理线程函数（静态函数，适配 std::thread）
    void processMsgThreadFunc();

    // 业务处理函数：解析 ORDER 数据
    void handleOrderData(const TdfMsgData& data);
    // 业务处理函数：解析 TRANSACTION 数据
    void handleTransactionData(const TdfMsgData& data);
    // 业务处理函数：解析 MARKET 快照数据（用于更新涨停价）
    void handleMarketData(const TdfMsgData& data);

    std::thread m_processThread;       // 处理线程对象
    std::atomic<bool> m_threadRunning; // 线程运行标记（原子变量，线程安全）
    TraderApi m_trader_api;
    SettingsManager m_settings_manager;
};

#endif // MARKET_DATA_PROCESSOR_H
