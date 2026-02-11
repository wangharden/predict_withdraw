#include "market_data_processor.h"

#include "stock_data_manager.h"
#include "stock_data_manager_factory.h"

#include <iostream>
#include <stdexcept>
#include "spdlog_api.h"

// 构造函数
MarketDataProcessor::MarketDataProcessor() : m_threadRunning(false) {}

// 析构函数：保证线程优雅退出
MarketDataProcessor::~MarketDataProcessor() {
    stopProcessThread();
}

// 启动处理线程
void MarketDataProcessor::startProcessThread(TraderApi& tr, SettingsManager& settings_manager) {
    if (m_threadRunning)
    {
        std::cout << "Process thread is already running" << std::endl;
        return;
    }

    m_threadRunning = true;
    m_trader_api = tr;
    m_settings_manager = settings_manager;
    // 启动线程
    m_processThread = std::thread(&MarketDataProcessor::processMsgThreadFunc, this);

}

// 停止处理线程
void MarketDataProcessor::stopProcessThread()
{
    if (!m_threadRunning)
    {
        std::cout << "Process thread is not running" << std::endl;
        return;
    }

    // 标记线程停止，停止队列
    m_threadRunning = false;
    MsgQueue::getInstance().stop();

    // 等待线程退出（避免僵尸线程）
    if (m_processThread.joinable())
    {
        m_processThread.join();
    }

    // 清空队列，释放剩余数据
    MsgQueue::getInstance().clear();
    std::cout << "Process thread stopped" << std::endl;
}

// 数据处理线程主函数
void MarketDataProcessor::processMsgThreadFunc()
{
    TdfMsgData data(nullptr, nullptr);
    MsgQueue& msgQueue = MsgQueue::getInstance();

    std::cout << "Process thread started" << std::endl;

    // 循环取数据处理，直到线程停止
    while (msgQueue.pop(data) && m_threadRunning)
    {
        try
        {
            // 根据数据类型分发处理
            switch (data.msg.nDataType)
            {
            case MSG_DATA_ORDER:
                handleOrderData(data);
                break;
            case MSG_DATA_TRANSACTION:
                handleTransactionData(data);
                break;
                // 其他类型暂不处理
            case MSG_DATA_MARKET:
            case MSG_DATA_ORDERQUEUE:
                break;
            default:
                std::cerr << "Unknown msg type: " << data.msg.nDataType << std::endl;
                break;
            }
        }
        catch (const std::exception& e)
        {
            // 捕获异常，避免单个消息处理失败导致线程崩溃
            std::cerr << "Process msg failed: " << e.what() << std::endl;
        }

        // TdfMsgData 出作用域前自动析构，释放内存
    }

    std::cout << "Process thread exited" << std::endl;
}

// 处理 ORDER 数据
void MarketDataProcessor::handleOrderData(const TdfMsgData& data)
{
    //std::cout << "handleOrderData : new order." << std::endl;
    
}

// 处理 TRANSACTION 数据
void MarketDataProcessor::handleTransactionData(const TdfMsgData& data)
{
    
}
