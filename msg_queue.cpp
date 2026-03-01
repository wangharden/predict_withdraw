#include "msg_queue.h"

#include "spdlog_api.h"

#include <chrono>
#include <cstring>

namespace {
constexpr int64_t kStatsLogIntervalNs = 2LL * 1000 * 1000 * 1000;
}

TdfMsgData::TdfMsgData()
{
    reset();
}

TdfMsgData::TdfMsgData(const TdfMsgData& other)
{
    *this = other;
}

TdfMsgData& TdfMsgData::operator=(const TdfMsgData& other)
{
    if (this == &other)
    {
        return *this;
    }
    hTdf = other.hTdf;
    msg = other.msg;
    app_head = other.app_head;
    enqueue_steady_ns = other.enqueue_steady_ns;
    std::memcpy(payload, other.payload, sizeof(payload));
    msg.pAppHead = &app_head;
    msg.pData = payload;
    return *this;
}

TdfMsgData::TdfMsgData(TdfMsgData&& other) noexcept
{
    *this = other;
}

TdfMsgData& TdfMsgData::operator=(TdfMsgData&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    return (*this = static_cast<const TdfMsgData&>(other));
}

void TdfMsgData::reset()
{
    hTdf = nullptr;
    std::memset(&msg, 0, sizeof(msg));
    std::memset(&app_head, 0, sizeof(app_head));
    enqueue_steady_ns = 0;
    msg.pAppHead = &app_head;
    msg.pData = payload;
}

MsgQueue::MsgQueue()
    : m_ring(kCapacity)
    , m_read_index(0)
    , m_write_index(0)
    , m_isStopped(false)
    , m_last_stats_log_ns(0)
{
}

