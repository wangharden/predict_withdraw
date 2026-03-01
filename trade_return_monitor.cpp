#include "trade_return_monitor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "itpdk/itpdk_dict.h"
#include "settings_manager.h"
#include "spdlog_api.h"
#include "utility_functions.h"

namespace {

template <size_t N>
std::string safe_cstr(const char (&buf)[N]) {
    size_t len = 0;
    for (; len < N; ++len) {
        if (buf[len] == '\0') break;
    }
    return std::string(buf, len);
}

bool is_sz_market_str(const std::string& market) {
    return market == "SZ" || market.rfind("SZ", 0) == 0;
}

} // namespace

TradeReturnMonitor::TradeReturnMonitor(const SettingsManager& settings_manager) {
    khh_ = settings_manager.get_trading_Khh();
    sz_gdh_ = settings_manager.get_trading_sz_gdh();

    const auto& codes = settings_manager.get_codes_vector();
    watch_codes_.reserve(codes.size());
    for (const auto& raw : codes) {
        std::string normalized = normalize_stock_code(raw);
        if (!normalized.empty()) {
            watch_codes_.insert(normalized);
        }
    }

    if (!watch_codes_.empty()) {
        filter_by_whitelist_ = true;
    }

    recorded_by_stock_.reserve(watch_codes_.empty() ? 1024u : watch_codes_.size());
    recorded_order_ids_.reserve(watch_codes_.empty() ? 1024u : watch_codes_.size());
    printed_keys_.reserve(4096);
    followup_sent_stocks_.reserve(watch_codes_.empty() ? 1024u : watch_codes_.size());
}

TradeReturnMonitor::~TradeReturnMonitor() {
    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }
}

void TradeReturnMonitor::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    snapshot_thread_ = std::thread([this]() {
        auto now = std::chrono::system_clock::now();
        std::time_t now_t = std::chrono::system_clock::to_time_t(now);

        std::tm tm_local;
#ifdef _WIN32
        localtime_s(&tm_local, &now_t);
#else
        localtime_r(&now_t, &tm_local);
#endif
        tm_local.tm_hour = 9;
        tm_local.tm_min = 17;
        tm_local.tm_sec = 0;

        std::time_t target_t = std::mktime(&tm_local);
        auto target_tp = std::chrono::system_clock::from_time_t(target_t);
        if (now < target_tp) {
            std::this_thread::sleep_until(target_tp);
        }

        snapshot_0917();
        snapshot_ready_.store(true);
    });
}

