#pragma once

#include <map>
#include <string>

using namespace std;

// const char* এর বদলে std::string ব্যবহার করা নিরাপদ যাতে মেমরি লিক না হয়
struct DynamicBool : map<string, bool> {
    DynamicBool() { }
    
    // operator[] এর বদলে get ফাংশন ব্যবহার করুন
    bool get(const char* key, bool defaultValue = false) {
        string sKey(key);
        auto it = find(sKey);
        if (it == end()) {
            insert({sKey, defaultValue});
            return defaultValue;
        }
        return it->second;
    }
};

struct DynamicString : map<string, string> {
    DynamicString() { }
    
    // operator[] এর বদলে get ফাংশন ব্যবহার করুন
    string get(const char* key, string defaultValue = "") {
        string sKey(key);
        auto it = find(sKey);
        if (it == end()) {
            insert({sKey, defaultValue});
            return defaultValue;
        }
        return it->second;
    }
};

static DynamicBool dynamic_bool;
static DynamicString dynamic_string;
