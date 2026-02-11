#include "settings_manager.h"
#include "spdlog_api.h"
#include "utility_functions.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include <iostream>

using namespace rapidjson;

bool fileExists(const std::string& path) {
    FILE* file = fopen(path.c_str(), "r");
    if (file) {
        fclose(file);
        return true;
    }
    return false;
}

SettingsManager::SettingsManager()
{
}

bool SettingsManager::load_white_list(const std::string& filename) {
    // 打开文件
    // 打开JSON文件
    FILE* fp = fopen(filename.c_str(), "r");
    if (!fp) {
        s_spLogger->warn("当前线程id为: {},白名单文件不存在或无法打开={}, 将监控所有股票",thread_id(), filename);
        s_spLogger->flush();
        std::cout << "白名单文件不存在，将监控所有股票" << std::endl;
        return true;  // 文件不存在时返回成功，白名单为空表示监控所有
    }

    // 读取文件内容
    char readBuffer[65536];  // 缓冲区
    FileReadStream is(fp, readBuffer, sizeof(readBuffer));

    // 解析JSON
    Document doc;
    doc.ParseStream(is);
    fclose(fp);  // 及时关闭文件

    if (doc.HasParseError()) {
        s_spLogger->error("当前线程id为: {},JSON解析错误：={}",thread_id(), doc.GetParseError() );
        s_spLogger->error("当前线程id为: {},错误位置：={}",thread_id(), doc.GetErrorOffset() );
        s_spLogger->flush();
        return false;
    }

    if (!doc.IsObject()) {
        s_spLogger->error("当前线程id为: {},JSON格式错误，根节点不是对象",thread_id());
        s_spLogger->flush();
        return false;
    }

    // 遍历所有股票代码节点
    for (const auto& member : doc.GetObject()) {
        const std::string stockCode = member.name.GetString();
        const rapidjson::Value& stockObj = member.value;

        // 验证股票对象结构
        if (!stockObj.IsObject()) {
            s_spLogger->error("当前线程id为: {},股票数据格式错误:{}",thread_id(),stockCode);
            s_spLogger->flush();
            continue;
        }

        // 转换字符串为double并存储
        std::string code_sh = stockCode + ".SH";
        //std::cout << "code_sh:" << code_sh << std::endl;
        stock_codes.push_back(code_sh);
    }
    s_spLogger->error("当前线程id为: {},数据加载结束，总共加载白名单股票数:{}",thread_id(),stock_codes.size());

    std::cout << "白名单加载成功:" << stock_codes.size() << std::endl;
    s_spLogger->flush();
    return true;
}

bool SettingsManager::load_account_settings(std::string account_file)
{
    if(!fileExists(account_file))
    {
        s_spLogger->error("当前线程id为: {},the setting file name {} does not exist.",thread_id(),account_file);
        s_spLogger->flush();
        std::cerr << "配置文件未找到" << std::endl;
        return false;
    }

    s_spLogger->info("当前线程id为: {},the setting file name is {}.",thread_id(),account_file);


    if (readAccountConfig(account_file))
    {
        s_spLogger->info("当前线程id为: {},配置读取成功:{}",thread_id(),account_file);
        s_spLogger->flush();
        std::cout << "配置文件读取 成功" << std::endl;
        return true;
    }
    else
    {
        s_spLogger->error("当前线程id为: {},read error:{}",thread_id(),account_file);
        s_spLogger->flush();
        std::cerr << "配置文件读取 失败" << std::endl;
        return false;
    }
}

bool SettingsManager::readAccountConfig(std::string file_name)
{
    // 1. 打开JSON文件
    FILE* fp = fopen(file_name.c_str(), "r");
    if (!fp) {
        s_spLogger->error("当前线程id为: {},无法打开文件={}", thread_id(), file_name);
        return false;
    }

    // 2. 读取文件内容到缓冲区
    char readBuffer[65536];  // 64K缓冲区足够处理配置文件
    FileReadStream is(fp, readBuffer, sizeof(readBuffer));

    // 3. 解析JSON
    Document document;
    document.ParseStream(is);
    fclose(fp);  // 及时关闭文件（RAII风格，避免泄漏）

    // 4. 检查解析错误
    if (document.HasParseError()) {
        s_spLogger->error("当前线程id为: {},JSON解析错误：={}", thread_id(), document.GetParseError());
        s_spLogger->error("当前线程id为: {},错误位置：={}", thread_id(), document.GetErrorOffset());
        return false;
    }

    // 5. 检查根节点是否为对象
    if (!document.IsObject()) {
        s_spLogger->error("当前线程id为: {},JSON格式错误，根节点不是对象", thread_id());
        return false;
    }

    // ===================== 解析 trading 子对象 =====================
    if (document.HasMember("trading") && document["trading"].IsObject()) {
        const Value& trading_obj = document["trading"];

        // 读取 trading.account（字符串类型）
        if (trading_obj.HasMember("sWtfs") && trading_obj["sWtfs"].IsString()) {
            trading_sWtfs = trading_obj["sWtfs"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 trading.account 配置", thread_id());
            return false;
        }

        if (trading_obj.HasMember("sKey") && trading_obj["sKey"].IsString()) {
            trading_sKey = trading_obj["sKey"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 trading.account 配置", thread_id());
            return false;
        }

        // 读取 trading.account（字符串类型）
        if (trading_obj.HasMember("sKhh") && trading_obj["sKhh"].IsString()) {
            trading_sKhh = trading_obj["sKhh"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 trading.account 配置", thread_id());
            return false;
        }

        // 读取 trading.password（字符串类型）
        if (trading_obj.HasMember("sPwd") && trading_obj["sPwd"].IsString()) {
            trading_sPwd = trading_obj["sPwd"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 trading.password 配置", thread_id());
            return false;
        }

        // 读取 trading.config_section（字符串类型）
        if (trading_obj.HasMember("sNode") && trading_obj["sNode"].IsString()) {
            trading_sNode = trading_obj["sNode"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 trading.config_section 配置", thread_id());
            return false;
        }
    } else {
        s_spLogger->error("当前线程id为: {},缺少或错误的 trading 配置节点", thread_id());
        return false;
    }

    // ===================== 解析 market 子对象 =====================
    if (document.HasMember("market") && document["market"].IsObject()) {
        const Value& market_obj = document["market"];

        // 读取 market.host（字符串类型）
        if (market_obj.HasMember("host") && market_obj["host"].IsString()) {
            market_host = market_obj["host"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 market.host 配置", thread_id());
            return false;
        }

        // 读取 market.port（整数类型）
        if (market_obj.HasMember("port") && market_obj["port"].IsInt()) {
            market_port = market_obj["port"].GetInt();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 market.port 配置", thread_id());
            return false;
        }

        // 读取 market.user（字符串类型）
        if (market_obj.HasMember("user") && market_obj["user"].IsString()) {
            market_user = market_obj["user"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 market.user 配置", thread_id());
            return false;
        }

        // 读取 market.password（字符串类型）
        if (market_obj.HasMember("password") && market_obj["password"].IsString()) {
            market_password = market_obj["password"].GetString();
        } else {
            s_spLogger->error("当前线程id为: {},缺少或错误的 market.password 配置", thread_id());
            return false;
        }
    } else {
        s_spLogger->error("当前线程id为: {},缺少或错误的 market 配置节点", thread_id());
        return false;
    }

    // 所有配置项读取成功
    s_spLogger->info("当前线程id为: {},配置文件读取成功！trading.Khh={}, market.user={}",
                     thread_id(), trading_sKhh, market_user);
    return true;
}
