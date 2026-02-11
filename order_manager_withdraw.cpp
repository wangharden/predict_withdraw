#include "order_manager_withdraw.h"
#include "spdlog_api.h"
#include "utility_functions.h"
#include "trade_return_monitor.h"

OrderManagerWithdraw * orderManagerWithdraw = OrderManagerWithdraw::get_order_manager_withdraw();

OrderManagerWithdraw::OrderManagerWithdraw() : m_isRunning(false)
{

}

OrderManagerWithdraw::~OrderManagerWithdraw()
{
    stopWorkerThread();
}

void OrderManagerWithdraw::startWorkerThread()
{
    if (!m_isRunning) {
        m_isRunning = true;
        m_workerThread = std::thread(&OrderManagerWithdraw::workerThread, this);
    }
}

void OrderManagerWithdraw::stopWorkerThread()
{
    if (m_isRunning) {
        m_isRunning = false;
        m_cv.notify_one();
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
    }
}

int64 OrderManagerWithdraw::getRevocableOrderIds(const std::string& stockCode, std::vector<OrderWithdraw>& outIds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    outIds.clear();

    // 遍历可撤订单列表，收集符合条件的OrderId
    for (const auto& order : m_revocableOrders) {
        // 如果证券代码为空，则返回所有可撤委托；否则返回指定证券的
        if (stockCode.empty() || order.StockCode == stockCode) {
            outIds.push_back(order);
        }
    }

    // 对 outIds 按 OrderQty 降序排序
    std::sort(outIds.begin(), outIds.end(),
        [](const OrderWithdraw& a, const OrderWithdraw& b) {
            return a.OrderQty > b.OrderQty; // 降序：a的数量大于b时，a排在前面
        }
    );

    return outIds.size();
}

OrderManagerWithdraw * OrderManagerWithdraw::get_order_manager_withdraw()
{
    // 假设OrderManager是单例，实际项目中应根据实际情况获取实例
    // 这里简化处理，实际应用中建议使用单例模式或全局访问点
    static OrderManagerWithdraw* instance = nullptr;
    if (!instance) {
        // 实际项目中应改为正确的实例获取方式
        instance = new OrderManagerWithdraw();
        instance->startWorkerThread();
        SECITPDK_SetStructMsgCallback(&OrderManagerWithdraw::staticAsyncCallback);
    }

    return instance;
}

void OrderManagerWithdraw::staticAsyncCallback(const char* pTime, stStructMsg &stMsg, int nType)
{
    get_order_manager_withdraw()->handleMessage(pTime, stMsg, nType);
}

void OrderManagerWithdraw::handleMessage(const char* pTime, const stStructMsg &stMsg, int nType)
{
    s_spLogger->info("当前线程id为: {},add to queue ################## msg type:{}",thread_id(),nType);
    // 将消息放入队列，由工作线程处理
    std::lock_guard<std::mutex> lock(m_mutex);
    m_msgQueue.emplace(pTime ? pTime : "", stMsg, nType);
    m_cv.notify_one();
}

void OrderManagerWithdraw::workerThread()
{
    while (m_isRunning) {
        std::tuple<std::string, stStructMsg, int> msg;
        bool hasMsg = false;

        // 从队列获取消息
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_isRunning || !m_msgQueue.empty(); });

            if (!m_isRunning) break;

            if (!m_msgQueue.empty()) {
                msg = std::move(m_msgQueue.front());
                m_msgQueue.pop();
                hasMsg = true;
            }
        }

        // 处理消息
        if (hasMsg) {
            std::string time = std::get<0>(msg);
            stStructMsg stMsg = std::get<1>(msg);
            int nType = std::get<2>(msg);

            s_spLogger->info("当前线程id为: {},new msg received from queue,order_type:{},order_id:{},stock_code:{},order_volume:{},cancle_order_id:{},with_draw_volume:{}",thread_id(),
                             stMsg.OrderType,stMsg.OrderId,stMsg.StockCode,stMsg.OrderQty,stMsg.CXOrderId,stMsg.WithdrawQty);

            // 处理不同类型的消息
            switch (nType) {
                case NOTIFY_PUSH_ORDER:
                    // 委托推送，检查是否可撤
                    s_spLogger->info("当前线程id为: {},msg : NOTIFY_PUSH_ORDER, msg type:{}",thread_id(),nType);

                    updateRevocableOrders(stMsg);
                    break;
                case NOTIFY_PUSH_MATCH:
                    // 成交推送，更新可撤状态
                    s_spLogger->info("当前线程id为: {},msg : NOTIFY_PUSH_MATCH, msg type:{}",thread_id(),nType);
                    if (m_trade_return_monitor) {
                        m_trade_return_monitor->on_match(stMsg);
                    }
                    updateRevocableOrders(stMsg);
                    break;
                case NOTIFY_PUSH_WITHDRAW:
                    // 撤单推送，从可撤列表移除
                    s_spLogger->info("当前线程id为: {},msg : NOTIFY_PUSH_WITHDRAW, msg type:{}",thread_id(),nType);
                    updateRevocableOrders(stMsg);
                    break;
                case NOTIFY_PUSH_INVALID:
                    // 废单推送，从可撤列表移除
                    s_spLogger->info("当前线程id为: {},msg : NOTIFY_PUSH_INVALID, msg type:{}",thread_id(),nType);
                    updateRevocableOrders(stMsg);
                    break;
                default:
                    // 其他类型消息
                    break;
            }
        }
    }
}

