#include "order_manager_withdraw.h"
#include "spdlog_api.h"
#include "utility_functions.h"
#include "trade_return_monitor.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
constexpr int64_t kCancelAckRetryTimeoutNs = 2LL * 1000 * 1000 * 1000; // 2s
constexpr int kWorkerWakeupMs = 100;
constexpr int kMaxCancelRetryAttempts = 3; // 与 INVALID 重试上限保持一致
}

OrderManagerWithdraw * orderManagerWithdraw = OrderManagerWithdraw::get_order_manager_withdraw();

OrderManagerWithdraw::OrderManagerWithdraw() : m_isRunning(false)
{

}

OrderManagerWithdraw::~OrderManagerWithdraw()
{
    stopWorkerThread();
    if (time_spend_fp_)
    {
        std::fflush(time_spend_fp_);
        std::fclose(time_spend_fp_);
        time_spend_fp_ = nullptr;
    }
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

void OrderManagerWithdraw::set_trading_account_info(const std::string& khh,
                                                    const std::string& sh_gdh,
                                                    const std::string& sz_gdh)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    khh_ = khh;
    sh_gdh_ = sh_gdh;
    sz_gdh_ = sz_gdh;
}

int64_t OrderManagerWithdraw::steady_now_ns()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string OrderManagerWithdraw::normalize_symbol(const std::string& symbol)
{
    if (symbol.size() >= 3 && (symbol.find(".SH") != std::string::npos || symbol.find(".SZ") != std::string::npos))
    {
        return symbol;
    }
    return symbol;
}

bool OrderManagerWithdraw::split_symbol(const std::string& symbol, std::string& out_code, std::string& out_market)
{
    out_code.clear();
    out_market.clear();
    const size_t dot = symbol.find('.');
    if (dot == std::string::npos)
    {
        return false;
    }
    out_code = symbol.substr(0, dot);
    out_market = symbol.substr(dot + 1);
    return (out_market == "SH" || out_market == "SZ");
}