void TradeReturnMonitor::on_match(const stStructMsg& msg) {
    auto now = std::chrono::system_clock::now();
    if (local_hhmmss_int(now) < 93000) {
        return;
    }
    if (!snapshot_ready_.load()) {
        return;
    }

    const std::string market = safe_cstr(msg.Market);
    if (!is_sz_market_str(market)) {
        return;
    }
    if (msg.EntrustType != static_cast<uint8>(JYLB_SALE)) {
        return;
    }

    const std::string stock_code = safe_cstr(msg.StockCode);
    const std::string stock_key = normalize_stock_code(stock_code);
    if (stock_key.empty()) {
        return;
    }
    if (!is_watch_stock(stock_key)) {
        return;
    }

    std::string local_time = local_time_string(now);

    RecordedOrder recorded_copy;
    bool has_recorded = false;
    const char* reason = nullptr;

    bool should_send_followup = false;
    std::string followup_stock_key;
    double followup_price = 0.0;

    uint64_t key = make_dedup_key(msg);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (printed_keys_.find(key) != printed_keys_.end()) {
            return;
        }

        const int64_t order_id = msg.OrderId;
        if (recorded_order_ids_.find(order_id) != recorded_order_ids_.end()) {
            auto it = recorded_by_stock_.find(stock_key);
            if (it != recorded_by_stock_.end()) {
                recorded_copy = it->second;
                has_recorded = true;
            }
            reason = "recorded_0917_order";
        } else {
            auto it = recorded_by_stock_.find(stock_key);
            if (it == recorded_by_stock_.end()) {
                return;
            }
            const RecordedOrder& recorded = it->second;
            if (order_id == recorded.order_id) {
                return;
            }
            if (std::fabs(msg.OrderPrice - recorded.order_price) < 1e-6) {
                recorded_copy = recorded;
                has_recorded = true;
                reason = "same_price_second_sale";

                if (followup_sent_stocks_.find(stock_key) == followup_sent_stocks_.end()) {
                    followup_sent_stocks_.insert(stock_key);
                    should_send_followup = true;
                    followup_stock_key = stock_key;
                    followup_price = recorded.order_price;
                }
            } else {
                return;
            }
        }

        printed_keys_.insert(key);
    }

    log_match(msg, reason, has_recorded ? &recorded_copy : nullptr, local_time);

    if (should_send_followup) {
        if (khh_.empty()) {
            s_spLogger->error("[FOLLOWUP] skip order: missing khh. stock={} price={}", followup_stock_key, followup_price);
            s_spLogger->flush();
            return;
        }
        if (sz_gdh_.empty()) {
            s_spLogger->error("[FOLLOWUP] skip order: missing SZ gdh. stock={} price={}", followup_stock_key, followup_price);
            s_spLogger->flush();
            return;
        }

        // 对于 SZ 成交的第二笔卖出：以第一笔的价格再发一笔 100 股限价买入委托
        int64_t nRet = SECITPDK_OrderEntrust(khh_.c_str(),
                                            "SZ",
                                            followup_stock_key.c_str(),
                                            JYLB_BUY,
                                            100,
                                            followup_price,
                                            0,
                                            sz_gdh_.c_str());
        if (nRet > 0) {
            s_spLogger->info("[FOLLOWUP] limit order sent: stock={} price={} qty=100 ret_wth={}", followup_stock_key, followup_price, static_cast<long long>(nRet));
        } else {
            std::string err = SECITPDK_GetLastError();
            s_spLogger->error("[FOLLOWUP] limit order failed: stock={} price={} qty=100 ret={} err={}",
                              followup_stock_key,
                              followup_price,
                              static_cast<long long>(nRet),
                              gbk_to_utf8(err).c_str());
        }

        // 再发送一笔本方最优价格委托
        // 注意：本方最优价格委托仍需传实际价格，传0会触发价格笼子校验导致废单
        int64_t nRet2 = SECITPDK_OrderEntrust(khh_.c_str(),
                                             "SZ",
                                             followup_stock_key.c_str(),
                                             JYLB_BUY,
                                             100,
                                             followup_price,  // 传实际价格（传0会因价格笼子导致废单）
                                             102,  // DDLX_SZSB_BFZYJ 本方最优价格
                                             sz_gdh_.c_str());
        if (nRet2 > 0) {
            s_spLogger->info("[FOLLOWUP] best price order sent: stock={} qty=100 type=BFZYJ ret_wth={}", followup_stock_key, static_cast<long long>(nRet2));
        } else {
            std::string err2 = SECITPDK_GetLastError();
            s_spLogger->error("[FOLLOWUP] best price order failed: stock={} qty=100 type=BFZYJ ret={} err={}",
                              followup_stock_key,
                              static_cast<long long>(nRet2),
                              gbk_to_utf8(err2).c_str());
        }
        s_spLogger->flush();
    }
}

