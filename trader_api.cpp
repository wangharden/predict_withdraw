#include "trader_api.h"
#include "secitpdk/secitpdk.h"
#include "spdlog_api.h"
#include "utility_functions.h"
#include <iostream>

bool TraderApi::connect(const SettingsManager &settings_manager)
{
    const std::string   sWtfs = settings_manager.get_trading_Wtfs();
    const std::string   sNode = settings_manager.get_trading_node();

//    std::cout << sWtfs << std::endl;
//    std::cout << sNode << std::endl;

    char sVer[64] = { 0 };

    SECITPDK_SetLogPath("./log");            //日志目录
    SECITPDK_SetProfilePath("./");           //配置文件目录

    if (!SECITPDK_Init(HEADER_VER))
    {   //初始化，在所有接口使用前调用，除路径设置接口外
        s_spLogger->error("当前线程id为: {},初始化失败,SECITPDK_Init()",thread_id());
        s_spLogger->flush();
        return false;
    }

    SECITPDK_SetWriteLog(true);
    SECITPDK_SetFixWriteLog(true);
    SECITPDK_SetWTFS(sWtfs.c_str());               //设置委托方式
    SECITPDK_GetVersion(sVer);

    SECITPDK_SetNode(sNode.c_str());

    s_spLogger->info("当前线程id为: {},veriosn:{}",thread_id(),sVer);

    return true;
}

void TraderApi::query_account_data(std::string Khh,std::string &sShGdh,std::string &sSzGdh,std::string &sShZjzh,std::string &sSzZjzh)
{
    long nRet = 0;
    //查询客户股东信息
    vector<ITPDK_GDH> arGDH;
    arGDH.reserve(5);


    nRet = (long)SECITPDK_QueryAccInfo(Khh.c_str(), arGDH);
    if (nRet < 0)
    {
        string msg = SECITPDK_GetLastError();              //查询失败，获取错误信息
        s_spLogger->error("当前线程id为: {},SECITPDK_QueryAccInfo failed. Msg:{}",thread_id(), gbk_to_utf8(msg).c_str());
        return;
    }
    s_spLogger->info("当前线程id为: {},查询,SECITPDK_QueryAccInfo success. Num of results: {}.",thread_id(), nRet);
    for (auto& itr : arGDH)
    {
        s_spLogger->info("当前线程id为: {},AccountId:{} -- Market:{};SecuAccount:{};FundAccount:{};HolderName:{}",thread_id(),
                         itr.AccountId, itr.Market, itr.SecuAccount, itr.FundAccount,gbk_to_utf8(itr.HolderName));

        if (strlen(itr.SecuAccount) > 0)
        {
            sShZjzh = itr.FundAccount;  //资金账号
            sSzZjzh = itr.FundAccount;
        }
        if (0 == strcmp(itr.Market, "SH"))
        {
            sShGdh = itr.SecuAccount;
        }
        if (0 == strcmp(itr.Market, "SZ"))
        {
            sSzGdh = itr.SecuAccount;
        }
    }
}

int64_t TraderApi::login(SettingsManager &settings_manager)
{
    const std::string   sKey = settings_manager.get_trading_Key();
    const std::string   sKhh = settings_manager.get_trading_Khh();
    const std::string   sPwd = settings_manager.getTradingPassword();

//    std::cout << sKey << std::endl;
//    std::cout << sKhh << std::endl;
//    std::cout << sPwd << std::endl;

    int64 nRet = 0;
    {
        nRet = SECITPDK_TradeLogin(sKey.c_str(), sKhh.c_str(), sPwd.c_str());     //登录
    }
    if (nRet <= 0)
    {
        string msg = SECITPDK_GetLastError();              //登录失败，获取错误信息
        s_spLogger->error("当前线程id为: {},Login failed. Msg:{}",thread_id(), gbk_to_utf8( msg).c_str());
    }
    else
    {
        s_spLogger->info("当前线程id为: {},登录,:Login success. Token:{}",thread_id(), nRet);
        std::string sShGdh,sSzGdh,sShZjzh,sSzZjzh;
        query_account_data(sKhh,sShGdh,sSzGdh,sShZjzh,sSzZjzh);
        settings_manager.set_sh_gdh(sShGdh);
        settings_manager.set_sz_gdh(sSzGdh);
    }
    return nRet;
}