void OrderManagerWithdraw::sanitize_csv_field(char* dst, size_t dst_len, const char* src)
{
    if (!dst || dst_len == 0)
    {
        return;
    }
    dst[0] = '\0';
    if (!src)
    {
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i)
    {
        char c = src[i];
        if (c == ',' || c == '\n' || c == '\r')
        {
            c = ' ';
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

void OrderManagerWithdraw::ensure_time_spend_log_open()
{
    if (time_spend_fp_)
    {
        return;
    }
    time_spend_fp_ = std::fopen("time_spend.log", "ab");
    if (!time_spend_fp_)
    {
        return;
    }
    // 行缓冲：降低flush次数，同时避免丢太多（异常退出时）。
    std::setvbuf(time_spend_fp_, nullptr, _IOLBF, 0);
}

void OrderManagerWithdraw::time_spend_write_line(const char* line)
{
    if (!line)
    {
        return;
    }
    ensure_time_spend_log_open();
    if (!time_spend_fp_)
    {
        return;
    }
    const size_t n = std::strlen(line);
    if (n == 0)
    {
        return;
    }
    std::fwrite(line, 1, n, time_spend_fp_);
    std::fwrite("\n", 1, 1, time_spend_fp_);
}

void OrderManagerWithdraw::post_limitup_trigger(LimitUpTrigger trigger)
{
    if (trigger.symbol.empty())
    {
        return;
    }
    if (trigger.type != LimitUpTriggerType::SEALED_STOP && trigger.limitup_raw <= 0)
    {
        return;
    }
    trigger.symbol = normalize_symbol(trigger.symbol);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        LimitUpOrderState& st = limitup_states_[trigger.symbol];

        // 封板停止：立即标记；忙时允许当前闭环完成
        if (trigger.type == LimitUpTriggerType::SEALED_STOP)
        {
            st.stop_after_done = true;
            // 若仍未开始发送（IDLE/WAIT_SEND），可直接停止；否则等闭环结束再停止
            if (st.phase == LimitUpOrderState::IDLE || st.phase == LimitUpOrderState::WAIT_SEND)
            {
                st.phase = LimitUpOrderState::STOPPED;
                st.pending_sys_id = 0;
                st.to_cancel_sys_id = 0;
                st.cancel_attempts = 0;
            }
            return;
        }

        if (st.phase == LimitUpOrderState::STOPPED || st.stop_after_done)
        {
            return;
        }

        // 仅在空闲态接纳触发；忙时覆盖抑制（不入队）
        if (st.phase != LimitUpOrderState::IDLE)
        {
            st.suppressed_while_busy++;
            return;
        }

        // 价格触发仅用于“第一次启动”；若已启动（已有active/seq>0），忽略
        if (trigger.type == LimitUpTriggerType::PRICE_107)
        {
            if (st.seq > 0 || st.active_sys_id != 0 || st.pending_sys_id != 0)
            {
                return;
            }
        }

        // 仅09:30后
        if (trigger.event_time > 0 && trigger.event_time < 93000000)
        {
            return;
        }

        // 覆盖式：一旦接纳该触发，立即将状态置为busy，避免短时间内重复入队
        st.phase = LimitUpOrderState::WAIT_SEND;
        m_limitupTriggerQueue.push(std::move(trigger));
    }

    m_cv.notify_one();
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
        LimitUpTrigger trigger;
        bool hasTradeMsg = false;
        bool hasTrigger = false;

        // 从队列获取消息
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(kWorkerWakeupMs), [this] {
                return !m_isRunning || !m_msgQueue.empty() || !m_limitupTriggerQueue.empty();
            });

            if (!m_isRunning) break;

            if (!m_msgQueue.empty()) {
                msg = std::move(m_msgQueue.front());
                m_msgQueue.pop();
                hasTradeMsg = true;
            }
            if (!m_limitupTriggerQueue.empty()) {
                trigger = std::move(m_limitupTriggerQueue.front());
                m_limitupTriggerQueue.pop();
                hasTrigger = true;
            }
        }

        // 处理消息
        if (hasTradeMsg) {
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

            // 本策略：回报驱动状态机推进（不影响原有可撤列表维护）
            handle_limitup_trade_msg(time.c_str(), stMsg, nType);
        }
        if (hasTrigger)
        {
            handle_limitup_trigger(trigger);
        }

        handle_limitup_timeouts();
    }
}

void OrderManagerWithdraw::handle_limitup_trigger(const LimitUpTrigger& trigger)
{
    if (trigger.symbol.empty())
    {
        return;
    }
    // limitup_raw 已在post侧校验，这里留作兜底保护
    if (trigger.limitup_raw <= 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = limitup_states_.find(trigger.symbol);
        if (it != limitup_states_.end() && it->second.phase == LimitUpOrderState::WAIT_SEND)
        {
            it->second.phase = it->second.stop_after_done ? LimitUpOrderState::STOPPED : LimitUpOrderState::IDLE;
        }
        return;
    }

    LimitUpOrderState st_copy;
    bool do_send = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = limitup_states_.find(trigger.symbol);
        if (it == limitup_states_.end())
        {
            return;
        }
        LimitUpOrderState& st = it->second;
        if (st.phase == LimitUpOrderState::STOPPED)
        {
            return;
        }
        if (st.stop_after_done)
        {
            // 封板后停止：若还在WAIT_SEND，直接终止（不再发送新单）
            if (st.phase == LimitUpOrderState::WAIT_SEND || st.phase == LimitUpOrderState::IDLE)
            {
                st.phase = LimitUpOrderState::STOPPED;
            }
            else
            {
                st.stop_after_done = true;
            }
            return;
        }
        if (st.phase != LimitUpOrderState::WAIT_SEND)
        {
            st.suppressed_while_busy++;
            return;
        }

        st.seq++;
        st.phase = LimitUpOrderState::WAIT_NEW_ACK;
        st.reason = (trigger.type == LimitUpTriggerType::PRICE_107) ? "PRICE_107" : "SELL_SUM_50W";
        if (trigger.type == LimitUpTriggerType::SELL_SUM_50W && trigger.trigger_count_50w == 1)
        {
            st.reason = "SELL_SUM_50W_FIRST";
        }
        st.trigger_nTime = trigger.event_time;
        st.signal_steady_ns = trigger.signal_steady_ns;
        st.limitup_raw = trigger.limitup_raw;
        st.base_raw = trigger.base_raw;
        st.tick_raw = trigger.tick_raw;
        st.trigger_count_50w = trigger.trigger_count_50w;
        st.send_steady_ns = 0;
        st.pending_sys_id = 0;
        st.to_cancel_sys_id = 0;
        st.cancel_attempts = 0;
        st.last_cancel_send_ns = 0;

        st_copy = st;
        do_send = true;
    }

    if (!do_send)
    {
        return;
    }

    if (!send_limitup_sell_order(trigger.symbol, st_copy))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = limitup_states_.find(trigger.symbol);
        if (it != limitup_states_.end())
        {
            it->second.phase = it->second.stop_after_done ? LimitUpOrderState::STOPPED : LimitUpOrderState::IDLE;
            it->second.pending_sys_id = 0;
            it->second.to_cancel_sys_id = 0;
            it->second.send_steady_ns = 0;
        }
    }
}

