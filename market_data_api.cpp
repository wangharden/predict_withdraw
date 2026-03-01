#include "market_data_api.h"
#include "spdlog_api.h"

#include "msg_queue.h"

MarketDataApi::MarketDataApi()
{

    // 设置TDF日志路径（在环境设置之前）
    TDF_SetLogPath("./log");
    // 环境设置（从 PDF）
    TDF_SetEnv(TDF_ENVIRON_HEART_BEAT_INTERVAL, 10);
    TDF_SetEnv(TDF_ENVIRON_MISSED_BEART_COUNT, 3);
    TDF_SetEnv(TDF_ENVIRON_OPEN_TIME_OUT, 30);

    memset(&settings, 0, sizeof(settings));

    settings.nServerNum = 1;

    // 回调
    settings.pfnMsgHandler = OnDataReceived;
    settings.pfnSysMsgNotify = OnSystemMessage;

    settings.nTime = 0;
    //settings.szMarkets = "SZ-2-0;SH-2-0";
    //settings.szMarkets = "SZ-2-0;";
    settings.szMarkets = "";
    settings.szSubScriptions = "";
    settings.nTypeFlags = DATA_TYPE_TRANSACTION | DATA_TYPE_ORDER;
}

bool MarketDataApi::connect(const SettingsManager &settings_manager)
{
    const std::string   host_   =   settings_manager.getMarketHost();
    int                 port_   =   settings_manager.getMarketPort();
    const std::string   user_   =   settings_manager.getMarketUser();
    const std::string   password_ = settings_manager.getMarketPassword();

    strncpy(settings.siServer[0].szIp, host_.c_str(), sizeof(settings.siServer[0].szIp) - 1);
    snprintf(settings.siServer[0].szPort, sizeof(settings.siServer[0].szPort), "%d", port_);
    strncpy(settings.siServer[0].szUser, user_.c_str(), sizeof(settings.siServer[0].szUser) - 1);
    strncpy(settings.siServer[0].szPwd, password_.c_str(), sizeof(settings.siServer[0].szPwd) - 1);

    // 白名单为空时全市场订阅，非空时连接后按代码订阅
    const std::string codes_string = settings_manager.get_codes_string();
    if (codes_string.empty()) {
        settings.szMarkets = "SZ-2-0;SH-2-0";
    }

    TDF_ERR err = TDF_ERR_SUCCESS;

    THANDLE tdf_handle_ = TDF_OpenExt(&settings, &err);

    if (err != TDF_ERR_SUCCESS)
    {
        std::cerr << "连接失败: " << err << std::endl;
        return false;
    }
    else
    {
        std::cout << "连接成功" << std::endl;

        // 白名单非空时按代码订阅；为空时已通过 szMarkets 全市场订阅
        if (!codes_string.empty()) {
            char m_buf[20000];
            std::snprintf(m_buf, sizeof(m_buf), "%s", codes_string.c_str());
            TDF_SetSubscription(tdf_handle_, m_buf, SUBSCRIPTION_SET);
        }
        return true;
    }
}

void MarketDataApi::OnDataReceived(THANDLE hTdf, TDF_MSG* pMsgHead)
{
    try
    {
        if (pMsgHead == nullptr)
        {
            s_spLogger->error("MarketDataApi::OnDataReceived: pMsgHead is null, discard"); // 日志告警
            return;
        }
        if (pMsgHead->nDataType == MSG_DATA_TRANSACTION ||
            pMsgHead->nDataType == MSG_DATA_ORDER ||
            pMsgHead->nDataType == MSG_DATA_MARKET)
        {   //mid 源头过滤,减少无效处理
            MsgQueue::getInstance().push(hTdf, pMsgHead);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Push msg to queue failed: " << e.what() << std::endl;
    }
}

void MarketDataApi::OnSystemMessage(THANDLE hTdf, TDF_MSG* pSysMsg)
{
    if (!pSysMsg)
    {
        return;
    }

    switch (pSysMsg->nDataType)
    {
    case MSG_SYS_CONNECT_RESULT:
    {
        TDF_CONNECT_RESULT* pResult = (TDF_CONNECT_RESULT*)pSysMsg->pData;
        if (pResult && pResult->nConnResult)
        {   //mid 行情 没有 这个回报
            std::cout << "[TDF系统] 连接成功: " << pResult->szIp << ":" << pResult->szPort << std::endl;
        }
        break;
    }
    case MSG_SYS_LOGIN_RESULT:
    {
        TDF_LOGIN_RESULT* pResult = (TDF_LOGIN_RESULT*)pSysMsg->pData;
        if (pResult && pResult->nLoginResult != 0)
        {   //mid 行情 没有 登陆成功回报
            std::cout << "[TDF系统] 登录成功: " << pResult->szInfo << std::endl;
            //            TDF_SetSubscription(tdf_handle_, "600000.SH", SUBSCRIPTION_SET);
        }
        else
        {
            std::cout << "[TDF系统] 登录失败: " << pResult->szInfo << std::endl;
            TDF_SetSubscription(hTdf, "600000.SH", SUBSCRIPTION_SET);
        }
        break;
    }
    case MSG_SYS_CODETABLE_RESULT:
    {   //mid 行情 有 这个回报
        std::cout << "[TDF系统] 代码表接收完成，开始接收行情..." << std::endl;
        //        TDF_SetSubscription(hTdf, "600000.SH", SUBSCRIPTION_SET);
        break;
    }
    }
}
