#ifndef TIMESPANMANAGER_H
#define TIMESPANMANAGER_H


#include "spdlog/spdlog.h"

struct TimeSpan {
    std::chrono::system_clock::time_point sBegin;
    std::chrono::system_clock::time_point sEnd;
    std::int32_t iThresholdToWithdrawS;
    std::int32_t iThresholdToBuyMS;
};

class TimeSpanManager
{
private:
    // 存储所有LogInfo配置
    std::vector<TimeSpan> m_time_spans;
    // 默认阈值（无匹配时返回）
    std::int32_t DEFAULT_THRESHOLD = 200;

public:
    // 构造函数
    TimeSpanManager() = default;
    ~TimeSpanManager() = default;

    // 添加LogInfo配置到管理器
    void addTimeSpan(const TimeSpan& logInfo);

    std::vector<TimeSpan> get_time_spans();

    // 方法1：根据时间点匹配，返回iThresholdToWithdrawS
    std::int32_t getWithdrawThreshold(const std::chrono::system_clock::time_point& checkTime);

    // 方法2：根据时间点匹配，返回iThresholdToBuyMS
    std::int32_t getBuyThreshold(const std::chrono::system_clock::time_point& checkTime);
};

#endif // TIMESPANMANAGER_H