bool OrderManagerWithdraw::send_limitup_sell_order(const std::string& symbol, LimitUpOrderState& st)
{
    std::string code;
    std::string market;
    if (!split_symbol(symbol, code, market))
    {
        return false;
    }
    if (khh_.empty())
    {
        return false;
    }
    const std::string& gdh = (market == "SH") ? sh_gdh_ : sz_gdh_;
    if (gdh.empty())
    {
        return false;
    }

    const double price = static_cast<double>(st.limitup_raw) / 10000.0;
    const int64_t qty = 100;

    const int64_t send_ns = steady_now_ns();
    const int64_t sys_id = SECITPDK_OrderEntrust(khh_.c_str(),
                                                 market.c_str(),
                                                 code.c_str(),
                                                 JYLB_SALE,
                                                 qty,
                                                 price,
                                                 0 /* limit */,
                                                 gdh.c_str());
    if (sys_id <= 0)
    {
        std::string err = SECITPDK_GetLastError();
        if (s_spLogger)
        {
            s_spLogger->error("[LUP_ORDER_SEND_FAIL] code={} reason={} ret={} err={}",
                              symbol,
                              st.reason,
                              static_cast<long long>(sys_id),
                              gbk_to_utf8(err).c_str());
            s_spLogger->flush();
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = limitup_states_.find(symbol);
        if (it != limitup_states_.end())
        {
            it->second.pending_sys_id = sys_id;
            it->second.send_steady_ns = send_ns;
        }
    }

    if (s_spLogger)
    {
        s_spLogger->info("[LUP_ORDER_SEND] code={} seq={} sys_id={} price={} qty={} reason={} trigger_time={}",
                         symbol,
                         st.seq,
                         static_cast<long long>(sys_id),
                         price,
                         static_cast<long long>(qty),
                         st.reason,
                         st.trigger_nTime);
        s_spLogger->flush();
    }

    char line[768];
    std::snprintf(line,
                  sizeof(line),
                  "v1,ORDER_SEND,%s,%u,%s,%d,%lld,%lld,%lld,%lld,%lld,%lld,%d",
                  symbol.c_str(),
                  st.seq,
                  st.reason.c_str(),
                  st.trigger_nTime,
                  static_cast<long long>(st.signal_steady_ns),
                  static_cast<long long>(send_ns),
                  static_cast<long long>(st.limitup_raw),
                  static_cast<long long>(st.base_raw),
                  static_cast<long long>(st.tick_raw),
                  static_cast<long long>(sys_id),
                  st.trigger_count_50w);
    time_spend_write_line(line);

    return true;
}

bool OrderManagerWithdraw::send_cancel_order(const std::string& symbol, LimitUpOrderState& st, int64_t target_sys_id)
{
    if (khh_.empty() || target_sys_id <= 0)
    {
        return false;
    }
    std::string code;
    std::string market;
    if (!split_symbol(symbol, code, market))
    {
        return false;
    }

    const int64_t cancel_send_ns = steady_now_ns();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = limitup_states_.find(symbol);
        if (it != limitup_states_.end())
        {
            it->second.last_cancel_send_ns = cancel_send_ns;
        }
    }

    const int64_t ret = SECITPDK_OrderWithdraw(khh_.c_str(), market.c_str(), target_sys_id);
    if (ret <= 0)
    {
        std::string err = SECITPDK_GetLastError();
        if (s_spLogger)
        {
            s_spLogger->error("[LUP_CANCEL_SEND_FAIL] code={} seq={} target_sys_id={} attempt={} ret={} err={}",
                              symbol,
                              st.seq,
                              static_cast<long long>(target_sys_id),
                              st.cancel_attempts,
                              static_cast<long long>(ret),
                              gbk_to_utf8(err).c_str());
            s_spLogger->flush();
        }
        return false;
    }

    if (s_spLogger)
    {
        s_spLogger->info("[LUP_CANCEL_SEND] code={} seq={} target_sys_id={} attempt={}",
                         symbol,
                         st.seq,
                         static_cast<long long>(target_sys_id),
                         st.cancel_attempts);
        s_spLogger->flush();
    }

    char line[256];
    std::snprintf(line,
                  sizeof(line),
                  "v1,CANCEL_SEND,%s,%u,%lld,%d,%lld",
                  symbol.c_str(),
                  st.seq,
                  static_cast<long long>(target_sys_id),
                  st.cancel_attempts,
                  static_cast<long long>(cancel_send_ns));
    time_spend_write_line(line);

    return true;
}

