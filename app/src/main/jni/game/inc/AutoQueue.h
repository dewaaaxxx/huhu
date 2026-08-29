#pragma once

struct MatchInfo {
    bool set;
    string Tier;
    int32_t arg10, arg11;
} lastMatchInfo;

DEFINE(void*, StartMatch, ptr arg1, int64_t arg2, string arg3, int64_t arg4, int32_t arg5, int32_t arg6, int64_t arg7, int32_t arg8, char arg9, int32_t arg10, int32_t arg11) {
    LOGI("StartMatch");

    LOGI("Tier %s", arg3.c_str());
    LOGI("arg4 %p, arg5 %p, arg6 %p, arg7 %p, arg8 %p, arg9 %p, arg10 %p, arg11 %p", arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11);
    
    lastMatchInfo = { true, arg3, arg10, arg11 };

    return _StartMatch(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11);
}

std::map<string, int64_t> modeBets = {
    {"M1", 50}, // Table London 100 - Color Blue
    {"M2", 100}, // Table Sydney 200 - Color Green
    {"M3", 500}, // Table Lisbon 1k - Color Blue
    {"M4", 2500}, // Table Tokyo 2.5k - Color Dark Red
    {"M5", 10000}, // Table Las Vegas 10k - Color Black
    {"M6", 50000}, // Table Jakarta 50k - Color Maroon
    {"M7", 100000}, // Table Toronto 100k - Color Light Grey
    {"M8", 250000}, // Table Cairo 500k - Color Yellow
    {"M9", 500000}, // // Table Dubai 1M - Color Drak Blue
    {"M10", 1000000}, // Table Shanghai 2M - Color Dark Orange
    {"M11", 2500000}, // Table Paris 5M - Color Esmerald
    {"M12", 4000000}, // Table Rome 8M - Color Light Maroon
    {"M13", 5000000}, // Table Bangkok 10M - Color Dark Green
    {"M14", 10000000}, // Table Seoul 20M - Color Grey
    {"M15", 15000000}, // Table Mumbai 30M - Color Red Maroon
    {"M16", 25000000}, // Table Berlin 50M - Color Esmerald
    {"M17", 100000000}, // Table Osaka 200M - Color Dark Blue
};

void StartLastMatch() {
    LOGI("StartLastMatch");

    if (persistent_int["iAutoQueue_Mode"] == 0 && lastMatchInfo.set) {
        _StartMatch(sharedMenuManager.instance, 0, lastMatchInfo.Tier, 0, 0, 0, 0, 0, 0, lastMatchInfo.arg10, lastMatchInfo.arg11);

    } else if (persistent_int["iAutoQueue_Mode"] == 1) {
        auto coins = sharedUserInfo.coins();
        auto maxBet = coins * persistent_int["iAutoQueue_BetPercent"] / 100;
        
        string selectedMode = "M1";
        int64_t selectedBet = 50;
        
        for (const auto& [mode, bet] : modeBets) {
            if (maxBet >= bet) {
                selectedMode = mode;
                selectedBet = bet;
            }
        }
        
        LOGI("Selected mode %s with bet %lld (50%% of %lld coins)", selectedMode.c_str(), selectedBet, coins);
        _StartMatch(sharedMenuManager.instance, 0, selectedMode, 0, 0, 0, 0, 0, 0, 0x7100000001, 0xffffffff);

    } else if (persistent_int["iAutoQueue_Mode"] == 2) {
        // Fix Table mode — map UI index (0-16) to M key
        static const char* mKeys[17] = {
            "M1","M2","M3","M4","M5","M6","M7","M8","M9",
            "M10","M11","M12","M13","M14","M15","M16","M17"
        };

        int fixIdx = persistent_int["iAutoQueue_FixTable"];
        if (fixIdx < 0 || fixIdx > 16) fixIdx = 0;

        string desiredMode = mKeys[fixIdx];
        int64_t desiredBet = modeBets[desiredMode];

        auto coins = sharedUserInfo.coins();

        // If user can't afford the desired table, fall back to highest affordable
        if (coins < desiredBet) {
            string fallbackMode = "M1";
            int64_t fallbackBet = 50;
            for (const auto& [mode, bet] : modeBets) {
                if (coins >= bet && bet >= fallbackBet) {
                    fallbackMode = mode;
                    fallbackBet = bet;
                }
            }
            LOGI("Fix Table: wanted %s (bet %lld) but only have %lld coins, falling back to %s",
                 desiredMode.c_str(), desiredBet, coins, fallbackMode.c_str());
            desiredMode = fallbackMode;
        } else {
            LOGI("Fix Table: starting %s (bet %lld, coins %lld)", desiredMode.c_str(), desiredBet, coins);
        }

        _StartMatch(sharedMenuManager.instance, 0, desiredMode, 0, 0, 0, 0, 0, 0, 0x7100000001, 0xffffffff);
    }
}

DEFINE(int64_t, popMenuState, int64_t arg1, int64_t arg2, int32_t arg3, int64_t arg4) {
    LOGI("popMenuState arg1 %p, arg2 %p, arg3 %d, arg4 %p", arg1, arg2, arg3, arg4);
    return _popMenuState(arg1, arg2, arg3, arg4);
}

void PopMenuState(int stateId) {
    LOGI("PopMenuState %d", stateId);
    auto _popMenuState = M(int64_t, libmain + 0x3051f00, int64_t, int64_t, int32_t, int64_t); // MenuManager::popMenuState:withScene:
    _popMenuState(sharedMenuManager.instance, 0, stateId, sharedMenuManager.instance);
}