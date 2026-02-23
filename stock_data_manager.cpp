#include "stock_data_manager.h"
#include "stock_data_manager_factory.h"
#include "spdlog_api.h"
#include <iostream>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <cmath>
using std::cout;

namespace {
constexpr int kStartTime0930 = 93000000;                      // HHMMSSmmm
constexpr int64_t kThreshold50wRaw = 500000LL * 10000LL;      // 50万（元）换算到 raw price*volume 口径
}

// ===================== StockDataManager 实现 =====================
StockDataManager::StockDataManager(const StockCode& stockCode)
    : m_stockCode(stockCode)
    , m_T1(0)
    , m_buyOrderVolume(0)
    , m_nOrder(0)
    , m_FBOrder(0)
    , m_limitUpPrice(0.0)
    , m_isLimitUp(false)
    , m_lastCalcTime(0)
    , m_flagOrderInitialized(false)
    , m_flagOrder(0)
    , m_sumAmountRaw(0)
    , m_triggerCount50w(0)
{}

bool StockDataManager::isSHMarket() const
{
    return m_stockCode.size() >= 3 && m_stockCode.find(".SH") != std::string::npos;
}

bool StockDataManager::isSZMarket() const
{
    return m_stockCode.size() >= 3 && m_stockCode.find(".SZ") != std::string::npos;
}

int64_t StockDataManager::getLimitUpPriceRaw() const
{
    if (m_limitUpPrice <= 0.0)
    {
        return 0;
    }
    return static_cast<int64_t>(std::llround(m_limitUpPrice * 10000.0));
}

bool StockDataManager::isLimitUpRawPrice(int64_t rawPrice) const
{
    const int64_t limitUpRaw = getLimitUpPriceRaw();
    return limitUpRaw > 0 && rawPrice == limitUpRaw;
}

void StockDataManager::onSellSumThresholdHit(OrderIdType currentOrderId, int eventTime, const char* reason)
{
    OrderIdType oldFlagOrder = m_flagOrder;

    m_flagOrder = currentOrderId;
    m_sumAmountRaw = 0; // 按需求：sum刷新用sum=0
    ++m_triggerCount50w;

    if (s_spLogger)
    {
        s_spLogger->info("[LIMITUP_SELL_50W] code={} time={} reason={} flag_order_old={} flag_order_new={} trigger={}",
            m_stockCode,
            eventTime,
            reason ? reason : "",
            static_cast<unsigned long long>(oldFlagOrder),
            static_cast<unsigned long long>(m_flagOrder),
            m_triggerCount50w);
    }
    else
    {
        std::cout << "[LIMITUP_SELL_50W] code=" << m_stockCode
                  << " time=" << eventTime
                  << " reason=" << (reason ? reason : "")
                  << " flag_order_old=" << oldFlagOrder
                  << " flag_order_new=" << m_flagOrder
                  << " trigger=" << m_triggerCount50w
                  << std::endl;
    }
}

TimeStamp StockDataManager::time_to_no(TimeStamp m_time)
{
    if (m_time < 130000000)
        return m_time/10000000*3600000 + m_time/100000%100*60000 + m_time/1000%100*1000 + m_time%1000;
    else
        return m_time/10000000*3600000 + m_time/100000%100*60000 + m_time/1000%100*1000 + m_time%1000 - 5400000;
}
void StockDataManager::processOrder(const TDF_ORDER& order)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (order.nTime < kStartTime0930)
    {
        return;
    }

    const bool isSell = (order.chFunctionCode == 'S');
    if (!isSell)
    {
        return;
    }

    const bool isShCancel = isSHMarket() && (order.chOrderKind == 'D');

    // 新增卖单和上海撤单都按涨停价位过滤
    if (!isLimitUpRawPrice(order.nPrice))
    {
        return;
    }

    const OrderIdType orderId = (order.nOrder > 0) ? static_cast<OrderIdType>(order.nOrder) : 0;
    if (orderId == 0)
    {
        return;
    }

    // 初始化flag：09:30后首笔涨停价卖委托（不包含撤单）
    if (!m_flagOrderInitialized && !isShCancel)
    {
        m_flagOrder = orderId;
        m_flagOrderInitialized = true;
        m_sumAmountRaw = 0;
        return;
    }

    if (!m_flagOrderInitialized)
    {
        return;
    }

    if (orderId <= m_flagOrder)
    {
        return;
    }

    const int64_t delta = static_cast<int64_t>(order.nPrice) * static_cast<int64_t>(order.nVolume);
    if (delta <= 0)
    {
        return;
    }

    // 上海撤单在order流里扣减
    if (isShCancel)
    {
        m_sumAmountRaw = (m_sumAmountRaw > delta) ? (m_sumAmountRaw - delta) : 0;
        return;
    }

    // 首次封板后终止“涨停价卖单新增累计金额”功能
    if (m_isLimitUp)
    {
        return;
    }

    // 普通卖委托新增累计
    m_sumAmountRaw += delta;
    if (m_sumAmountRaw >= kThreshold50wRaw)
    {
        onSellSumThresholdHit(orderId, order.nTime, "order_add");
    }
}

