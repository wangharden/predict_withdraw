#ifndef ORDERMANAGER_WITHDRAW_H
#define ORDERMANAGER_WITHDRAW_H

#include "stdafx.h"
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>

#include <queue>
#include <memory>
#include <iostream>
#include <algorithm>
#include <cstring>

class TradeReturnMonitor;

// 可撤委托数据结构
struct OrderWithdraw {
    int64 CXOrderId;
    int64 OrderId;               // 委托号
    std::string Market;             // 交易所
    std::string StockCode;         // 证券代码
    std::string AccountId;         // 客户号
    std::string SecuAccount;       // 股东号
    int64 OrderQty;              // 委托数量
    int64 TotalMatchQty;         // 总成交数量
    int32_t OrderStatus;           // 申报状态
    // 可根据需要添加其他字段

    // 从stStructMsg构造
    OrderWithdraw(const stStructMsg& msg)
        :
        CXOrderId(msg.CXOrderId),
        OrderId(msg.OrderId),
        Market(msg.Market),
        StockCode(msg.StockCode),
        AccountId(msg.AccountId),
        SecuAccount(msg.SecuAccount),
        OrderQty(msg.OrderQty),
        TotalMatchQty(msg.TotalMatchQty),
        OrderStatus(msg.OrderStatus) {}
};

class OrderManagerWithdraw
{
public:
    OrderManagerWithdraw();
    ~OrderManagerWithdraw();

    // 启动工作线程
    void startWorkerThread();
    // 停止工作线程
    void stopWorkerThread();

    // 获取指定证券的可撤委托ID列表
    // 参数: stockCode-证券代码, outIds-输出的委托ID向量
    // 返回: 找到的可撤委托数量
    int64 getRevocableOrderIds(const std::string& stockCode, std::vector<OrderWithdraw>& outIds);

    // 更新可撤订单列表
    void updateRevocableOrders(const stStructMsg& msg);

    void set_trade_return_monitor(TradeReturnMonitor* monitor) { m_trade_return_monitor = monitor; }

    static OrderManagerWithdraw * get_order_manager_withdraw();

private:
    // 静态回调函数
    static void staticAsyncCallback(const char* pTime, stStructMsg &stMsg, int nType);
    // 处理消息的实际函数
    void handleMessage(const char* pTime, const stStructMsg &stMsg, int nType);
    // 工作线程函数
    void workerThread();
    // 判断订单是否可撤
    bool isOrderRevocable(const stStructMsg& msg);

private:
    // 线程同步相关
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_workerThread;
    bool m_isRunning;
    std::queue<std::tuple<std::string, stStructMsg, int>> m_msgQueue;

    // 可撤委托存储容器
    std::vector<OrderWithdraw> m_revocableOrders;

    TradeReturnMonitor* m_trade_return_monitor = nullptr;
};

extern OrderManagerWithdraw * orderManagerWithdraw;

#endif // ORDERMANAGER_WITHDRAW_H
