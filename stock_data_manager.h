#ifndef STOCK_DATA_MANAGER_H
#define STOCK_DATA_MANAGER_H

#include <string>
#include <mutex>
#include <vector>
#include <cstdint>
#include <unordered_map>

#include "TDFAPI.h"
#include "TDCore.h"
#include "TDFAPIInner.h"
#include "TDNonMarketStruct.h"

#include "msg_queue.h"

// 基础类型定义
using TimeStamp = int64_t;               // 微秒级时间戳
using StockCode = std::string;           // 股票代码
using VolumeType = int64_t;             // 数量/成交量类型
using OrderIdType = uint64_t;            // 委托单号类型

// ===================== 单股票数据管理类 =====================
class StockDataManager {
public:
    // 构造函数
    StockDataManager(const StockCode& stockCode);
    // 禁用拷贝
    StockDataManager(const StockDataManager&) = delete;
    StockDataManager& operator=(const StockDataManager&) = delete;
    // 移动构造（默认）
    StockDataManager(StockDataManager&&) = default;
    StockDataManager& operator=(StockDataManager&&) = default;

    // 处理委托数据
    void processOrder(const TDF_ORDER& order);
    // 处理快照数据（更新涨停价）
    void processMarketData(const TDF_MARKET_DATA& md);
    // 处理成交数据
    void processTransaction(const TDF_TRANSACTION& trans);
    // 计算封板后任意时刻过去1000ms总成交量
    VolumeType calcLast1000msVolume(TimeStamp currentTime);
    // 计算剩余挂单消化时间（毫秒），返回-1=未封板，-2=无成交，>=0=有效时间
    double calcRemainingVol(TimeStamp currentTime);
    double calcSpeed(TimeStamp currentTime);
    double calcLastingTime(TimeStamp currentTime);
    bool get_LimitUpStatus() const { return m_isLimitUp; }
    double calcRemainingTime(TimeStamp currentTime);
    TimeStamp time_to_no(TimeStamp m_time);
    // 重置数据（如次日行情/封板失效）
    void reset();

    // 获取核心参数（只读）
    StockCode getStockCode() const { return m_stockCode; }
    TimeStamp getT1() const { std::lock_guard<std::mutex> lock(m_mutex); return m_T1; }
    VolumeType getBuyOrderVolume() const { std::lock_guard<std::mutex> lock(m_mutex); return m_buyOrderVolume; }
    bool isLimitUp() const { std::lock_guard<std::mutex> lock(m_mutex); return m_isLimitUp; }

public:
    double m_limitUpPrice;                // 涨停价

private:
    bool isSHMarket() const;
    bool isSZMarket() const;
    int64_t getLimitUpPriceRaw() const;
    bool isLimitUpRawPrice(int64_t rawPrice) const;
    void onSellSumThresholdHit(OrderIdType currentOrderId, int eventTime, const char* reason);

private:
    const StockCode m_stockCode;          // 股票代码
    TimeStamp m_T1;                       // 首次封板时间（微秒）
    VolumeType m_buyOrderVolume;          // 封板后40ms内委买存量
    OrderIdType m_nOrder;                 // 40ms后首次委托单号
    OrderIdType m_FBOrder;                // 首次封板委托单号
    bool m_isLimitUp;                     // 是否已封板
    TimeStamp m_lastCalcTime;             // 最后计算时间
    bool m_flagOrderInitialized;          // 卖侧flag_order是否已初始化
    OrderIdType m_flagOrder;              // 卖侧基准委托号
    int64_t m_sumAmountRaw;               // 卖侧累计金额（raw: price_raw*volume）
    int m_triggerCount50w;                // 50万阈值触发次数

    std::vector<TDF_ORDER> m_orderHistory;    // 委托历史（用于1000ms统计）
    std::vector<TDF_TRANSACTION> m_transHistory; // 成交历史（用于1000ms统计）
    mutable std::mutex m_mutex;            // 线程安全锁（mutable支持const方法加锁）
};

#endif // STOCK_DATA_MANAGER_H
