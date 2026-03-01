#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "TDFAPI.h"
#include "TDCore.h"
#include "TDFAPIInner.h"
#include "TDNonMarketStruct.h"

struct TdfMsgData {
    static const size_t kPayloadBytes = 1024;

    TdfMsgData();
    TdfMsgData(const TdfMsgData& other);
    TdfMsgData& operator=(const TdfMsgData& other);
    TdfMsgData(TdfMsgData&& other) noexcept;
    TdfMsgData& operator=(TdfMsgData&& other) noexcept;

    THANDLE hTdf;
    TDF_MSG msg;
    TDF_APP_HEAD app_head;
    int64_t enqueue_steady_ns;
    alignas(16) unsigned char payload[kPayloadBytes];

    void reset();
};

class MsgQueue {
public:
    static MsgQueue& getInstance() {
        static MsgQueue instance;
        return instance;
    }

    MsgQueue(const MsgQueue&) = delete;
    MsgQueue& operator=(const MsgQueue&) = delete;

    void push(THANDLE hTdf, const TDF_MSG* pMsgHead);
    bool pop(TdfMsgData& data);
    void stop();
    void clear();

    // 设置白名单：push 时只入队白名单内股票的数据。
    // 空列表 = 不过滤（全市场入队）。必须在行情连接前调用。
    void setWhitelist(const std::vector<std::string>& codes);

    bool isStopped() const {
        return m_isStopped.load(std::memory_order_acquire);
    }

private:
    MsgQueue();

    struct QueueStats {
        std::atomic<uint64_t> dropped_market{0};
        std::atomic<uint64_t> dropped_order{0};
        std::atomic<uint64_t> dropped_transaction{0};
        std::atomic<uint64_t> max_depth{0};
        std::atomic<uint64_t> max_callback_enqueue_ns{0};
        std::atomic<uint64_t> max_queue_delay_ns{0};
    };

    static int64_t steady_now_ns();
    static void sanitize_payload_pointers(int dataType, void* payload, int payload_len);
    static void update_max(std::atomic<uint64_t>& holder, uint64_t value);
    void maybe_log_stats(int64_t now_ns);
    bool has_data() const;
    void add_drop_stat(int dataType, uint64_t dropped_count);
    bool isWhitelisted(const char* szWindCode) const;

private:
    static const uint64_t kCapacity = 1ULL << 16;
    static const uint64_t kMask = kCapacity - 1;

    std::vector<TdfMsgData> m_ring;
    std::atomic<uint64_t> m_read_index;
    std::atomic<uint64_t> m_write_index;
    std::atomic<bool> m_isStopped;
    std::mutex m_wait_mutex;
    std::condition_variable m_cv;
    QueueStats m_stats;
    std::atomic<int64_t> m_last_stats_log_ns;

    // 白名单过滤（启动时设置后运行期只读，无需同步）
    std::unordered_set<std::string> m_whitelist;
    bool m_filterEnabled = false;
    std::atomic<uint64_t> m_filtered_count{0};  // 被过滤掉的消息计数
};

#endif // MSG_QUEUE_H
