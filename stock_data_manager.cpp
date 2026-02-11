#include "stock_data_manager.h"
#include "stock_data_manager_factory.h"
#include <iostream>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
using std::cout;
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
{}
TimeStamp StockDataManager::time_to_no(TimeStamp m_time)
{
    if (m_time < 130000000)
        return m_time/10000000*3600000 + m_time/100000%100*60000 + m_time/1000%100*1000 + m_time%1000;
    else
        return m_time/10000000*3600000 + m_time/100000%100*60000 + m_time/1000%100*1000 + m_time%1000 - 5400000;
}
void StockDataManager::processOrder(const TDF_ORDER& order)
{
    
}

void StockDataManager::processTransaction(const TDF_TRANSACTION& trans)
{
  
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
    m_orderHistory.clear();
    m_transHistory.clear();
}
