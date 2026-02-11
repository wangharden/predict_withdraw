#include "spdlog_api.h"
#include <spdlog/sinks/daily_file_sink.h>
#include <thread>
#include <sstream>
#include "utility_functions.h"

std::shared_ptr<spdlog::logger> s_spLogger = nullptr;

void create_logger(std::string logger_name,std::string logger_file_name_prefix)
{
    if (!s_spLogger)
    { // 确保仅初始化一次（多实例时避免重复创建）
        //mid 会自动创建以下文件，确保每天一个，logFilename 之后的两个参数是指定每日新创建日志的时间点

        s_spLogger = spdlog::daily_logger_mt(logger_name, logger_file_name_prefix, 0, 0);
        spdlog::set_default_logger(s_spLogger);
        spdlog::set_level(spdlog::level::info); // 设置日志级别 	>= 设置级别的日志才会被记录

        // 获取并记录日志文件名
        if (auto sink = std::dynamic_pointer_cast<spdlog::sinks::daily_file_sink_mt>(s_spLogger->sinks()[0])) {
            std::string filename = sink->filename();

            s_spLogger->info("当前线程id为: {}",thread_id());
            s_spLogger->info("当前线程id为: {},------------------------------------------程序启动-----------------------------------------",thread_id());
            s_spLogger->info("当前线程id为: {},程序日志文件打开成功: {}",thread_id(), filename);
        } else {
            s_spLogger->error("当前线程id为: {},无法获取日志文件信息",thread_id());
        }

        s_spLogger->flush();;
    }
}
