#ifndef UTILITY_FUNCTIONS_H
#define UTILITY_FUNCTIONS_H

#include <cstring>  // 必须放在第三方头文件之前！
#include <string>
#include <chrono>

std::string thread_id();
std::string gbk_to_utf8(const std::string& gbk_str);

// 获取当天日期
std::string getCurrentDateString();
std::chrono::system_clock::time_point timeStrToTimePoint(const std::string& time_str);

int64_t timeStrToTimestampMs(const std::string& time_str);

std::string SECITPDK_GetLastError();

#endif // UTILITY_FUNCTIONS_H
