#include <string>
#include <vector>
#include <sstream>
#include <array>
#include <cstring>

#include "server/socket.h"
#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#endif
#include "string.h"
#include "regexp.h"
#include "defer.h"

bool hostPointsToLocalServer(std::string host, const std::string &listen_address)
{
    if(host.size() > 1 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    const bool wildcard = listen_address == "0.0.0.0" || listen_address == "::";
    addrinfo hints {}, *targets = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if(getaddrinfo(host.c_str(), nullptr, &hints, &targets) != 0) return false;
    defer(freeaddrinfo(targets);)
    const auto ipv4 = [](const sockaddr *address) {
        return address->sa_family == AF_INET || (address->sa_family == AF_INET6 &&
            IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6 *>(address)->sin6_addr));
    };
    const auto key = [](const sockaddr *address) {
        std::array<unsigned char, 16> bytes {};
        if(address->sa_family == AF_INET6)
            std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in6 *>(address)->sin6_addr, 16);
        else if(address->sa_family == AF_INET)
        {
            bytes[10] = bytes[11] = 0xff;
            std::memcpy(bytes.data() + 12, &reinterpret_cast<const sockaddr_in *>(address)->sin_addr, 4);
        }
        return bytes;
    };
    const auto matches = [&](const sockaddr *address) {
        if(!address || (address->sa_family != AF_INET && address->sa_family != AF_INET6)) return false;
        if(listen_address == "0.0.0.0" && !ipv4(address)) return false;
        for(auto *target = targets; target; target = target->ai_next)
            if(key(target->ai_addr) == key(address)) return true;
        return false;
    };
    bool local = false;
    if(!wildcard)
    {
        addrinfo *bound = nullptr;
        if(getaddrinfo(listen_address.c_str(), nullptr, &hints, &bound) == 0)
        {
            for(auto *address = bound; address; address = address->ai_next)
                local = local || matches(address->ai_addr);
            freeaddrinfo(bound);
        }
    }
    else
    {
        for(auto *target = targets; target; target = target->ai_next)
        {
            const auto bytes = key(target->ai_addr);
            const bool unspecified = bytes[12] == 0 && bytes[13] == 0 && bytes[14] == 0 && bytes[15] == 0;
            local = local || (ipv4(target->ai_addr) && (bytes[12] == 127 || unspecified)) ||
                (listen_address == "::" && target->ai_family == AF_INET6 &&
                 (IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6 *>(target->ai_addr)->sin6_addr) ||
                  IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6 *>(target->ai_addr)->sin6_addr)));
        }
#ifdef _WIN32
        ULONG size = 16384;
        std::vector<unsigned char> storage(size);
        auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
        ULONG status = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size);
        if(status == ERROR_BUFFER_OVERFLOW)
        {
            storage.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
            status = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size);
        }
        if(status == NO_ERROR)
            for(auto *adapter = adapters; adapter; adapter = adapter->Next)
                for(auto *address = adapter->FirstUnicastAddress; address; address = address->Next)
                    local = local || matches(address->Address.lpSockaddr);
#else
        ifaddrs *interfaces = nullptr;
        if(getifaddrs(&interfaces) == 0)
        {
            for(auto *address = interfaces; address; address = address->ifa_next)
                local = local || matches(address->ifa_addr);
            freeifaddrs(interfaces);
        }
#endif
    }
    return local;
}

std::string hostnameToIPAddr(const std::string &host)
{
    int retVal;
    std::string retAddr;
    char cAddr[128] = {};
    struct sockaddr_in *target;
    struct sockaddr_in6 *target6;
    struct addrinfo hint = {}, *retAddrInfo, *cur;
    retVal = getaddrinfo(host.data(), NULL, &hint, &retAddrInfo);
    if(retVal != 0)
    {
        freeaddrinfo(retAddrInfo);
        return "";
    }

    for(cur = retAddrInfo; cur != NULL; cur = cur->ai_next)
    {
        if(cur->ai_family == AF_INET)
        {
            target = reinterpret_cast<struct sockaddr_in *>(cur->ai_addr);
            inet_ntop(AF_INET, &target->sin_addr, cAddr, sizeof(cAddr));
            break;
        }
        else if(cur->ai_family == AF_INET6)
        {
            target6 = reinterpret_cast<struct sockaddr_in6 *>(cur->ai_addr);
            inet_ntop(AF_INET6, &target6->sin6_addr, cAddr, sizeof(cAddr));
            break;
        }
    }
    retAddr.assign(cAddr);
    freeaddrinfo(retAddrInfo);
    return retAddr;
}

bool isIPv4(const std::string &address)
{
    return regMatch(address, "^(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3}$");
}

bool isIPv6(const std::string &address)
{
    std::vector<std::string> regLists = {"^(?:[0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}$", "^((?:[0-9A-Fa-f]{1,4}(:[0-9A-Fa-f]{1,4})*)?)::((?:([0-9A-Fa-f]{1,4}:)*[0-9A-Fa-f]{1,4})?)$", "^(::(?:[0-9A-Fa-f]{1,4})(?::[0-9A-Fa-f]{1,4}){5})|((?:[0-9A-Fa-f]{1,4})(?::[0-9A-Fa-f]{1,4}){5}::)$"};
    for(unsigned int i = 0; i < regLists.size(); i++)
    {
        if(regMatch(address, regLists[i]))
            return true;
    }
    return false;
}

void urlParse(std::string &url, std::string &host, std::string &path, int &port, bool &isTLS)
{
    std::vector<std::string> args;
    string_size pos;

    if(regMatch(url, "^https://(.*)"))
        isTLS = true;
    url = regReplace(url, "^(http|https)://", "");
    pos = url.find("/");
    if(pos == url.npos)
    {
        host = url;
        path = "/";
    }
    else
    {
        host = url.substr(0, pos);
        path = url.substr(pos);
    }
    pos = host.rfind(":");
    if(regFind(host, "\\[(.*)\\]")) //IPv6
    {
        args = split(regReplace(host, "\\[(.*)\\](.*)", "$1,$2"), ",");
        if(args.size() == 2) //with port
            port = to_int(args[1].substr(1));
        host = args[0];
    }
    else if(pos != host.npos)
    {
        port = to_int(host.substr(pos + 1));
        host = host.substr(0, pos);
    }
    if(port == 0)
    {
        if(isTLS)
            port = 443;
        else
            port = 80;
    }
}

std::string getFormData(const std::string &raw_data)
{
    std::stringstream strstrm;
    std::string line;

    std::string boundary;
    std::string file; /* actual file content */

    int i = 0;

    strstrm<<raw_data;

    while (std::getline(strstrm, line))
    {
        if(i == 0)
            boundary = line.substr(0, line.length() - 1); // Get boundary
        else if(startsWith(line, boundary))
            break; // The end
        else if(line.length() == 1)
        {
            // Time to get raw data
            char c;
            int bl = boundary.length();
            bool endfile = false;
            char buffer[256];
            while(!endfile)
            {
                int j = 0;
                while(j < 256 && strstrm.get(c) && !endfile)
                {
                    buffer[j] = c;
                    int k = 0;
                    // Verify if we are at the end
                    while(boundary[bl - 1 - k] == buffer[j - k])
                    {
                        if(k >= bl - 1)
                        {
                            // We are at the end of the file
                            endfile = true;
                            break;
                        }
                        k++;
                    }
                    j++;
                }
                file.append(buffer, j);
                j = 0;
            };
            file.erase(file.length() - bl);
            break;
        }
        i++;
    }
    return file;
}
