#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TDFAPI.h"
#include "TDFAPIStruct.h"

#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"

namespace {

struct MarketConfig {
    std::string host;
    int port = 0;
    std::string user;
    std::string password;
};

bool load_market_config(const char* path, MarketConfig& out) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        std::cerr << "Failed to open config: " << path << "\n";
        return false;
    }
    char readBuffer[64 * 1024];
    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
    rapidjson::Document doc;
    doc.ParseStream(is);
    std::fclose(fp);

    if (!doc.IsObject() || !doc.HasMember("market") || !doc["market"].IsObject()) {
        std::cerr << "Invalid config schema (missing market object): " << path << "\n";
        return false;
    }

    const auto& market = doc["market"];
    if (!market.HasMember("host") || !market["host"].IsString() || !market.HasMember("port") || !market["port"].IsInt() ||
        !market.HasMember("user") || !market["user"].IsString() || !market.HasMember("password") ||
        !market["password"].IsString()) {
        std::cerr << "Invalid market fields in: " << path << "\n";
        return false;
    }

    out.host = market["host"].GetString();
    out.port = market["port"].GetInt();
    out.user = market["user"].GetString();
    out.password = market["password"].GetString();
    return true;
}

struct TypeMax {
    std::atomic<int> max_item_count{0};
    std::atomic<int> max_item_size{0};
    std::atomic<int> max_data_len{0};
};

