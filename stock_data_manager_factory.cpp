#include "stock_data_manager_factory.h"
#include "stock_data_manager.h"
#include <memory>
#include <iostream>
#include "spdlog_api.h"
#include "utility_functions.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"

#include "secitpdk/secitpdk.h"

using namespace rapidjson;
using std::cout;
// ===================== StockDataManagerFactory 实现 =====================
StockDataManagerFactory& StockDataManagerFactory::getInstance() {
    static StockDataManagerFactory instance;
    return instance;
}

StockDataManager* StockDataManagerFactory::getStockManager(const StockCode& stockCode) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_stockManagerMap.find(stockCode);
    if (it != m_stockManagerMap.end()) {
        return it->second.get();
    }

    // 创建新实例并加入映射
    //auto manager = std::make_unique<StockDataManager>(stockCode);
    std::unique_ptr<StockDataManager> manager(new StockDataManager(stockCode));

    StockDataManager* ptr = manager.get();
    m_stockManagerMap[stockCode] = std::move(manager);
    return ptr;
}

void StockDataManagerFactory::resetStockManager(const StockCode& stockCode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_stockManagerMap.find(stockCode);
    if (it != m_stockManagerMap.end()) {
        it->second->reset();
    }
}

void StockDataManagerFactory::resetAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_stockManagerMap) {
        pair.second->reset();
    }
}

void StockDataManagerFactory::removeStockManager(const StockCode& stockCode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stockManagerMap.erase(stockCode);
}

bool StockDataManagerFactory::init_factory(const std::vector<std::string>& stock_codes)
{
    for(const auto & stock_code:stock_codes)
    {
        getStockManager(stock_code);
    }
    return true;
}

bool StockDataManagerFactory::updateLimitupPrice(const std::string & sKhh)
{
    vector<ITPDK_ZQDM> arZQDM;
    int64 nRet;

    string lpBrowindex = "";
    string lpJys = "";
    string lpZqdm = "";
    //string lpZqdm = "603008.SH;603778.SH";
    string lpZqlb = "";
    int nExeflag = 0;

    s_spLogger->info("开始查询涨停价.");
    //std::cout<<"开始查询涨停价."<<std::endl;
    int32 i = 0;
    do
    {
        s_spLogger->info("第 {} 次查询.",i++);
        //std::cout<<"第 "<<i<<" 次查询."<<std::endl;
        vector<ITPDK_ZQDM> tmpZQDM;
        tmpZQDM.reserve(200);
        {
            nRet = SECITPDK_QueryZQDMInfo(sKhh.c_str(), 200, lpBrowindex.c_str(), lpJys.c_str(), lpZqdm.c_str(), lpZqlb.c_str(), nExeflag, tmpZQDM);
        }
        if (nRet < 0)
        {
            string msg = SECITPDK_GetLastError();              //查询失败，获取错误信息
            s_spLogger->error("SECITPDK_QueryZQDMInfo failed. Msg:%s\n", gbk_to_utf8(msg));
            break;
        }
        else
        {
            arZQDM.insert(arZQDM.end(), tmpZQDM.begin(), tmpZQDM.end());
            lpBrowindex = (tmpZQDM.end() - 1)->BrowIndex;
        }
    } while (nRet >= 200);

    s_spLogger->info("SECITPDK_QueryZQDMInfo success. Num of results {}.", arZQDM.size());
    std::cout<<"SECITPDK_QueryZQDMInfo success. Num of results "<<arZQDM.size()<<"."<<std::endl;
    //s_spLogger->flush();
    s_spLogger->info("开始更新涨停价.");
    std::cout<<"开始更新涨停价."<<std::endl;
    //s_spLogger->flush();
    int32 count_updated = 0;
    for (auto& zqdmRecord : arZQDM)
    {
        std::string market = zqdmRecord.Market;
        std::string stock_code = zqdmRecord.StockCode;
        std::string stockCodeKey;

        if (!market.empty())
        {
            stockCodeKey = stock_code + "." + market;
        }

        auto it = stockCodeKey.empty() ? m_stockManagerMap.end() : m_stockManagerMap.find(stockCodeKey);
        if (it == m_stockManagerMap.end())
        {
            // 兼容异常返回值或历史数据：尝试常见后缀回退匹配
            auto it_sh = m_stockManagerMap.find(stock_code + ".SH");
            if (it_sh != m_stockManagerMap.end())
            {
                it = it_sh;
                stockCodeKey = stock_code + ".SH";
            }
            else
            {
                auto it_sz = m_stockManagerMap.find(stock_code + ".SZ");
                if (it_sz != m_stockManagerMap.end())
                {
                    it = it_sz;
                    stockCodeKey = stock_code + ".SZ";
                }
            }
        }
        if (it != m_stockManagerMap.end())
        {
            std::cout<< stockCodeKey << " HighLimitPrice:" << zqdmRecord.HighLimitPrice <<std::endl;
            s_spLogger->info("通过查询的涨停价更新成功 {},原先价格 {},新价格 {}.", stockCodeKey,it->second->m_limitUpPrice,zqdmRecord.HighLimitPrice);
            s_spLogger->flush();
            it->second->m_limitUpPrice = zqdmRecord.HighLimitPrice;
            count_updated++;
        }
    }
    s_spLogger->info("涨停价更新结束,共有待更新白名单股票:{}个,已更新:{}个,查询服务器总共返回查询:{}个.", m_stockManagerMap.size(),count_updated,arZQDM.size());
    s_spLogger->flush();
    return true;
    return count_updated == m_stockManagerMap.size();
}
