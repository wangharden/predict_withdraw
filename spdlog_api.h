#ifndef SPDLOG_API_H
#define SPDLOG_API_H

#include "spdlog/spdlog.h"

extern std::shared_ptr<spdlog::logger> s_spLogger;
void create_logger(std::string logger_name,std::string logger_file_name_prefix);

#endif // SPDLOG_API_H