inline void atomic_max(std::atomic<int>& dst, int v) {
    int cur = dst.load(std::memory_order_relaxed);
    while (v > cur && !dst.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void update(TypeMax& st, int itemCount, int itemSize, int dataLen) {
    atomic_max(st.max_item_count, itemCount);
    atomic_max(st.max_item_size, itemSize);
    atomic_max(st.max_data_len, dataLen);
}

std::atomic<bool> g_running{true};
std::atomic<bool> g_code_table_ready{false};
std::atomic<bool> g_subscription_set{false};
std::atomic<int64_t> g_subscribe_steady_ns{0};

THANDLE g_hTdf = nullptr;

TypeMax g_tx;
TypeMax g_order;
TypeMax g_orderqueue;
TypeMax g_market;
TypeMax g_other;

std::atomic<int> g_dbg_print_left{50};
std::atomic<int> g_last_record_time{0}; // HHMMSSmmm from first item when available
std::atomic<bool> g_measure_enabled{false};
std::atomic<int> g_last_tx_time{0};
std::atomic<int> g_last_order_time{0};
std::atomic<int> g_last_orderqueue_time{0};
std::atomic<int> g_last_market_time{0};
std::atomic<int> g_measure_start_time{0};

std::mutex g_log_mu;
FILE* g_log_fp = nullptr;
std::atomic<int> g_log_first_left{0};
std::atomic<bool> g_log_all{false};
std::atomic<bool> g_log_enabled{false};

const char* data_type_name(int dt) {
    switch (dt) {
    case MSG_DATA_TRANSACTION:
        return "TRANSACTION";
    case MSG_DATA_ORDER:
        return "ORDER";
    case MSG_DATA_ORDERQUEUE:
        return "ORDERQUEUE";
    case MSG_DATA_MARKET:
        return "MARKET";
    default:
        return "OTHER";
    }
}

void log_csv_line(int dataType, int itemCount, int itemSize, int dataLen, int serverTime, int recordTime, const char* firstWind,
                  const char* lastWind) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (!g_log_fp) {
        return;
    }
    std::fprintf(g_log_fp, "%s,%d,%d,%d,%d,%d,%s,%s\n", data_type_name(dataType), itemCount, itemSize, dataLen, serverTime, recordTime,
                 firstWind ? firstWind : "", lastWind ? lastWind : "");
}

void OnDataReceived(THANDLE, TDF_MSG* pMsgHead) {
    if (!pMsgHead || !pMsgHead->pAppHead || !pMsgHead->pData) {
        return;
    }

    const int itemCount = pMsgHead->pAppHead->nItemCount;
    const int itemSize = pMsgHead->pAppHead->nItemSize;
    const int dataLen = pMsgHead->nDataLen;

    // Track time progress using the first record's nTime when possible.
    int rec_time = 0;
    const char* first_wind = "";
    const char* last_wind = "";
    switch (pMsgHead->nDataType) {
    case MSG_DATA_TRANSACTION: {
        auto* p = static_cast<TDF_TRANSACTION*>(pMsgHead->pData);
        if (itemCount > 0) {
            rec_time = p[0].nTime;
            first_wind = p[0].szWindCode;
            last_wind = p[itemCount - 1].szWindCode;
        }
        break;
    }
    case MSG_DATA_ORDER: {
        auto* p = static_cast<TDF_ORDER*>(pMsgHead->pData);
        if (itemCount > 0) {
            rec_time = p[0].nTime;
            first_wind = p[0].szWindCode;
            last_wind = p[itemCount - 1].szWindCode;
        }
        break;
    }
    case MSG_DATA_ORDERQUEUE: {
        auto* p = static_cast<TDF_ORDER_QUEUE*>(pMsgHead->pData);
        if (itemCount > 0) {
            rec_time = p[0].nTime;
            first_wind = p[0].szWindCode;
            last_wind = p[itemCount - 1].szWindCode;
        }
        break;
    }
    case MSG_DATA_MARKET: {
        auto* p = static_cast<TDF_MARKET_DATA*>(pMsgHead->pData);
        if (itemCount > 0) {
            rec_time = p[0].nTime;
            first_wind = p[0].szWindCode;
            last_wind = p[itemCount - 1].szWindCode;
        }
        break;
    }
    default:
        break;
    }
    if (rec_time > 0) {
        atomic_max(g_last_record_time, rec_time);
        switch (pMsgHead->nDataType) {
        case MSG_DATA_TRANSACTION:
            atomic_max(g_last_tx_time, rec_time);
            break;
        case MSG_DATA_ORDER:
            atomic_max(g_last_order_time, rec_time);
            break;
        case MSG_DATA_ORDERQUEUE:
            atomic_max(g_last_orderqueue_time, rec_time);
            break;
        case MSG_DATA_MARKET:
            atomic_max(g_last_market_time, rec_time);
            break;
        default:
            break;
        }
    }

    int left = g_dbg_print_left.load(std::memory_order_relaxed);
    while (left > 0 && !g_dbg_print_left.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
    }
    if (left > 0) {
        std::cout << "DBG dataType=" << pMsgHead->nDataType << " dataLen=" << dataLen << " itemCount=" << itemCount
                  << " itemSize=" << itemSize << " serverTime=" << pMsgHead->nServerTime << "\n";
    }

    const int measure_start_time = g_measure_start_time.load(std::memory_order_relaxed);
    if (g_measure_enabled.load(std::memory_order_relaxed) && (measure_start_time > 0) && (rec_time > 0) && (rec_time < measure_start_time)) {
        return;
    }

    if (g_measure_enabled.load(std::memory_order_relaxed) && g_log_enabled.load(std::memory_order_relaxed)) {
        bool do_log = false;
        if (g_log_all.load(std::memory_order_relaxed) || itemCount > 1) {
            do_log = true;
        } else {
            int log_left = g_log_first_left.load(std::memory_order_relaxed);
            while (log_left > 0 &&
                   !g_log_first_left.compare_exchange_weak(log_left, log_left - 1, std::memory_order_relaxed)) {
            }
            if (log_left > 0) {
                do_log = true;
            }
        }
        if (do_log) {
            log_csv_line(pMsgHead->nDataType, itemCount, itemSize, dataLen, pMsgHead->nServerTime, rec_time, first_wind, last_wind);
        }
    }

    if (!g_measure_enabled.load(std::memory_order_relaxed)) {
        return;
    }

    switch (pMsgHead->nDataType) {
    case MSG_DATA_TRANSACTION:
        update(g_tx, itemCount, itemSize, dataLen);
        break;
    case MSG_DATA_ORDER:
        update(g_order, itemCount, itemSize, dataLen);
        break;
    case MSG_DATA_ORDERQUEUE:
        update(g_orderqueue, itemCount, itemSize, dataLen);
        break;
    case MSG_DATA_MARKET:
        update(g_market, itemCount, itemSize, dataLen);
        break;
    default:
        update(g_other, itemCount, itemSize, dataLen);
        break;
    }
}

void OnSystemMessage(THANDLE, TDF_MSG* pSysMsg) {
    if (!pSysMsg) {
        return;
    }
    if (pSysMsg->nDataType == MSG_SYS_CODETABLE_RESULT) {
        g_code_table_ready.store(true, std::memory_order_release);
    } else if (pSysMsg->nDataType == MSG_SYS_FAIL_REPLAY) {
        auto* p = static_cast<TDF_FAIL_REPLAY*>(pSysMsg->pData);
        if (p) {
            std::cerr << "Replay failed: market=" << p->szMarket << " date=" << p->nDate << " info=" << p->szInfo << "\n";
        } else {
            std::cerr << "Replay failed.\n";
        }
        g_running.store(false, std::memory_order_release);
    } else if (pSysMsg->nDataType == MSG_SYS_DISCONNECT_NETWORK) {
        std::cerr << "Network disconnected.\n";
        g_running.store(false, std::memory_order_release);
    }
}

std::string join_subscription(const std::vector<std::string>& codes) {
    std::string out;
    out.reserve(codes.size() * 12);
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i) out.push_back(';');
        out.append(codes[i]);
    }
    return out;
}

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string build_1500_sub_list_from_code_table(THANDLE hTdf, int total) {
    TDF_CODE* sh_codes = nullptr;
    unsigned int sh_items = 0;
    TDF_CODE* sz_codes = nullptr;
    unsigned int sz_items = 0;

    (void)TDF_GetCodeTable(hTdf, "SH-2-0", &sh_codes, &sh_items);
    (void)TDF_GetCodeTable(hTdf, "SZ-2-0", &sz_codes, &sz_items);

    const int target_sh = total / 2;
    const int target_sz = total - target_sh;

    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(total));

    // Prefer A-share stock codes to ensure ORDER/TRANSACTION exist in replay.
    // SH: 6xxxxx (incl. 60/68/69), SZ: 0xxxxx/3xxxxx.
    for (unsigned int i = 0; sh_codes && i < sh_items && static_cast<int>(out.size()) < target_sh; ++i) {
        const std::string w = sh_codes[i].szWindCode;
        const std::string code = sh_codes[i].szCode;
        if (w.empty() || code.size() != 6) continue;
        if (!ends_with(w, ".SH")) continue;
        if (code[0] != '6') continue;
        out.emplace_back(w);
    }

    for (unsigned int i = 0; sz_codes && i < sz_items && static_cast<int>(out.size()) < target_sh + target_sz; ++i) {
        const std::string w = sz_codes[i].szWindCode;
        const std::string code = sz_codes[i].szCode;
        if (w.empty() || code.size() != 6) continue;
        if (!ends_with(w, ".SZ")) continue;
        if (!(code[0] == '0' || code[0] == '3')) continue;
        out.emplace_back(w);
    }

    if (sh_codes) TDF_FreeArr(sh_codes);
    if (sz_codes) TDF_FreeArr(sz_codes);

    if (static_cast<int>(out.size()) < total) {
        std::cerr << "Warning: only collected " << out.size() << " codes from code table.\n";
    }
    return join_subscription(out);
}