void TradeReturnMonitor::snapshot_0917() {
    if (khh_.empty()) {
        s_spLogger->error("TradeReturnMonitor: missing KHH.");
        s_spLogger->flush();
        return;
    }

    const int kRowCount = 200;
    int64_t brow_index = 0;
    int64_t total_rows = 0;
    int pages = 0;

    auto t_start = std::chrono::steady_clock::now();

    for (;;) {
        int64_t prev_brow_index = brow_index;
        std::vector<ITPDK_DRWT> arDrwt;
        arDrwt.reserve(kRowCount);
        int64 nRet = SECITPDK_QueryOrders(khh_.c_str(),
                                         0 /* all */,
                                         1 /* sort asc */,
                                         kRowCount,
                                         brow_index,
                                         "SZ",
                                         "",
                                         0,
                                         arDrwt);
        if (nRet < 0) {
            std::string msg = SECITPDK_GetLastError();
            s_spLogger->error("TradeReturnMonitor: SECITPDK_QueryOrders failed. Msg:{}", gbk_to_utf8(msg).c_str());
            s_spLogger->flush();
            break;
        }

        pages++;
        total_rows += nRet;

        for (const auto& drwt : arDrwt) {
            const std::string market = safe_cstr(drwt.Market);
            if (!is_sz_market_str(market)) {
                continue;
            }
            if (drwt.EntrustType != JYLB_SALE) {
                continue;
            }

            std::string stock_code = safe_cstr(drwt.StockCode);
            std::string stock_key = normalize_stock_code(stock_code);
            if (stock_key.empty()) {
                continue;
            }
            if (!is_watch_stock(stock_key)) {
                continue;
            }

            std::string hhmmss = extract_hhmmss6(drwt.EntrustTime);
            if (!hhmmss.empty() && hhmmss > "091700") {
                continue;
            }

            RecordedOrder candidate;
            candidate.order_id = drwt.OrderId;
            candidate.order_price = drwt.OrderPrice;
            candidate.order_qty = drwt.OrderQty;
            candidate.stock_code = stock_key;
            candidate.entrust_time_hhmmss = hhmmss;

            std::lock_guard<std::mutex> lock(mutex_);
            auto it = recorded_by_stock_.find(stock_key);
            if (it == recorded_by_stock_.end()) {
                recorded_by_stock_[stock_key] = candidate;
                recorded_order_ids_.insert(candidate.order_id);
                continue;
            }

            const RecordedOrder& existing = it->second;
            bool should_replace = false;
            if (!candidate.entrust_time_hhmmss.empty() && !existing.entrust_time_hhmmss.empty()) {
                if (candidate.entrust_time_hhmmss < existing.entrust_time_hhmmss) {
                    should_replace = true;
                } else if (candidate.entrust_time_hhmmss == existing.entrust_time_hhmmss &&
                           candidate.order_id < existing.order_id) {
                    should_replace = true;
                }
            } else if (candidate.order_id < existing.order_id) {
                should_replace = true;
            }

            if (should_replace) {
                recorded_order_ids_.erase(existing.order_id);
                it->second = candidate;
                recorded_order_ids_.insert(candidate.order_id);
            }
        }

        if (nRet < kRowCount || arDrwt.empty()) {
            break;
        }

        brow_index = arDrwt.back().BrowIndex;
        if (brow_index == prev_brow_index) {
            break;
        }
    }

    size_t recorded_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorded_count = recorded_by_stock_.size();
    }

    auto t_end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    s_spLogger->info("TradeReturnMonitor: snapshot 09:17 done. recorded_stocks={}, rows={}, pages={}, cost_ms={}",
                     recorded_count,
                     static_cast<long long>(total_rows),
                     pages,
                     static_cast<long long>(ms));
    s_spLogger->flush();
}

std::string TradeReturnMonitor::normalize_stock_code(const std::string& s) {
    std::string out = s;
    size_t dot = out.find('.');
    if (dot != std::string::npos) {
        out = out.substr(0, dot);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
              out.end());
    return out;
}

std::string TradeReturnMonitor::extract_hhmmss6(const char* s) {
    if (!s) return "";
    std::string digits;
    digits.reserve(6);
    for (size_t i = 0; s[i] != '\0' && digits.size() < 6; ++i) {
        if (s[i] >= '0' && s[i] <= '9') {
            digits.push_back(s[i]);
        }
    }
    if (digits.size() != 6) {
        return "";
    }
    return digits;
}

int TradeReturnMonitor::local_hhmmss_int(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_local;
#ifdef _WIN32
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    return tm_local.tm_hour * 10000 + tm_local.tm_min * 100 + tm_local.tm_sec;
}

