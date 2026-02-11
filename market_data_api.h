#ifndef MARKETDATAAPI_H
#define MARKETDATAAPI_H

#include <assert.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <queue>
#include <cctype>
#include <list>
#include <time.h>
#include <thread>

#include "TDFAPI.h"
#include "TDCore.h"
#include "TDFAPIInner.h"
#include "TDNonMarketStruct.h"
#include "settings_manager.h"

typedef void* THANDLE;

class MarketDataApi
{
public:
    MarketDataApi();
    bool connect(const SettingsManager &settings_manager);

private:
    static void OnDataReceived(THANDLE hTdf, TDF_MSG* pMsgHead);
    static void OnSystemMessage(THANDLE hTdf, TDF_MSG* pSysMsg);

private:
    TDF_OPEN_SETTING_EXT settings;
};

#endif // MARKETDATAAPI_H