int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void print_type(const char* name, const TypeMax& st) {
    const int c = st.max_item_count.load(std::memory_order_relaxed);
    const int s = st.max_item_size.load(std::memory_order_relaxed);
    const int l = st.max_data_len.load(std::memory_order_relaxed);
    std::cout << name << ": max_item_count=" << c << " max_item_size=" << s << " max_data_len=" << l
              << " (count*size=" << (static_cast<int64_t>(c) * static_cast<int64_t>(s)) << ")\n";
}

void reset_stats() {
    g_tx.max_item_count.store(0);
    g_tx.max_item_size.store(0);
    g_tx.max_data_len.store(0);
    g_order.max_item_count.store(0);
    g_order.max_item_size.store(0);
    g_order.max_data_len.store(0);
    g_orderqueue.max_item_count.store(0);
    g_orderqueue.max_item_size.store(0);
    g_orderqueue.max_data_len.store(0);
    g_market.max_item_count.store(0);
    g_market.max_item_size.store(0);
    g_market.max_data_len.store(0);
    g_other.max_item_count.store(0);
    g_other.max_item_size.store(0);
    g_other.max_data_len.store(0);
}

THANDLE open_tdf(const MarketConfig& cfg, const std::string& markets, const std::string& subs, unsigned int typeFlags, unsigned int nTime, TDF_ERR& out_err) {
    TDF_OPEN_SETTING_EXT settings;
    std::memset(&settings, 0, sizeof(settings));
    settings.nServerNum = 1;
    std::snprintf(settings.siServer[0].szIp, sizeof(settings.siServer[0].szIp), "%s", cfg.host.c_str());
    std::snprintf(settings.siServer[0].szPort, sizeof(settings.siServer[0].szPort), "%d", cfg.port);
    std::snprintf(settings.siServer[0].szUser, sizeof(settings.siServer[0].szUser), "%s", cfg.user.c_str());
    std::snprintf(settings.siServer[0].szPwd, sizeof(settings.siServer[0].szPwd), "%s", cfg.password.c_str());
    settings.pfnMsgHandler = OnDataReceived;
    settings.pfnSysMsgNotify = OnSystemMessage;
    settings.szMarkets = markets.c_str();
    settings.szResvMarkets = "";
    settings.szSubScriptions = subs.c_str();
    settings.nTypeFlags = typeFlags;
    settings.nTime = nTime;
    settings.nConnectionID = 0;
    settings.nCodeTypeFlags = 0;
    out_err = TDF_ERR_SUCCESS;
    return TDF_OpenExt(&settings, &out_err);
}

} // namespace

