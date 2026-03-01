#include <thread>
#include "market_data_api.h"
#include "msg_queue.h"
#include "spdlog_api.h"
#include "settings_manager.h"
#include "market_data_processor.h"
#include "stock_data_manager_factory.h"
#include "trader_api.h"
#include "trade_return_monitor.h"
#include "order_manager_withdraw.h"

int main(int argc, char*argv[])
{
    {   //mid *** 必须最先建立,其他过程都要调用日志
        ///mid 日志文件必须初始化设置参数创建
        std::string logger_name = "server_buy_withdraw";
        std::string logger_file_name_prefix = "server_logs/server_buy_withdraw_log";
        create_logger(logger_name,logger_file_name_prefix);
    }

    // 延迟初始化 orderManagerWithdraw（全局指针），确保 s_spLogger 已就绪
    orderManagerWithdraw = OrderManagerWithdraw::get_order_manager_withdraw();

    SettingsManager settings_manager;
    if(false == settings_manager.load_account_settings("account.ini"))
    {
        std::cerr << "配置文件 account.ini 读取 失败" << std::endl;
        return 0;
    }
    // 白名单加载：文件不存在或为空时监控所有股票
    settings_manager.load_white_list("white_list.txt");

    if(false == StockDataManagerFactory::getInstance().init_factory(settings_manager.get_codes_vector()))
    {
        std::cerr << "股票管理对象 初始化 失败" << std::endl;
        return 0;
    }

    TraderApi trader_api;
    if(false == trader_api.connect(settings_manager))
    {
        std::cerr << "交易服务器 连接 失败" << std::endl;
        return 0;
    }

    if(trader_api.login(settings_manager)<=0)
    {
        std::cerr << "交易服务器 登陆 失败" << std::endl;
        return 0;
    }

    // 配置本策略下单/撤单所需账户信息
    orderManagerWithdraw->set_trading_account_info(settings_manager.get_trading_Khh(),
                                                   settings_manager.get_trading_sh_gdh(),
                                                   settings_manager.get_trading_sz_gdh());

    TradeReturnMonitor trade_return_monitor(settings_manager);
    orderManagerWithdraw->set_trade_return_monitor(&trade_return_monitor);
    trade_return_monitor.start();

    if(false == StockDataManagerFactory::getInstance().updateLimitupPrice(settings_manager.get_trading_Khh()))
    {
        std::cerr << "涨停价 更新 失败" << std::endl;
        return 0;
    }

    // 源头过滤：白名单非空时，MsgQueue 只入队白名单股票的行情数据
    MsgQueue::getInstance().setWhitelist(settings_manager.get_codes_vector());

    MarketDataApi market_data_api;
    MarketDataProcessor market_data_processor;
    market_data_processor.startProcessThread(trader_api, settings_manager);
    if(false == market_data_api.connect(settings_manager))
    {
        std::cerr << "行情服务器 连接 失败" << std::endl;
        return 0;
    }

    int i = 0;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        i++;
    }

    // MsgQueue::getInstance().stop();
    // std::this_thread::sleep_for(std::chrono::seconds(1));
    // market_data_processor.stopProcessThread();

    return 0;
}
















































