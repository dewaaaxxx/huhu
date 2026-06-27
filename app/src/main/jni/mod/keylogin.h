#include <json/json.hpp>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <random>
#include <curl/curl.h>

#include "include/obfuscate.h"

bool bValid = false;

  // ── Feature flags (set from server response per license key) ─────────────────
  static std::vector<std::string> g_Features;

  // Returns true if the feature is explicitly enabled for this key.
  // If g_Features is empty → all features on by default.
  inline bool HasFeature(const std::string& feat) {
      if (g_Features.empty()) return true; // no restrictions = all features on
      return std::find(g_Features.begin(), g_Features.end(), feat) != g_Features.end();
  }

// ── Crypto helpers ──────────────────────────────────────────────────────────

std::string xor_encrypt(const std::string& data, const std::string& key) {
    std::string result;
    result.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i)
        result += data[i] ^ key[i % key.length()];
    return result;
}

std::string xor_decrypt(const std::string& data, const std::string& key) {
    return xor_encrypt(data, key);
}

std::string base64_encode(const std::string& data) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    size_t i = 0;
    unsigned char a3[3], a4[4];
    while (data.length() - i >= 3) {
        a3[0] = (unsigned char)data[i]; a3[1] = (unsigned char)data[i+1]; a3[2] = (unsigned char)data[i+2];
        a4[0] = (a3[0]&0xfc)>>2; a4[1] = ((a3[0]&0x03)<<4)+((a3[1]&0xf0)>>4);
        a4[2] = ((a3[1]&0x0f)<<2)+((a3[2]&0xc0)>>6); a4[3] = a3[2]&0x3f;
        for (int j=0;j<4;j++) result+=chars[a4[j]];
        i+=3;
    }
    if (data.length()-i>0) {
        int remaining=data.length()-i;
        for (int j=0;j<3;j++) a3[j]=(j<remaining)?(unsigned char)data[i+j]:0;
        a4[0]=(a3[0]&0xfc)>>2; a4[1]=((a3[0]&0x03)<<4)+((a3[1]&0xf0)>>4);
        a4[2]=((a3[1]&0x0f)<<2)+((a3[2]&0xc0)>>6); a4[3]=a3[2]&0x3f;
        for (int j=0;j<remaining+1;j++) result+=chars[a4[j]];
        while (result.length()%4) result+='=';
    }
    return result;
}

std::string base64_decode(const std::string& input) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    std::vector<int> T(256,-1);
    for (int i=0;i<64;i++) T[(unsigned char)chars[i]]=i;
    int val=0,valb=-8;
    for (unsigned char c:input) {
        if (T[c]==-1) break;
        val=(val<<6)+T[c]; valb+=6;
        if (valb>=0) { result.push_back(char((val>>valb)&0xFF)); valb-=8; }
    }
    return result;
}

std::string encryptPayload(const nlohmann::json& obj, const std::string& key) {
    std::string json = obj.dump();
    std::string encrypted = xor_encrypt(json, key);
    return base64_encode(encrypted);
}

nlohmann::json decryptPayload(const std::string& encoded, const std::string& key) {
    try {
        std::string decoded = base64_decode(encoded);
        std::string decrypted = xor_decrypt(decoded, key);
        return nlohmann::json::parse(decrypted);
    } catch (...) {
        return {};
    }
}

// ── libcurl write callback ──────────────────────────────────────────────────

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ── HTTP POST via libcurl ────────────────────────────────────────────────────

static std::string httpPost(const std::string& url, const std::string& body, int timeoutSec = 15) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)timeoutSec);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// ── License status ──────────────────────────────────────────────────────────
  enum LicenseStatusCode { LSC_OK = 0, LSC_BANNED, LSC_EXPIRED, LSC_VERSION_OLD, LSC_PENDING };
  static LicenseStatusCode g_LicenseStatusCode = LSC_PENDING;
  static std::string g_LicenseStatusMsg = "";
  static bool g_StatusChecking = false;
  static time_t g_lastSyncTime  = 0;   // updated every successful refresh

  // ── State vars ───────────────────────────────────────────────────────────────