int main(int argc, char** argv) {
    int symbol_count = 1500;
    int run_seconds = 20;
    int start_hhmmssmmm = 93000000;
    int wait_start_timeout_seconds = 30;
    const char* log_path = nullptr;
    int log_first = 200;
    bool log_all = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
            symbol_count = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            run_seconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            start_hhmmssmmm = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--wait-start-timeout") == 0 && i + 1 < argc) {
            wait_start_timeout_seconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (std::strcmp(argv[i], "--log-first") == 0 && i + 1 < argc) {
            log_first = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--log-all") == 0) {
            log_all = true;
        }
    }

    MarketConfig cfg;
    if (!load_market_config("account.ini", cfg)) {
        return 2;
    }

    // Keep logs under current working directory.
    (void)TDF_SetLogPath("./logs_probe");

    if (log_path) {
        std::lock_guard<std::mutex> lk(g_log_mu);
        g_log_fp = std::fopen(log_path, "wb");
        if (!g_log_fp) {
            std::cerr << "Failed to open log file: " << log_path << "\n";
            return 2;
        }
        std::fprintf(g_log_fp, "dataType,itemCount,itemSize,dataLen,serverTime,recordTime,firstWind,lastWind\n");
        std::fflush(g_log_fp);
        g_log_first_left.store(log_first, std::memory_order_release);
        g_log_all.store(log_all, std::memory_order_release);
        g_log_enabled.store(true, std::memory_order_release);
        std::cout << "Probe log enabled: " << log_path << " (log_first=" << log_first << " log_all=" << (log_all ? 1 : 0) << ")\n";
    }

    (void)TDF_SetEnv(TDF_ENVIRON_HEART_BEAT_INTERVAL, 10);
    (void)TDF_SetEnv(TDF_ENVIRON_MISSED_BEART_COUNT, 3);
    (void)TDF_SetEnv(TDF_ENVIRON_OPEN_TIME_OUT, 30);

    std::cout << "Connecting to " << cfg.host << ":" << cfg.port << " user=" << cfg.user << "\n";
    std::cout << "Enum values: MARKET=" << MSG_DATA_MARKET << " TRANSACTION=" << MSG_DATA_TRANSACTION
              << " ORDER=" << MSG_DATA_ORDER << " ORDERQUEUE=" << MSG_DATA_ORDERQUEUE << "\n";

    // Phase 1: open a lightweight connection to fetch code table, then build the N-symbol subscription string.
    const std::string markets = "SZ-2-0;SH-2-0";
    const std::string initial_sub = "600000.SH;000001.SZ";
    std::cout << "\n[Phase1] Open (realtime) to fetch code table. initial_sub=" << initial_sub << "\n";
    g_running.store(true, std::memory_order_release);
    g_code_table_ready.store(false, std::memory_order_release);
    g_dbg_print_left.store(0, std::memory_order_release); // no debug prints in phase1
    TDF_ERR err = TDF_ERR_SUCCESS;
    THANDLE h1 = open_tdf(cfg, markets, initial_sub, DATA_TYPE_NONE, 0u, err);
    if (err != TDF_ERR_SUCCESS || !h1) {
        std::cerr << "TDF_OpenExt(phase1) failed, err=" << err << "\n";
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire) && !g_code_table_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) {
            std::cerr << "Timeout waiting for code table (phase1).\n";
            break;
        }
    }

    std::string sub_list_1500;
    if (g_running.load(std::memory_order_acquire) && g_code_table_ready.load(std::memory_order_acquire)) {
        std::cout << "[Phase1] Code table ready. Building subscription for " << symbol_count << " symbols...\n";
        sub_list_1500 = build_1500_sub_list_from_code_table(h1, symbol_count);
        std::cout << "[Phase1] Built subscription len=" << sub_list_1500.size() << "\n";
    }
    (void)TDF_Close(h1);

    if (sub_list_1500.empty()) {
        std::cerr << "Failed to build subscription list.\n";
        return 2;
    }

    // Phase 2: open replay with the full subscription list provided at open time (matches 行情测试 history tool behavior).
    std::cout << "\n[Phase2] Open (replay) with N-symbol subscription at open time. nTime=0xFFFFFFFF\n";
    reset_stats();
    g_running.store(true, std::memory_order_release);
    g_code_table_ready.store(false, std::memory_order_release);
    g_last_record_time.store(0, std::memory_order_release);
    g_last_tx_time.store(0, std::memory_order_release);
    g_last_order_time.store(0, std::memory_order_release);
    g_last_orderqueue_time.store(0, std::memory_order_release);
    g_last_market_time.store(0, std::memory_order_release);
    g_measure_start_time.store(0, std::memory_order_release);
    g_measure_enabled.store(false, std::memory_order_release);
    g_dbg_print_left.store(50, std::memory_order_release);

    const unsigned int type_flags = DATA_TYPE_TRANSACTION | DATA_TYPE_ORDER | DATA_TYPE_ORDERQUEUE;
    THANDLE h2 = open_tdf(cfg, markets, sub_list_1500, type_flags, 0xFFFFFFFFu, err);
    if (err != TDF_ERR_SUCCESS || !h2) {
        std::cerr << "TDF_OpenExt(phase2) failed, err=" << err << "\n";
        return 3;
    }

    // Wait until data time reaches start_hhmmssmmm (e.g. 09:30:00.000) then measure for run_seconds.
    const auto t_wait_start = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire)) {
        const int t_mkt = g_last_market_time.load(std::memory_order_acquire);
        const int t_tx = g_last_tx_time.load(std::memory_order_acquire);
        const int t_ord = g_last_order_time.load(std::memory_order_acquire);
        const int t_oq = g_last_orderqueue_time.load(std::memory_order_acquire);

        const bool ok_mkt = (t_mkt >= start_hhmmssmmm);
        const bool ok_tx = ((type_flags & DATA_TYPE_TRANSACTION) == 0u) || (t_tx >= start_hhmmssmmm);
        const bool ok_ord = ((type_flags & DATA_TYPE_ORDER) == 0u) || (t_ord >= start_hhmmssmmm);
        const bool ok_oq = ((type_flags & DATA_TYPE_ORDERQUEUE) == 0u) || (t_oq >= start_hhmmssmmm);

        if (ok_mkt && ok_tx && ok_ord && ok_oq) {
            std::cout << "[Phase2] Reached start time " << start_hhmmssmmm << " (MARKET=" << t_mkt << " TX=" << t_tx
                      << " ORDER=" << t_ord << " ORDERQUEUE=" << t_oq << "). Start measuring.\n";
            break;
        }
        if (std::chrono::steady_clock::now() - t_wait_start > std::chrono::seconds(wait_start_timeout_seconds)) {
            std::cout << "[Phase2] Wait start timeout (" << wait_start_timeout_seconds << "s). MARKET=" << t_mkt << " TX=" << t_tx
                      << " ORDER=" << t_ord << " ORDERQUEUE=" << t_oq << ". Start measuring anyway.\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    reset_stats();
    g_measure_start_time.store(start_hhmmssmmm, std::memory_order_release);
    g_measure_enabled.store(true, std::memory_order_release);

    const auto t_collect = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire) && std::chrono::steady_clock::now() - t_collect < std::chrono::seconds(run_seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    (void)TDF_Close(h2);

    std::cout << "\n=== Max stats (observed) ===\n";
    print_type("TRANSACTION", g_tx);
    print_type("ORDER      ", g_order);
    print_type("ORDERQUEUE ", g_orderqueue);
    print_type("MARKET     ", g_market);
    print_type("OTHER      ", g_other);

    const int max_data_len =
        std::max(std::max(g_tx.max_data_len.load(), g_order.max_data_len.load()),
                 std::max(g_orderqueue.max_data_len.load(), g_market.max_data_len.load()));

    const int max_payload_with_app_head = max_data_len + static_cast<int>(sizeof(TDF_APP_HEAD));
    std::cout << "\nRecommended slot payload capacity (data only) >= " << max_data_len << " bytes\n";
    std::cout << "Recommended slot payload capacity (TDF_APP_HEAD+data) >= " << max_payload_with_app_head << " bytes\n";
    std::cout << "sizeof(TDF_APP_HEAD)=" << sizeof(TDF_APP_HEAD) << " sizeof(TDF_MSG)=" << sizeof(TDF_MSG) << "\n";

    g_log_enabled.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        if (g_log_fp) {
            std::fflush(g_log_fp);
            std::fclose(g_log_fp);
            g_log_fp = nullptr;
        }
    }

    return 0;
}