void OrderManagerWithdraw::handle_limitup_timeouts()
{
    struct RetryTask {
        std::string symbol;
        int64_t target_sys_id;
        uint32_t seq;
        int attempt;
    };

    const int64_t now_ns = steady_now_ns();
    std::vector<RetryTask> retry_tasks;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& kv : limitup_states_)
        {
            const std::string& symbol = kv.first;
            LimitUpOrderState& st = kv.second;

            if (st.phase != LimitUpOrderState::WAIT_CANCEL_ACK || st.to_cancel_sys_id <= 0)
            {
                continue;
            }

            const bool no_attempt_yet = (st.last_cancel_send_ns <= 0);
            const bool timed_out = (!no_attempt_yet) && (now_ns - st.last_cancel_send_ns >= kCancelAckRetryTimeoutNs);
            if (!no_attempt_yet && !timed_out)
            {
                continue;
            }

            if (st.cancel_attempts >= kMaxCancelRetryAttempts)
            {
                continue; // 按当前策略停留WAIT_CANCEL_ACK，不再自动放行
            }

            ++st.cancel_attempts;
            RetryTask task;
            task.symbol = symbol;
            task.target_sys_id = st.to_cancel_sys_id;
            task.seq = st.seq;
            task.attempt = st.cancel_attempts;
            retry_tasks.push_back(std::move(task));
        }
    }

    for (size_t i = 0; i < retry_tasks.size(); ++i)
    {
        LimitUpOrderState tmp;
        tmp.seq = retry_tasks[i].seq;
        tmp.cancel_attempts = retry_tasks[i].attempt;
        send_cancel_order(retry_tasks[i].symbol, tmp, retry_tasks[i].target_sys_id);
    }
}