std::string ERROR_MESSAGE = "";
static bool logged_in     = false;
static bool is_logging_in = false;
std::string g_Token, g_Auth;
std::string g_ExpTime = "N/A";
  static int64_t g_ExpiryTimestamp = 0;

  // Parse ISO date "YYYY-MM-DDTHH:MM:SS..." or "YYYY-MM-DD" to UTC unix timestamp
  static int64_t ParseExpiryDateUTC(const std::string& s) {
      if (s.empty() || s == "N/A" || s == "Lifetime" || s == "null") return 0;
      int y=2000,mo=1,d=1,h=0,m=0,sec=0;
      if (sscanf(s.c_str(),"%d-%d-%dT%d:%d:%d",&y,&mo,&d,&h,&m,&sec) < 3 &&
          sscanf(s.c_str(),"%d-%d-%d",&y,&mo,&d) < 3) return 0;
      static const int dim[]={31,28,31,30,31,30,31,31,30,31,30,31};
      int64_t days=0;
      for(int yr=1970;yr<y;yr++){bool lp=(yr%4==0&&(yr%100!=0||yr%400==0));days+=lp?366:365;}
      bool lp=(y%4==0&&(y%100!=0||y%400==0));
      for(int mn=1;mn<mo;mn++) days+=dim[mn-1]+(mn==2&&lp?1:0);
      days+=d-1;
      return days*86400LL+h*3600LL+m*60LL+sec;
  }

// ── Login ────────────────────────────────────────────────────────────────────

