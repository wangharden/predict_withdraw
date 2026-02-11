#ifndef TRADER_API_H
#define TRADER_API_H

#include "settings_manager.h"
#include "stdafx.h"
#include <vector>
#include "order_manager_withdraw.h"

class TraderApi
{
public:
    TraderApi(){};

    bool connect(const SettingsManager &settings_manager);

    int64_t login(SettingsManager &settings_manager);

    void query_account_data(std::string Khh,std::string &sShGdh,std::string &sSzGdh,std::string &sShZjzh,std::string &sSzZjzh);

    void init_revocable_orders(const SettingsManager &settings_manager);

    std::vector<ITPDK_DRWT> query_revocable_orders(std::string Khh,int64 nBrowindex,int nRowcount);

    stStructMsg buildStructMsgFromDRWT(const ITPDK_DRWT &drwt);

    int64 getRevocableOrder(std::string const &symbol,std::vector<OrderWithdraw> &stockRevocable);

    void revoke(std::string const & symbol,const SettingsManager &settings_manager);

};

#endif // TRADER_API_H