void TraderApi::init_revocable_orders(const SettingsManager &settings_manager)
{
    const std::string   sKhh = settings_manager.get_trading_Khh();
    // 1.启动后，向服务器请求所有可撤委托，并初始化本地可撤委托数据，browIndex应该是翻页，
    // 2.如服务器共有850个可撤委托，browIndex=0,每次请求200时，返回[0,199],browIndex = 1,返回[200,399],
    // 3.这个逻辑得测试，挂单8张，每次请求2条记录，看是否可以请求完所有挂单
    int64 browIndex = 0; // 初始化起始索引为0，使用int64类型匹配函数参数
    int64 count = 0;
    const int MAX_ROWS_PER_QUERY = 100;

    s_spLogger->info("当前线程id为: {},开始向服务器请求所有可撤委托，用于初始化本地可撤委托数据。每次请求可撤委托个数:{}",thread_id(),MAX_ROWS_PER_QUERY);

    while (true) {
        vector<ITPDK_DRWT> arDrwt = query_revocable_orders(sKhh, browIndex, MAX_ROWS_PER_QUERY);

        // 检查查询结果
        if (arDrwt.empty()) {
            s_spLogger->info("当前线程id为: {},查询完成，没有更多待处理订单,共请求:{}次，共请求：{}个可撤委托。",thread_id(),browIndex,count);
            break;
        }

        s_spLogger->info("当前线程id为: {},第 {} 次查询结果，返回 {} 条记录。",thread_id(), browIndex,arDrwt.size());

        // 处理每条记录并保存返回的消息对象
        for (auto& itr : arDrwt) {
            s_spLogger->info("当前线程id为: {},AccountId:{0};WTH:{1} -- SBWTH:{2};StockCode:{3};StockType:{4};JYS:{5};WTJG:{6:.2f};FrozenBalance:{7};MatchAmt:{8};CurrentQty:{9};BrowIndex:{10};KFSBDBH:{11};",
                             thread_id(),
                             itr.AccountId, static_cast<long>(itr.OrderId), itr.SBWTH, itr.StockCode,
                             itr.StockType, itr.Market, itr.OrderPrice, itr.FrozenBalance,
                             itr.MatchAmt, static_cast<long>(itr.OrderQty), static_cast<long>(itr.BrowIndex),
                             itr.KFSBDBH);

            // 保存返回的消息对象供后续使用
            stStructMsg msg = buildStructMsgFromDRWT(itr);

            orderManagerWithdraw->updateRevocableOrders(msg);

            count++;
        }

        browIndex++;
    }
}