std::string TradeReturnMonitor::local_time_string(std::chrono::system_clock::time_point tp) {
    long long ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    int ms_part = static_cast<int>(ms_since_epoch % 1000);
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_local;
#ifdef _WIN32
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    // 使用 strftime 替代 std::put_time（GCC 4.8 不支持 put_time）
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_local);
    std::ostringstream oss;
    oss << time_buf << '.' << std::setw(3) << std::setfill('0') << ms_part;
    return oss.str();
}

uint64_t TradeReturnMonitor::fnv1a64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

bool TradeReturnMonitor::is_watch_stock(const std::string& stock_code) const {
    if (!filter_by_whitelist_) {
        return true;
    }
    if (watch_codes_.empty()) {
        return true;
    }
    return watch_codes_.find(normalize_stock_code(stock_code)) != watch_codes_.end();
}

uint64_t TradeReturnMonitor::make_dedup_key(const stStructMsg& msg) const {
    const uint64_t seed = 1469598103934665603ULL;
    uint64_t h = seed;
    h = fnv1a64(&msg.OrderId, sizeof(msg.OrderId), h);

    std::string serial = safe_cstr(msg.MatchSerialNo);
    if (!serial.empty()) {
        h = fnv1a64(serial.data(), serial.size(), h);
        return h;
    }

    std::string match_time = safe_cstr(msg.MatchTime);
    if (!match_time.empty()) {
        h = fnv1a64(match_time.data(), match_time.size(), h);
    }
    h = fnv1a64(&msg.MatchQty, sizeof(msg.MatchQty), h);
    h = fnv1a64(&msg.MatchPrice, sizeof(msg.MatchPrice), h);
    return h;
}

void TradeReturnMonitor::log_match(const stStructMsg& msg,
                                  const char* reason,
                                  const RecordedOrder* recorded,
                                  const std::string& local_time) const {
    std::string result_info = safe_cstr(msg.ResultInfo);
    if (!result_info.empty()) {
        result_info = gbk_to_utf8(result_info);
    }

    if (recorded) {
        s_spLogger->info(
            "[MATCH] local_time={} reason={} stock={} order_id={} order_price={} order_qty={} match_serial={} "
            "match_time={} match_qty={} match_price={} match_amt={} total_match_qty={} total_match_amt={} "
            "status={} withdraw_flag={} result={} recorded_order_id={} recorded_price={} recorded_time={}",
            local_time,
            reason,
            safe_cstr(msg.StockCode),
            static_cast<long long>(msg.OrderId),
            msg.OrderPrice,
            static_cast<long long>(msg.OrderQty),
            safe_cstr(msg.MatchSerialNo),
            safe_cstr(msg.MatchTime),
            static_cast<long long>(msg.MatchQty),
            msg.MatchPrice,
            msg.MatchAmt,
            static_cast<long long>(msg.TotalMatchQty),
            msg.TotalMatchAmt,
            msg.OrderStatus,
            safe_cstr(msg.WithdrawFlag),
            result_info.c_str(),
            static_cast<long long>(recorded->order_id),
            recorded->order_price,
            recorded->entrust_time_hhmmss.c_str());
    } else {
        s_spLogger->info(
            "[MATCH] local_time={} reason={} stock={} order_id={} order_price={} order_qty={} match_serial={} "
            "match_time={} match_qty={} match_price={} match_amt={} total_match_qty={} total_match_amt={} "
            "status={} withdraw_flag={} result={}",
            local_time,
            reason,
            safe_cstr(msg.StockCode),
            static_cast<long long>(msg.OrderId),
            msg.OrderPrice,
            static_cast<long long>(msg.OrderQty),
            safe_cstr(msg.MatchSerialNo),
            safe_cstr(msg.MatchTime),
            static_cast<long long>(msg.MatchQty),
            msg.MatchPrice,
            msg.MatchAmt,
            static_cast<long long>(msg.TotalMatchQty),
            msg.TotalMatchAmt,
            msg.OrderStatus,
            safe_cstr(msg.WithdrawFlag),
            result_info.c_str());
    }
    s_spLogger->flush();
}
