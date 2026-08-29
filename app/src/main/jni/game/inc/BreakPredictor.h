#pragma once

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace BreakPredictor {

struct TableStats {
    int mine = 0;
    int opponent = 0;
};

struct PredictionInfo {
    std::string table;
    int mine = 0;
    int opponent = 0;
    int total = 0;
    bool known = false;
    bool minePrediction = false;
    int confidence = 0;
};

inline std::mutex g_mutex;
inline std::map<std::string, TableStats> g_stats;
inline std::string g_pendingTable;
inline std::string g_loadedPackage;
inline bool g_loaded = false;
inline bool g_observedCurrentMatch = false;

inline std::string StoragePathUnlocked() {
    if (PACKAGE_NAME.empty()) return std::string();
    return std::string("/data/user/0/") + PACKAGE_NAME + "/files/break_predictor.dat";
}

inline void EnsureLoadedUnlocked() {
    if (PACKAGE_NAME.empty()) return;
    if (g_loaded && g_loadedPackage == PACKAGE_NAME) return;

    g_stats.clear();
    const std::string path = StoragePathUnlocked();
    std::ifstream in(path);
    std::string table;
    int mine = 0;
    int opponent = 0;

    while (in >> table >> mine >> opponent) {
        if (table.empty() || mine < 0 || opponent < 0) continue;
        g_stats[table] = { mine, opponent };
    }

    g_loadedPackage = PACKAGE_NAME;
    g_loaded = true;
}

inline void SaveUnlocked() {
    const std::string path = StoragePathUnlocked();
    if (path.empty()) return;

    std::ofstream out(path, std::ios::trunc);
    if (!out) return;

    for (const auto& entry : g_stats) {
        out << entry.first << ' '
            << entry.second.mine << ' '
            << entry.second.opponent << '\n';
    }
}

// Called when a new match request is issued. The value is not a fact about
// the next break; it only identifies the table whose history will be used.
inline void SetPendingTable(const std::string& table) {
    if (table.empty()) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureLoadedUnlocked();
    g_pendingTable = table;
    g_observedCurrentMatch = false;
}

inline std::string PendingTable() {
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureLoadedUnlocked();
    return g_pendingTable;
}

// State 4 is the local player's turn and state 7 is the opponent's turn.
// The first such state after StartMatch is used as the observed break owner.
inline void ObserveFirstTurn(int stateId) {
    if (stateId != 4 && stateId != 7) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureLoadedUnlocked();
    if (g_pendingTable.empty() || g_observedCurrentMatch) return;

    TableStats& stats = g_stats[g_pendingTable];
    if (stateId == 4) ++stats.mine;
    else ++stats.opponent;

    g_observedCurrentMatch = true;
    SaveUnlocked();
}

inline PredictionInfo GetPrediction(const std::string& table) {
    PredictionInfo result;
    result.table = table;
    if (table.empty()) return result;

    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureLoadedUnlocked();

    const auto it = g_stats.find(table);
    if (it == g_stats.end()) return result;

    result.mine = it->second.mine;
    result.opponent = it->second.opponent;
    result.total = result.mine + result.opponent;

    if (result.total <= 0) return result;

    // Historical percentage from observed matches. The UI labels small
    // samples as low confidence instead of hiding the real percentage.
    result.known = true;
    result.minePrediction = result.mine >= result.opponent;
    const int winnerCount = std::max(result.mine, result.opponent);
    result.confidence = (winnerCount * 100) / result.total;
    return result;
}

} // namespace BreakPredictor
