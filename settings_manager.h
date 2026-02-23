#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <string>
#include <vector>
#include "time_span_manager.h"
#include <iostream>

class SettingsManager
{
public:
    SettingsManager();

public:
    bool load_account_settings(std::string account_file);
    bool load_white_list(const std::string& filename);

    const std::string&  get_trading_Wtfs() const { return trading_sWtfs; }
    const std::string&  get_trading_Key() const { return trading_sKey; }
    const std::string&  get_trading_Khh() const { return trading_sKhh; }
    const std::string&  getTradingPassword() const { return trading_sPwd; }
    const std::string&  get_trading_node() const { return trading_sNode; }
    const std::string&  get_trading_sh_gdh() const { return trading_sh_gdh; }
    const std::string&  get_trading_sz_gdh() const { return trading_sz_gdh; }

    void set_sh_gdh(const std::string& gdh) { trading_sh_gdh = gdh; }
    void set_sz_gdh(const std::string& gdh) { trading_sz_gdh = gdh; }

    const std::string&  getMarketHost() const { return market_host; }
    const std::string&  getMarketUser() const { return market_user; }
    const std::string&  getMarketPassword() const { return market_password; }
    int                 getMarketPort() const { return market_port; }

    const std::vector<std::string>& get_codes_vector() const
    {
        return stock_codes;
    }

    std::string get_codes_string() const;

private:
    bool readAccountConfig(std::string file_name);

private:
    // ===== 对应JSON的成员变量（与JSON字段名完全一致）=====
    // trading 子对象
    std::string trading_sWtfs;
    std::string trading_sKey;
    std::string trading_sKhh;
    std::string trading_sPwd;
    std::string trading_sNode;
    std::string trading_sh_gdh;
    std::string trading_sz_gdh;

    // market 子对象
    std::string market_host;
    int         market_port = 0;
    std::string market_user;
    std::string market_password;

    std::vector<std::string> stock_codes;
};

#endif // SETTINGSMANAGER_H