int64_t MsgQueue::steady_now_ns()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void MsgQueue::update_max(std::atomic<uint64_t>& holder, uint64_t value)
{
    uint64_t old_value = holder.load(std::memory_order_relaxed);
    while (value > old_value &&
           !holder.compare_exchange_weak(old_value, value, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

void MsgQueue::sanitize_payload_pointers(int dataType, void* payload, int payload_len)
{
    if (!payload || payload_len <= 0)
    {
        return;
    }

    if (dataType == MSG_DATA_ORDER)
    {
        if (payload_len >= static_cast<int>(sizeof(TDF_ORDER)))
        {
            static_cast<TDF_ORDER*>(payload)->pCodeInfo = nullptr;
        }
        return;
    }

    if (dataType == MSG_DATA_TRANSACTION)
    {
        if (payload_len >= static_cast<int>(sizeof(TDF_TRANSACTION)))
        {
            static_cast<TDF_TRANSACTION*>(payload)->pCodeInfo = nullptr;
        }
        return;
    }

    if (dataType == MSG_DATA_MARKET)
    {
        if (payload_len >= static_cast<int>(sizeof(TDF_MARKET_DATA)))
        {
            static_cast<TDF_MARKET_DATA*>(payload)->pCodeInfo = nullptr;
        }
        return;
    }
}

bool MsgQueue::has_data() const
{
    const uint64_t read = m_read_index.load(std::memory_order_relaxed);
    const uint64_t write = m_write_index.load(std::memory_order_acquire);
    return read != write;
}

void MsgQueue::add_drop_stat(int dataType, uint64_t dropped_count)
{
    if (dropped_count == 0)
    {
        return;
    }
    if (dataType == MSG_DATA_MARKET)
    {
        m_stats.dropped_market.fetch_add(dropped_count, std::memory_order_relaxed);
        return;
    }
    if (dataType == MSG_DATA_ORDER)
    {
        const uint64_t total = m_stats.dropped_order.fetch_add(dropped_count, std::memory_order_relaxed) + dropped_count;
        if (s_spLogger && (total % 100 == 0))
        {
            s_spLogger->warn("[MSG_QUEUE_DROP] type=ORDER total={} last_batch={}", total, dropped_count);
        }
        return;
    }
    if (dataType == MSG_DATA_TRANSACTION)
    {
        const uint64_t total = m_stats.dropped_transaction.fetch_add(dropped_count, std::memory_order_relaxed) + dropped_count;
        if (s_spLogger && (total % 100 == 0))
        {
            s_spLogger->warn("[MSG_QUEUE_DROP] type=TRANSACTION total={} last_batch={}", total, dropped_count);
        }
        return;
    }
}

void MsgQueue::maybe_log_stats(int64_t now_ns)
{
    const int64_t last_ns = m_last_stats_log_ns.load(std::memory_order_relaxed);
    if (last_ns > 0 && now_ns - last_ns < kStatsLogIntervalNs)
    {
        return;
    }

    int64_t expected = last_ns;
    if (!m_last_stats_log_ns.compare_exchange_strong(expected, now_ns, std::memory_order_relaxed, std::memory_order_relaxed))
    {
        return;
    }

    if (!s_spLogger)
    {
        return;
    }

    const uint64_t read = m_read_index.load(std::memory_order_relaxed);
    const uint64_t write = m_write_index.load(std::memory_order_acquire);
    const uint64_t depth = write - read;
    s_spLogger->info("[MSG_QUEUE_STATS] depth={} max_depth={} dropped_market={} dropped_order={} dropped_transaction={} filtered={} max_enqueue_ns={} max_queue_delay_ns={}",
                     static_cast<unsigned long long>(depth),
                     static_cast<unsigned long long>(m_stats.max_depth.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(m_stats.dropped_market.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(m_stats.dropped_order.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(m_stats.dropped_transaction.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(m_filtered_count.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(m_stats.max_callback_enqueue_ns.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(m_stats.max_queue_delay_ns.load(std::memory_order_relaxed)));
}

void MsgQueue::setWhitelist(const std::vector<std::string>& codes)
{
    m_whitelist.clear();
    m_whitelist.reserve(codes.size());
    for (const auto& code : codes)
    {
        if (!code.empty())
        {
            m_whitelist.insert(code);
        }
    }
    m_filterEnabled = !m_whitelist.empty();
    if (s_spLogger)
    {
        s_spLogger->info("[MSG_QUEUE] whitelist set: {} codes, filter_enabled={}",
                         m_whitelist.size(), m_filterEnabled ? 1 : 0);
    }
}

bool MsgQueue::isWhitelisted(const char* szWindCode) const
{
    // szWindCode 是 char[32]，SSO 下 std::string 构造无堆分配（长度 < 22）
    return m_whitelist.count(std::string(szWindCode)) > 0;
}

void MsgQueue::push(THANDLE hTdf, const TDF_MSG* pMsgHead)
{
    if (m_isStopped.load(std::memory_order_acquire) || pMsgHead == nullptr)
    {
        return;
    }

    const int data_type = pMsgHead->nDataType;
    if (data_type != MSG_DATA_MARKET &&
        data_type != MSG_DATA_ORDER &&
        data_type != MSG_DATA_TRANSACTION)
    {
        return;
    }

    if (pMsgHead->pAppHead == nullptr || pMsgHead->pData == nullptr)
    {
        return;
    }

    const TDF_APP_HEAD* p_app_head = pMsgHead->pAppHead;
    const int item_count = p_app_head->nItemCount;
    const int item_size = p_app_head->nItemSize;
    if (item_count <= 0 || item_size <= 0)
    {
        return;
    }

    if (item_size > static_cast<int>(TdfMsgData::kPayloadBytes))
    {
        add_drop_stat(data_type, static_cast<uint64_t>(item_count));
        if (s_spLogger)
        {
            s_spLogger->error("[MSG_QUEUE_DROP_OVERSIZE] type={} item_size={} max_payload={}",
                              data_type,
                              item_size,
                              static_cast<int>(TdfMsgData::kPayloadBytes));
        }
        return;
    }

    const int64_t enqueue_begin_ns = steady_now_ns();
    const uint64_t write = m_write_index.load(std::memory_order_relaxed);
    const uint64_t read = m_read_index.load(std::memory_order_acquire);
    const uint64_t used = write - read;

    // 白名单过滤：逐 item 检查 szWindCode，只入队匹配的记录
    // TDF_ORDER / TDF_TRANSACTION / TDF_MARKET_DATA 的前 32 字节都是 szWindCode
    const unsigned char* src_data = static_cast<const unsigned char*>(pMsgHead->pData);
    uint64_t actual_written = 0;

    for (int i = 0; i < item_count; ++i)
    {
        const unsigned char* item_ptr = src_data + static_cast<size_t>(i) * static_cast<size_t>(item_size);

        // 源头过滤：非白名单股票直接跳过，不做任何拷贝
        if (m_filterEnabled)
        {
            const char* windCode = reinterpret_cast<const char*>(item_ptr);
            if (!isWhitelisted(windCode))
            {
                m_filtered_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }

        // 容量检查（逐条检查，因为过滤后实际入队量远小于 item_count）
        if (used + actual_written >= kCapacity)
        {
            add_drop_stat(data_type, 1);
            continue;
        }

        TdfMsgData& slot = m_ring[(write + actual_written) & kMask];
        slot.reset();
        slot.hTdf = hTdf;
        slot.msg.sFlags = pMsgHead->sFlags;
        slot.msg.nDataType = data_type;
        slot.msg.nDataLen = item_size;
        slot.msg.nServerTime = pMsgHead->nServerTime;
        slot.msg.nOrder = pMsgHead->nOrder;
        slot.msg.nConnectId = pMsgHead->nConnectId;
        slot.app_head.nHeadSize = sizeof(TDF_APP_HEAD);
        slot.app_head.nItemCount = 1;
        slot.app_head.nItemSize = item_size;
        slot.enqueue_steady_ns = enqueue_begin_ns;

        std::memcpy(slot.payload, item_ptr, static_cast<size_t>(item_size));
        sanitize_payload_pointers(data_type, slot.payload, item_size);
        ++actual_written;
    }

    if (actual_written == 0)
    {
        return;
    }

    m_write_index.store(write + actual_written, std::memory_order_release);
    update_max(m_stats.max_depth, used + actual_written);

    const int64_t enqueue_end_ns = steady_now_ns();
    if (enqueue_end_ns > enqueue_begin_ns)
    {
        update_max(m_stats.max_callback_enqueue_ns, static_cast<uint64_t>(enqueue_end_ns - enqueue_begin_ns));
    }

    m_cv.notify_one();
    maybe_log_stats(enqueue_end_ns);
}

bool MsgQueue::pop(TdfMsgData& data)
{
    for (;;)
    {
        const uint64_t read = m_read_index.load(std::memory_order_relaxed);
        const uint64_t write = m_write_index.load(std::memory_order_acquire);
        if (read != write)
        {
            data = m_ring[read & kMask];
            m_read_index.store(read + 1, std::memory_order_release);

            const int64_t now_ns = steady_now_ns();
            if (data.enqueue_steady_ns > 0 && now_ns >= data.enqueue_steady_ns)
            {
                update_max(m_stats.max_queue_delay_ns, static_cast<uint64_t>(now_ns - data.enqueue_steady_ns));
            }
            maybe_log_stats(now_ns);
            return true;
        }

        if (m_isStopped.load(std::memory_order_acquire))
        {
            return false;
        }

        std::unique_lock<std::mutex> lock(m_wait_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() {
            return m_isStopped.load(std::memory_order_acquire) || has_data();
        });

        if (m_isStopped.load(std::memory_order_acquire) && !has_data())
        {
            return false;
        }
    }
}

void MsgQueue::stop()
{
    m_isStopped.store(true, std::memory_order_release);
    m_cv.notify_all();
}

void MsgQueue::clear()
{
    const uint64_t write = m_write_index.load(std::memory_order_acquire);
    m_read_index.store(write, std::memory_order_release);
}