void OrderManagerWithdraw::handle_limitup_trade_msg(const char* pTime, const stStructMsg& stMsg, int nType)
{
    const std::string market(stMsg.Market);
    const std::string code(stMsg.StockCode);
    if (market.empty() || code.empty())
    {
        return;
    }
    const std::string symbol = code + "." + market;

    char pTime_s[64];
    sanitize_csv_field(pTime_s, sizeof(pTime_s), pTime);
    char confirm_s[32];
    sanitize_csv_field(confirm_s, sizeof(confirm_s), stMsg.ConfirmTime);
    char result_s[128];
    sanitize_csv_field(result_s, sizeof(result_s), stMsg.ResultInfo);

    const int64_t now_ns = steady_now_ns();

    // ====== 新单确认 ======
    if (nType == NOTIFY_PUSH_ORDER)
    {
        uint32_t seq = 0;
        int64_t sys_id = stMsg.OrderId;
        int64_t prev_active = 0;
        bool need_cancel = false;
        bool stopped = false;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = limitup_states_.find(symbol);
            if (it == limitup_states_.end())
            {
                return;
            }
            LimitUpOrderState& st = it->second;
            if (st.phase != LimitUpOrderState::WAIT_NEW_ACK || st.pending_sys_id <= 0 || sys_id != st.pending_sys_id)
            {
                return;
            }

            seq = st.seq;
            prev_active = st.active_sys_id;

            st.active_sys_id = st.pending_sys_id;
            st.pending_sys_id = 0;

            if (prev_active > 0)
            {
                st.to_cancel_sys_id = prev_active;
                st.cancel_attempts = 1;
                st.last_cancel_send_ns = 0;
                st.phase = LimitUpOrderState::WAIT_CANCEL_ACK;
                need_cancel = true;
            }
            else
            {
                st.phase = st.stop_after_done ? LimitUpOrderState::STOPPED : LimitUpOrderState::IDLE;
                stopped = (st.phase == LimitUpOrderState::STOPPED);
            }
        }

        if (s_spLogger)
        {
            s_spLogger->info("[LUP_ORDER_ACK] code={} seq={} sys_id={} confirm_time={} pTime={}",
                             symbol,
                             seq,
                             static_cast<long long>(sys_id),
                             confirm_s,
                             pTime_s);
            s_spLogger->flush();
        }

        char line[512];
        std::snprintf(line,
                      sizeof(line),
                      "v1,ORDER_ACK,%s,%u,%lld,%s,%s,%lld,%d,%s",
                      symbol.c_str(),
                      seq,
                      static_cast<long long>(sys_id),
                      pTime_s,
                      confirm_s,
                      static_cast<long long>(now_ns),
                      stMsg.OrderStatus,
                      result_s);
        time_spend_write_line(line);

        if (need_cancel && prev_active > 0)
        {
            LimitUpOrderState tmp;
            tmp.seq = seq;
            tmp.cancel_attempts = 1;
            send_cancel_order(symbol, tmp, prev_active);
        }
        (void)stopped;
        return;
    }

    // ====== 撤单确认 ======
    if (nType == NOTIFY_PUSH_WITHDRAW)
    {
        uint32_t seq = 0;
        int64_t target = 0;
        const int64_t oid = stMsg.OrderId;
        const int64_t cx_oid = stMsg.CXOrderId;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = limitup_states_.find(symbol);
            if (it == limitup_states_.end())
            {
                return;
            }
            LimitUpOrderState& st = it->second;
            if (st.phase != LimitUpOrderState::WAIT_CANCEL_ACK || st.to_cancel_sys_id <= 0)
            {
                return;
            }
            if (oid != st.to_cancel_sys_id && cx_oid != st.to_cancel_sys_id)
            {
                return;
            }
            seq = st.seq;
            target = st.to_cancel_sys_id;

            st.to_cancel_sys_id = 0;
            st.cancel_attempts = 0;
            st.last_cancel_send_ns = 0;
            st.phase = st.stop_after_done ? LimitUpOrderState::STOPPED : LimitUpOrderState::IDLE;
        }

        if (s_spLogger)
        {
            s_spLogger->info("[LUP_CANCEL_ACK] code={} seq={} target_sys_id={} confirm_time={} pTime={}",
                             symbol,
                             seq,
                             static_cast<long long>(target),
                             confirm_s,
                             pTime_s);
            s_spLogger->flush();
        }

        char line[512];
        std::snprintf(line,
                      sizeof(line),
                      "v1,CANCEL_ACK,%s,%u,%lld,%s,%s,%lld,%d,%s",
                      symbol.c_str(),
                      seq,
                      static_cast<long long>(target),
                      pTime_s,
                      confirm_s,
                      static_cast<long long>(now_ns),
                      stMsg.OrderStatus,
                      result_s);
        time_spend_write_line(line);
        return;
    }

    // ====== 废单：新单废单 或 撤单废单（触发重试） ======
    if (nType == NOTIFY_PUSH_INVALID)
    {
        uint32_t seq = 0;
        int64_t pending = 0;
        int64_t target = 0;
        int attempt = 0;
        bool retry_cancel = false;
        const int64_t oid = stMsg.OrderId;
        const int64_t cx_oid = stMsg.CXOrderId;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = limitup_states_.find(symbol);
            if (it == limitup_states_.end())
            {
                return;
            }
            LimitUpOrderState& st = it->second;
            seq = st.seq;
            pending = st.pending_sys_id;
            target = st.to_cancel_sys_id;

            if (st.phase == LimitUpOrderState::WAIT_NEW_ACK && pending > 0 && oid == pending)
            {
                st.pending_sys_id = 0;
                st.phase = st.stop_after_done ? LimitUpOrderState::STOPPED : LimitUpOrderState::IDLE;

                if (s_spLogger)
                {
                    s_spLogger->warn("[LUP_ORDER_INVALID] code={} seq={} sys_id={} confirm_time={} pTime={} status={} result={}",
                                     symbol,
                                     seq,
                                     static_cast<long long>(oid),
                                     confirm_s,
                                     pTime_s,
                                     stMsg.OrderStatus,
                                     result_s);
                    s_spLogger->flush();
                }

                char line[512];
                std::snprintf(line,
                              sizeof(line),
                              "v1,ORDER_INVALID,%s,%u,%lld,%s,%s,%lld,%d,%s",
                              symbol.c_str(),
                              seq,
                              static_cast<long long>(oid),
                              pTime_s,
                              confirm_s,
                              static_cast<long long>(now_ns),
                              stMsg.OrderStatus,
                              result_s);
                time_spend_write_line(line);
                return;
            }

            if (st.phase == LimitUpOrderState::WAIT_CANCEL_ACK && target > 0 && (oid == target || cx_oid == target))
            {
                st.cancel_attempts++;
                attempt = st.cancel_attempts;
                retry_cancel = (attempt <= kMaxCancelRetryAttempts);
            }
            else
            {
                return;
            }
        }

        if (s_spLogger)
        {
            s_spLogger->warn("[LUP_CANCEL_INVALID] code={} seq={} target_sys_id={} attempt={} confirm_time={} pTime={}",
                             symbol,
                             seq,
                             static_cast<long long>(target),
                             attempt,
                             confirm_s,
                             pTime_s);
            s_spLogger->flush();
        }

        char line[512];
        std::snprintf(line,
                      sizeof(line),
                      "v1,CANCEL_INVALID,%s,%u,%lld,%d,%s,%s,%lld,%d,%s",
                      symbol.c_str(),
                      seq,
                      static_cast<long long>(target),
                      attempt,
                      pTime_s,
                      confirm_s,
                      static_cast<long long>(now_ns),
                      stMsg.OrderStatus,
                      result_s);
        time_spend_write_line(line);

        if (retry_cancel && target > 0)
        {
            LimitUpOrderState tmp;
            tmp.seq = seq;
            tmp.cancel_attempts = attempt;
            send_cancel_order(symbol, tmp, target);
        }
        return;
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

