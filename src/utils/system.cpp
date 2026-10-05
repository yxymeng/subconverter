#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif // _WIN32

#include "string.h"

void sleepMs(int interval)
{
    /*
    #ifdef _WIN32
        Sleep(interval);
    #else
        // Portable sleep for platforms other than Windows.
        struct timeval wait = { 0, interval * 1000 };
        select(0, NULL, NULL, NULL, &wait);
    #endif
    */
    //upgrade to c++11 standard
    std::this_thread::sleep_for(std::chrono::milliseconds(interval));
}

std::string getEnv(const std::string &name)
{
    std::string retVal;
#ifdef _WIN32
    char chrData[1024] = {};
    if(GetEnvironmentVariable(name.c_str(), chrData, 1023))
        retVal.assign(chrData);
#else
    char *env = getenv(name.c_str());
    if(env != NULL)
        retVal.assign(env);
#endif // _WIN32
    return retVal;
}

std::string getSystemProxy()
{
#ifdef _WIN32
    HKEY key = nullptr;
    if(RegOpenKeyExA(HKEY_CURRENT_USER, R"(Software\Microsoft\Windows\CurrentVersion\Internet Settings)", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return "";
    struct RegistryClose { HKEY key; ~RegistryClose() { RegCloseKey(key); } } close {key};
    DWORD enabled = 0, type = 0, size = sizeof(enabled);
    if(RegQueryValueExA(key, "ProxyEnable", nullptr, &type, reinterpret_cast<BYTE *>(&enabled), &size) != ERROR_SUCCESS ||
       type != REG_DWORD || !enabled) return "";
    size = 0;
    if(RegQueryValueExA(key, "ProxyServer", nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size > 65536)
        return "";
    std::string value(size, '\0');
    if(RegQueryValueExA(key, "ProxyServer", nullptr, &type, reinterpret_cast<BYTE *>(value.data()), &size) != ERROR_SUCCESS) return "";
    const auto terminator = value.find('\0');
    if(terminator != std::string::npos) value.resize(terminator);
    // WinINET can store one proxy or per-protocol proxies. Select HTTPS, then HTTP, then SOCKS.
    if(value.find('=') != std::string::npos)
    {
        for(const std::string &protocol : {"https", "http", "socks"})
            for(const auto &entry : split(value, ";"))
                if(startsWith(entry, protocol + "="))
                {
                    const auto selected = entry.substr(protocol.size() + 1);
                    return selected.find("://") == std::string::npos ? (protocol == "socks" ? "socks5h://" : "http://") + selected : selected;
                }
        return "";
    }
    return value.find("://") == std::string::npos ? "http://" + value : value;
#else
    string_array proxy_env = {"all_proxy", "ALL_PROXY", "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY"};
    for(std::string &x : proxy_env)
    {
        char* proxy = getenv(x.c_str());
        if(proxy != NULL && *proxy)
            return std::string(proxy);
    }
    return "";
#endif // _WIN32
}
