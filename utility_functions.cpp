#include "utility_functions.h"
#include <thread>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

#include "secitpdk/secitpdk.h"
#include "spdlog_api.h"

std::string SECITPDK_GetLastError()
{
    char msg[256];
    SECITPDK_GetLastError(msg);            //获取错误信息
    return msg;
}

std::string thread_id() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();  // 利用标准库对 thread::id 的输出支持
    return oss.str();
}

// 获取当天日期
std::string getCurrentDateString()
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

#ifdef _WIN32
    std::tm tm;
    localtime_s(&tm, &now_c);  // Windows安全版本
#else
    std::tm tm;
    localtime_r(&now_c, &tm);  // POSIX安全版本
#endif

    // 缓冲区长度调整为11（10个有效字符 + 1个字符串结束符'\0'）
    char buffer[11];
    // 格式化字符串改为"%Y-%m-%d"，对应YYYY-MM-DD格式
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm))
    {
        return std::string(buffer);
    }
    return "";
}

// 工具函数：将时间字符串转换为 std::chrono::time_point 对象（类型安全）
std::chrono::system_clock::time_point timeStrToTimePoint(const std::string& time_str)
{
    int64_t timestamp_ms = timeStrToTimestampMs(time_str);
    // 从毫秒级时间戳构造 time_point
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms));
}

std::string gbk_to_utf8(const std::string& gbk_str) {
#ifdef _WIN32
    // Windows 实现：使用 Windows API
    if (gbk_str.empty()) return "";
    
    // GBK -> Wide char (UTF-16)
    int wide_len = MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, nullptr, 0);
    if (wide_len == 0) {
        s_spLogger->error("当前线程id为: {},MultiByteToWideChar 失败", thread_id());
        return "";
    }
    
    std::wstring wide_str(wide_len, 0);
    MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, &wide_str[0], wide_len);
    
    // Wide char (UTF-16) -> UTF-8
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len == 0) {
        s_spLogger->error("当前线程id为: {},WideCharToMultiByte 失败", thread_id());
        return "";
    }
    
    std::string utf8_str(utf8_len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, &utf8_str[0], utf8_len, nullptr, nullptr);
    
    // 移除末尾的 null 字符
    if (!utf8_str.empty() && utf8_str.back() == '\0') {
        utf8_str.pop_back();
    }
    
    return utf8_str;
#else
    // Linux 实现：使用 iconv
    // 创建转换句柄（from_code, to_code）
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1) {
        s_spLogger->error("当前线程id为: {},iconv_open 失败",thread_id());
        return "";
    }

    // 输入缓冲区（注意：iconv 会修改指针，需拷贝原始数据）
    char* in_buf = const_cast<char*>(gbk_str.c_str());
    size_t in_len = gbk_str.size();

    // 输出缓冲区（UTF-8 最多占 3 字节/字符，预留足够空间）
    size_t out_len = in_len * 3 + 1;
    char* out_buf = new char[out_len];
    std::memset(out_buf, 0, out_len);
    char* out_ptr = out_buf;

    // 执行转换
    if (iconv(cd, &in_buf, &in_len, &out_ptr, &out_len) == (size_t)-1) {
        s_spLogger->error("当前线程id为: {},转换失败",thread_id());
        delete[] out_buf;
        iconv_close(cd);
        return "";
    }

    std::string utf8_str(out_buf);
    delete[] out_buf;
    iconv_close(cd);  // 释放句柄
    return utf8_str;
#endif
}

// 工具函数：将 "YYYY-MM-DD HH:mm:ss.SSS" 字符串转换为毫秒级时间戳（int64_t）
int64_t timeStrToTimestampMs(const std::string& time_str) {
    // 1. 拆解时间字符串（年、月、日、时、分、秒、毫秒）
    int year, month, day, hour, minute, second, millisecond;
    char delimiter; // 用于匹配分隔符（-、:、.、空格）
    std::istringstream iss(time_str);
    iss >> year >> delimiter >> month >> delimiter >> day
        >> hour >> delimiter >> minute >> delimiter >> second
        >> delimiter >> millisecond;

    // 2. 转换为 tm 结构体（C 风格时间结构）
    std::tm tm_time = {};
    tm_time.tm_year = year - 1900;  // tm_year 是从 1900 年开始的偏移量
    tm_time.tm_mon = month - 1;     // tm_mon 范围 [0,11]
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = minute;
    tm_time.tm_sec = second;

    // 3. 转换为 time_t（秒级时间戳）
    std::time_t sec_timestamp = std::mktime(&tm_time);
    if (sec_timestamp == -1) {
        throw std::invalid_argument("无效的时间字符串：" + time_str);
    }

    // 4. 转换为毫秒级时间戳（秒级 + 毫秒）
    return static_cast<int64_t>(sec_timestamp) * 1000 + millisecond;
}