INLINE bool Login(std::string androidID, std::string key) {
    if (androidID.empty()) { ERROR_MESSAGE = O("Could not get Android ID"); return false; }
    if (key.empty())       { ERROR_MESSAGE = O("Key is empty");             return false; }

    is_logging_in = true;
    ERROR_MESSAGE  = "";

    const std::string encrypt_key = OO("JiM21rNU12eERlNmpqa3FuQks").str();
    const std::string ws_token    = OO("KJGMDKFJDHG34KD").str();
    const std::string version     = OO("1.0").str();
    const std::string game_type   = OO("8ball").str();
    const std::string api_url     = OO("https://baru-baru.vercel.app/api/validate").str();

    try {
        nlohmann::json inner = {
            {OO("license_key").str(), key},
            {OO("hwid").str(),        androidID},
            {OO("game_type").str(),   game_type},
            {OO("version").str(),     version},
        };

        std::string encodedData = encryptPayload(inner, encrypt_key);

        nlohmann::json requestBody = {
            {OO("token").str(), ws_token},
            {OO("data").str(),  encodedData},
        };

        std::string rawResponse = httpPost(api_url, requestBody.dump(), 15);

        if (rawResponse.empty()) {
            ERROR_MESSAGE = OO("No response from server").str();
            is_logging_in = false;
            return false;
        }

        nlohmann::json outerJson = nlohmann::json::parse(rawResponse);

        if (!outerJson.contains("data") || !outerJson["data"].is_string()) {
            ERROR_MESSAGE = OO("Bad server response format").str();
            is_logging_in = false;
            return false;
        }

        nlohmann::json respJson = decryptPayload(outerJson["data"].get<std::string>(), encrypt_key);

        if (respJson.empty() || !respJson.contains("status")) {
            ERROR_MESSAGE = OO("Failed to decrypt server response").str();
            is_logging_in = false;
            return false;
        }

        if (respJson["status"] != "success") {
            std::string msg = respJson.value("message", "Unknown error");
            ERROR_MESSAGE = msg;
            is_logging_in = false;
            return false;
        }

        auto data = respJson["data"];

        std::string serverVersion = data.value("version", "");
        if (serverVersion != version) {
            ERROR_MESSAGE = OO("Old version. Please update. Latest: ").str() + serverVersion;
            is_logging_in = false;
            return false;
        }

        std::string expiryDate = data.value("expiry_date", "N/A");
        std::string authToken  = data.value("auth_token",  "");

        logged_in  = true;
        is_logging_in = false;
        g_Token    = authToken;
        g_Auth     = authToken;
        g_ExpTime  = expiryDate;
          g_ExpiryTimestamp = ParseExpiryDateUTC(expiryDate);
        bValid     = (g_Token == g_Auth) && !g_Token.empty();

        // Parse feature list from server
        g_Features.clear();
        if (data.contains("features") && data["features"].is_array()) {
            for (auto& f : data["features"])
                if (f.is_string()) g_Features.push_back(f.get<std::string>());
        }

        persistent_string["key"]  = key;
        persistent_string["hwid"] = androidID;
        save_persistence();
        return true;

    } catch (const std::exception& e) {
        ERROR_MESSAGE = OO("Error: ").str() + std::string(e.what());
        is_logging_in = false;
        return false;
    }
}

  // ── Refresh license status (background periodic check) ───────────────────────
  INLINE void RefreshLicenseStatus() {
      if (g_StatusChecking) return;
      std::string key  = persistent_string["key"];
      std::string hwid = persistent_string["hwid"];
      if (key.empty() || hwid.empty() || !logged_in) return;

      g_StatusChecking = true;
      std::thread([key, hwid]() {
          const std::string encrypt_key = OO("JiM21rNU12eERlNmpqa3FuQks").str();
          const std::string ws_token    = OO("KJGMDKFJDHG34KD").str();
          const std::string version     = OO("1.0").str();
          const std::string api_url     = OO("https://baru-baru.vercel.app/api/validate").str();
          try {
              nlohmann::json inner = {
                  {OO("license_key").str(), key},
                  {OO("hwid").str(),        hwid},
                  {OO("game_type").str(),   OO("8ball").str()},
                  {OO("version").str(),     version},
              };
              std::string encoded = encryptPayload(inner, encrypt_key);
              nlohmann::json req  = {
                  {OO("token").str(), ws_token},
                  {OO("data").str(),  encoded},
              };
              std::string raw = httpPost(api_url, req.dump(), 10);
              if (raw.empty()) { g_StatusChecking = false; return; }

              nlohmann::json outer = nlohmann::json::parse(raw);
              if (!outer.contains("data") || !outer["data"].is_string()) {
                  g_StatusChecking = false; return;
              }
              nlohmann::json resp = decryptPayload(outer["data"].get<std::string>(), encrypt_key);
              if (resp.empty() || !resp.contains("status")) { g_StatusChecking = false; return; }

              if (resp["status"] == "success") {
                  auto d = resp["data"];
                  std::string sv = d.value("version","");
                  if (sv != version) {
                      g_LicenseStatusCode = LSC_VERSION_OLD;
                      g_LicenseStatusMsg  = OO("Update required: v").str() + sv;
                  } else {
                      g_LicenseStatusCode = LSC_OK;
                      g_LicenseStatusMsg  = "";
                      g_ExpTime = d.value("expiry_date","N/A");
                        g_ExpiryTimestamp = ParseExpiryDateUTC(g_ExpTime);
                        g_lastSyncTime = time(nullptr); // record sync time
                        // Update feature list from server
                        g_Features.clear();
                        if (d.contains("features") && d["features"].is_array()) {
                            for (auto& f : d["features"])
                                if (f.is_string()) g_Features.push_back(f.get<std::string>());
                        }
                  }
              } else {
                  std::string msg = resp.value("message","License error");
                  g_LicenseStatusMsg = msg;
                  std::string ml = msg;
                  for (auto& c : ml) c = (char)tolower((unsigned char)c);
                  if (ml.find("ban")   != std::string::npos) g_LicenseStatusCode = LSC_BANNED;
                  else if (ml.find("expir") != std::string::npos) g_LicenseStatusCode = LSC_EXPIRED;
                  else g_LicenseStatusCode = LSC_BANNED;
              }
          } catch (...) {}
          g_StatusChecking = false;
      }).detach();
  }
  
