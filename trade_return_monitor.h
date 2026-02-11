#ifndef TRADE_RETURN_MONITOR_H
#define TRADE_RETURN_MONITOR_H

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "secitpdk/secitpdk.h"

class SettingsManager;

class TradeReturnMonitor {
public:
    explicit TradeReturnMonitor(const SettingsManager& settings_manager);
    ~TradeReturnMonitor();

    void start();
    void on_match(const stStructMsg& msg);

private:
    struct RecordedOrder {
        int64_t order_id = 0;
        double order_price = 0.0;
        int64_t order_qty = 0;
        std::string stock_code;
        std::string entrust_time_hhmmss;
    };

    void snapshot_0917();

    static std::string normalize_stock_code(const std::string& s);
    static std::string extract_hhmmss6(const char* s);
    static int local_hhmmss_int(std::chrono::system_clock::time_point tp);
    static std::string local_time_string(std::chrono::system_clock::time_point tp);
    static uint64_t fnv1a64(const void* data, size_t len, uint64_t seed);

    bool is_watch_stock(const std::string& stock_code) const;
    uint64_t make_dedup_key(const stStructMsg& msg) const;

    void log_match(const stStructMsg& msg,
                   const char* reason,
                   const RecordedOrder* recorded,
                   const std::string& local_time) const;

private:
    std::string khh_;
    std::string sz_gdh_;
    std::unordered_set<std::string> watch_codes_;
    bool filter_by_whitelist_ = false;

    std::unordered_set<std::string> followup_sent_stocks_;

    std::thread snapshot_thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> snapshot_ready_{false};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, RecordedOrder> recorded_by_stock_;
    std::unordered_set<int64_t> recorded_order_ids_;
    std::unordered_set<uint64_t> printed_keys_;
};

#endif // TRADE_RETURN_MONITOR_H