// 从 ITPDK_DRWT 构建 stStructMsg 对象
stStructMsg TraderApi::buildStructMsgFromDRWT(const ITPDK_DRWT& drwt) {
    stStructMsg msg;

    // 字段映射
    // 客户号 - 注意长度限制（13字符 vs 16字符）
    strncpy(msg.AccountId, drwt.AccountId, sizeof(msg.AccountId) - 1);

    // 委托号
    msg.OrderId = drwt.OrderId;

    // 撤销委托号
    msg.CXOrderId = drwt.CXOrderId;

    // 交易所 - 注意长度限制（3字符 vs 4字符）
    strncpy(msg.Market, drwt.Market, sizeof(msg.Market) - 1);

    // 证券代码 - 注意长度限制（9字符 vs 12字符）
    strncpy(msg.StockCode, drwt.StockCode, sizeof(msg.StockCode) - 1);

    // 证券类别 - 注意长度限制（9字符 vs 4字符）
    strncpy(msg.StockType, drwt.StockType, sizeof(msg.StockType) - 1);

    // 交易类别
    msg.EntrustType = static_cast<uint8>(drwt.EntrustType);

    // 委托价格
    msg.OrderPrice = drwt.OrderPrice;

    // 委托数量
    msg.OrderQty = drwt.OrderQty;

    // 成交价格
    msg.MatchPrice = drwt.MatchPrice;

    // 本次成交数量
    msg.MatchQty = drwt.MatchQty;

    // 总成交数量（这里假设 MatchQty 就是总成交数量）
    msg.TotalMatchQty = drwt.MatchQty;

    // 撤单数量
    msg.WithdrawQty = drwt.WithdrawQty;

    // 股东号 - 注意长度限制（11字符 vs 12字符）
    strncpy(msg.SecuAccount, drwt.SecuAccount, sizeof(msg.SecuAccount) - 1);

    // 委托批次号
    msg.BatchNo = drwt.BatchNo;

    // 订单类型
    msg.OrderType = drwt.OrderType;

    // 申报结果
    msg.OrderStatus = drwt.OrderStatus;

    // 撤销标志 - 注意长度限制（2字符 vs 4字符）
    strncpy(msg.WithdrawFlag, drwt.WithdrawFlag, sizeof(msg.WithdrawFlag) - 1);

    // 结果说明 - 注意长度限制（61字符 vs 128字符）
    strncpy(msg.ResultInfo, drwt.ResultInfo, sizeof(msg.ResultInfo) - 1);

    // 本次成交金额
    msg.MatchAmt = drwt.MatchAmt;

    // 总成交金额（这里假设 MatchAmt 就是总成交金额）
    msg.TotalMatchAmt = drwt.MatchAmt;

    // 冻结资金
    msg.FrozenBalance = drwt.FrozenBalance;

    // 开发商本地编号 - 注意长度限制（17字符 vs 17字符）
    //strncpy(msg.KFSBDBH, drwt.KFSBDBH, sizeof(msg.KFSBDBH) - 1);

    // 成交时间
    strncpy(msg.MatchTime, drwt.MatchTime, sizeof(msg.MatchTime) - 1);

    // 确认时间（这里使用委托时间作为确认时间）
    strncpy(msg.ConfirmTime, drwt.EntrustTime, sizeof(msg.ConfirmTime) - 1);

    return msg;
}

vector<ITPDK_DRWT> TraderApi::query_revocable_orders(std::string Khh,int64 nBrowindex,int nRowcount)
{
    s_spLogger->info("当前线程id为: {},=========== query_order_pending ============",thread_id());

    //当日委托查询
    int64 nRet = 0;
    vector<ITPDK_DRWT> arDrwt;
    arDrwt.reserve(nRowcount);                        //需要预分配足够空间，查询结果最大返回200条

    // string line, word;
    // vector<string> Str;//创建一个存储string类型的vector;

    //printf("请依次输入是否仅查询可撤委托（0查询全部，1查询可撤，2仅查委托，3仅查撤单）、排序方式（0逆序，1正序）、返回条数、分页符（按委托号）、"
    //       "交易所、证券代码、委托号：（以英文逗号分隔和结尾，不送则留空，例如总送入三个参数，第二个不送，则为1,,3,）    \n");

    //getline(cin, line);
    // line = "0,0,20,10,,,,";
    // istringstream record(line);
    // while (getline(record, word, ','))
    // {
    //     Str.push_back(word);
    // }

    // if (Str.size() < 7)
    // {
    //     printf("输入参数不足！\n");
    //     return 0;
    // }

    // int nType = atoi(Str[0].c_str());
    // int nSortType = atoi(Str[1].c_str());
    //int nRowcount = atoi(Str[2].c_str());
    //int64 nBrowindex = atoll(Str[3].c_str());
    // string lpJys = Str[4];
    // string lpZqdm = Str[5];
    // int64 nWth = atoll(Str[6].c_str());

    int nType = 1;          //mid 0查询全部，1查询可撤，2仅查委托，3仅查撤单）
    int nSortType = 1;      //mid 0逆序，1正序
    //nRowcount = 200;    //mid 查询结果最大返回200条
    //nBrowindex = 20;    //mid 分页符
    string lpJys = "";
    string lpZqdm = "";
    int64 nWth = 0;

    {
        nRet = (long)SECITPDK_QueryOrders(Khh.c_str(), nType, nSortType, nRowcount, nBrowindex, lpJys.c_str(), lpZqdm.c_str(), nWth, arDrwt);
    }
    if (nRet < 0)                               //查询失败
    {
        string msg = SECITPDK_GetLastError();
        s_spLogger->error("当前线程id为: {},SECITPDK_QueryOrders failed. Msg:{}",thread_id(), gbk_to_utf8( msg).c_str());
    }
    return arDrwt;
}