void StockDataManager::processMarketData(const TDF_MARKET_DATA& md)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (md.nHighLimited > 0)
    {
        m_limitUpPrice = static_cast<double>(md.nHighLimited) / 10000.0;
    }
}

void StockDataManager::processTransaction(const TDF_TRANSACTION& trans)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (trans.nTime < kStartTime0930)
    {
        return;
    }

    // 新需求1：首次完成封板打印
    // 解释：按你描述的“成交价=涨停价 且 function为S 的成交回报”，这里使用成交买卖方向 nBSFlag=='S'
    // （上海逐笔成交 chFunctionCode 常为空，不能用于判断买卖方向）
    const bool isSellTrade = (static_cast<char>(trans.nBSFlag) == 'S');
    if (!m_isLimitUp && isSellTrade && isLimitUpRawPrice(trans.nPrice))
    {
        m_isLimitUp = true;
        m_T1 = trans.nTime;

        const int hhmmss = trans.nTime / 1000;
        if (s_spLogger)
        {
            s_spLogger->info("[LIMITUP_SEAL] {} 股票在 {:06d} 实现封板", m_stockCode, hhmmss);
        }
        else
        {
            std::cout << "[LIMITUP_SEAL] " << m_stockCode << " 股票在 " << hhmmss << " 实现封板" << std::endl;
        }
    }

    if (!m_flagOrderInitialized)
    {
        return;
    }

    const OrderIdType askOrder = (trans.nAskOrder > 0) ? static_cast<OrderIdType>(trans.nAskOrder) : 0;
    if (askOrder == 0 || askOrder <= m_flagOrder)
    {
        return;
    }

    const bool isSzCancel = isSZMarket() && (trans.chFunctionCode == 'C');

    int64_t priceRaw = 0;
    if (isSzCancel)
    {
        // 深圳撤单在trade流里，样本中tradePrice为0；这里按“涨停价位撤单”使用涨停价补价
        priceRaw = getLimitUpPriceRaw();
        if (priceRaw <= 0)
        {
            return;
        }
    }
    else
    {
        // 成交扣减只统计涨停价成交
        if (!isLimitUpRawPrice(trans.nPrice))
        {
            return;
        }
        priceRaw = static_cast<int64_t>(trans.nPrice);
    }

    const int64_t delta = priceRaw * static_cast<int64_t>(trans.nVolume);
    if (delta <= 0)
    {
        return;
    }

    m_sumAmountRaw = (m_sumAmountRaw > delta) ? (m_sumAmountRaw - delta) : 0;
}

VolumeType StockDataManager::calcLast1000msVolume(TimeStamp currentTime)
{
    //std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_isLimitUp || m_T1 == 0)
    {
        return 0;
    }

    TimeStamp startTime = (std::max)(time_to_no(currentTime) - 999, time_to_no(m_T1));
    VolumeType totalVolume = 0;

    // 累加区间内成交量
    for (const auto& trans : m_transHistory)
    {
        if (time_to_no(trans.nTime) >= startTime && trans.nTime <= currentTime)
        {
            if (m_isLimitUp)
            {
                totalVolume += trans.nVolume;
            }
        }
    }

    return totalVolume;
}

double StockDataManager::calcRemainingVol(TimeStamp currentTime)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_isLimitUp || m_T1 == 0)
    {
        return -1.0; // 未封板
    }

    VolumeType remainingVolume = m_buyOrderVolume;
    if (remainingVolume == 0)
    {
        return 0.0; // 无剩余挂单
    }
    return remainingVolume ;
}

double StockDataManager::calcSpeed(TimeStamp currentTime)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_isLimitUp || m_T1 == 0)
    {
        return -1.0; // 未封板
    }

    VolumeType last1000msVol = calcLast1000msVolume(currentTime);
    if (last1000msVol == 0)
    {
        return -2.0; // 无成交，无法计算
    }

    // 计算平均每秒成交量 & 剩余消化时间
    double avgPerMs = (double)last1000msVol;
    return avgPerMs;
}

double StockDataManager::calcLastingTime(TimeStamp currentTime)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_isLimitUp || m_T1 == 0)
    {
        return -1.0; // 未封板
    }

    return time_to_no(currentTime) - time_to_no(m_T1);
}

double StockDataManager::calcRemainingTime(TimeStamp currentTime)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_isLimitUp || m_T1 == 0)
    {
        return -1.0; // 未封板
    }

    VolumeType last1000msVol = calcLast1000msVolume(currentTime);
    if (last1000msVol == 0)
    {
        return 99999; // 无成交，无法计算
    }

    VolumeType remainingVolume = m_buyOrderVolume;
    if (remainingVolume == 0)
    {
        return 0.0; // 无剩余挂单
    }

    // 计算平均每秒成交量 & 剩余消化时间
    //double avgPerMs = (double)last1000msVol ;
    return (double)remainingVolume / (double)last1000msVol;
}

void StockDataManager::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_T1 = 0;
    m_buyOrderVolume = 0;
    m_nOrder = 0;
    m_FBOrder = 0;
    m_isLimitUp = false;
    m_limitUpPrice = 0.0;
    m_flagOrderInitialized = false;
    m_flagOrder = 0;
    m_sumAmountRaw = 0;
    m_triggerCount50w = 0;
    m_orderHistory.clear();
    m_transHistory.clear();
}