bool OrderManagerWithdraw::isOrderRevocable(const stStructMsg& msg)
{
    // 判断订单是否可撤销的核心逻辑
    // 1. 订单未被撤销（撤销标志不为1）

    //这个未被撤销的状态判断需要确认，常量定义中没有，不知如何使用
    const bool isNotRevoked = (std::strcmp(msg.WithdrawFlag, "W") != 0);    // after push:"O",after withdraw:"W"
    s_spLogger->info("当前线程id为: {},withdrawflag:{}",thread_id(),msg.WithdrawFlag);
    s_spLogger->info("当前线程id为: {},orderStatus:{}",thread_id(),msg.OrderStatus);
    s_spLogger->info("当前线程id为: {},isNotRevoked:{}",thread_id(),isNotRevoked?"True":"False");

    // 2. 订单未完全成交（委托数量 > 总成交数量）
    const bool isUnfilled = (msg.OrderQty > msg.TotalMatchQty);
    s_spLogger->info("当前线程id为: {},isUnfilled:{}",thread_id(),isUnfilled?"True":"False");

    // 3. 订单处于可撤销的状态（已申报但未终结）
    // 假设SBJG_CONFIRM=已报, SBJG_PARTTRADE=部分成交, SBJG_WAITING=等待申报
    const bool isRevocableStatus = (msg.OrderStatus == SBJG_CONFIRM ||
                                   msg.OrderStatus == SBJG_PARTTRADE ||
                                   msg.OrderStatus == SBJG_WAITING);
    s_spLogger->info("当前线程id为: {},isRevocableStatus:{}",thread_id(),isRevocableStatus?"True":"False");


    s_spLogger->info("当前线程id为: {},OrderManager::isOrderRevocable:{}",thread_id(),isNotRevoked && isUnfilled && isRevocableStatus?"True":"False");

    // 所有条件同时满足才允许撤销
    return isNotRevoked && isUnfilled && isRevocableStatus;
}

void OrderManagerWithdraw::updateRevocableOrders(const stStructMsg& msg)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 查找当前订单是否已在可撤列表中
    auto it = std::find_if(m_revocableOrders.begin(), m_revocableOrders.end(),
        [&](const OrderWithdraw& order) {
            return order.OrderId == msg.CXOrderId;
        });

    s_spLogger->info("当前线程id为: {},size of revocable orders: {}",thread_id(),m_revocableOrders.size());

    if (isOrderRevocable(msg)) {
        // 订单可撤：如果已存在则更新，否则添加
        s_spLogger->info("当前线程id为: {},1.当前订单可撤",thread_id());
        if (it != m_revocableOrders.end()) {
            s_spLogger->info("当前线程id为: {},1.1.本地已有，更新到本地",thread_id());
            *it = OrderWithdraw(msg); // 更新现有订单
        } else {
            s_spLogger->info("当前线程id为: {},1.2.本地没有，新增到本地",thread_id());
            m_revocableOrders.emplace_back(msg); // 添加新订单
        }
    } else {
        // 订单不可撤：如果存在则移除
        s_spLogger->info("当前线程id为: {},2.当前订单不可撤",thread_id());
        if (it != m_revocableOrders.end()) {
            s_spLogger->info("当前线程id为: {},2.1.删除本地当前订单",thread_id());
            m_revocableOrders.erase(it);
        }
    }
    s_spLogger->flush();
}