int64 TraderApi::getRevocableOrder(std::string const &symbol,std::vector<OrderWithdraw> &stockRevocable){
    // symbol:"",return all
    // symbol:"600000,return only 600000
    // 3.1 查询所有可撤销订单
    // std::vector<RevocableOrder> allRevocable;
    // size_t count = orderManager->getRevocableOrderIds("", allRevocable);
    // print("当前可撤销订单总数: ", count);
    // for (RevocableOrder order : allRevocable) {
    //     //print("可撤销订单 ID: ", order.OrderId);
    // }

    // 3.2 查询特定股票的可撤销订单
    orderManagerWithdraw->getRevocableOrderIds(symbol, stockRevocable);
    s_spLogger->info("当前线程id为: {},股票: {} 的可撤销订单数: {}",thread_id(),symbol,stockRevocable.size());

    return stockRevocable.size();
}

void TraderApi::revoke(std::string const & symbol, const SettingsManager &settings_manager)
{

    std::string sKhh                =   settings_manager.get_trading_Khh();
    std::string sShGdh              =   settings_manager.get_trading_sh_gdh();                //上海股东号
    std::string sPwd                =   settings_manager.getTradingPassword();    //交易密码;

    ITPDK_CusReqInfo st_;
    
    strncpy(st_.AccountId, sKhh.c_str(), sizeof(st_.AccountId) - 1);
    st_.AccountId[sizeof(st_.AccountId) - 1] = '\0';  // 确保字符串终止

    strncpy(st_.Password, sPwd.c_str(), sizeof(st_.Password) - 1);
    st_.Password[sizeof(st_.Password) - 1] = '\0';    // 确保字符串终止



    std::vector<OrderWithdraw> allRevocable;
    if(0 < getRevocableOrder(symbol,allRevocable))
    {
        for (const OrderWithdraw &order : allRevocable)
        {
            s_spLogger->info("当前线程id为: {},可撤销订单 ID: {}",thread_id(), order.OrderId);
            {
                
                if(order.OrderQty < 2*100)
                {
                    s_spLogger->info("当前线程id为: {},OrderQty is {},no need to revoke.",thread_id(),order.OrderQty);
                    continue;
                }
                
                s_spLogger->info("当前线程id为: {},before withdraw",thread_id());
                int nRet = SECITPDK_OrderWithdrawEx(st_, order.Market.c_str(), order.OrderId);
                s_spLogger->info("当前线程id为: {},after withdraw, order.OrderId:{}",thread_id(),order.OrderId);

                if (nRet > 0)
                {
                    s_spLogger->info("当前线程id为: {},withdraw successed,KHH:{},return:{},token:{}",thread_id(),st_.AccountId,nRet,st_.Token);
                } else {
                    std::string msg = SECITPDK_GetLastError();
                    s_spLogger->error("当前线程id为: {},withdraw failed,KHH:{},return:{},msg:{}",thread_id(),st_.AccountId,nRet,gbk_to_utf8( msg));
                }
            }
        }
    }
}
