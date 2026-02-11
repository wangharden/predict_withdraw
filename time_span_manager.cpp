#include "time_span_manager.h"

void TimeSpanManager::addTimeSpan(const TimeSpan& logInfo)
{
    m_time_spans.push_back(logInfo);
}

std::vector<TimeSpan> TimeSpanManager::get_time_spans()
{
    return m_time_spans;
}

// 方法1：根据时间点匹配，返回iThresholdToWithdrawS
std::int32_t TimeSpanManager::getWithdrawThreshold(const std::chrono::system_clock::time_point& checkTime)
{
    for (const auto& logInfo : m_time_spans)
    {
        // 判断checkTime是否在[sBegin, sEnd]区间内
        if (checkTime >= logInfo.sBegin && checkTime <= logInfo.sEnd)
        {
            return logInfo.iThresholdToWithdrawS;
        }
    }
    return DEFAULT_THRESHOLD;
}

// 方法2：根据时间点匹配，返回iThresholdToBuyMS
std::int32_t TimeSpanManager::getBuyThreshold(const std::chrono::system_clock::time_point& checkTime)
{
    for (const auto& logInfo : m_time_spans)
    {
        if (checkTime >= logInfo.sBegin && checkTime <= logInfo.sEnd)
        {
            return logInfo.iThresholdToBuyMS;
        }
    }
    return DEFAULT_THRESHOLD;
}
