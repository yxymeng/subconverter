#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "server/socket.h"
#ifdef _WIN32
#include <windows.h>
#endif
//#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include "handler/diagnostics.h"

#include <curl/curl.h>

#include "handler/settings.h"
#include "utils/base64/base64.h"
#include "utils/defer.h"
#include "utils/file_extra.h"
#include "utils/lock.h"
#include "utils/logger.h"
#include "utils/network.h"
#include "utils/urlencode.h"
#include "version.h"
#include "webget.h"

#ifdef _WIN32
#ifndef _stat
#define _stat stat
#endif // _stat
#endif // _WIN32

std::mutex cache_rw_lock;
static const auto user_agent_str = "subconverter/" VERSION " cURL/" LIBCURL_VERSION;
static thread_local std::string latest_fetch_error;
std::string lastFetchError() { return latest_fetch_error; }

std::string sourceOrigin(const std::string &url)
{
    CURLU *parsed = curl_url();
    if(!parsed) return "";
    defer(curl_url_cleanup(parsed);)
    if(curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) return "";
    char *scheme = nullptr, *host = nullptr, *port = nullptr;
    defer(curl_free(scheme); curl_free(host); curl_free(port);)
    if(curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK ||
       curl_url_get(parsed, CURLUPART_HOST, &host, 0) != CURLUE_OK) return "";
    std::string origin = toLower(std::string(scheme) + "://" + host);
    if(curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_NO_DEFAULT_PORT) == CURLUE_OK)
        origin += ":" + std::string(port);
    return origin;
}

struct CacheFlight
{
    std::mutex mutex;
    std::atomic<unsigned long long> generation {0};
    std::string content, headers, error;
    bool success = false;
    int status_code = 0, transport_code = 0;
};
static std::mutex flights_mutex;
static std::map<std::string, std::weak_ptr<CacheFlight>> flights;
static std::shared_ptr<CacheFlight> cacheFlight(const std::string &key)
{
    std::lock_guard<std::mutex> lock(flights_mutex);
    auto flight = flights[key].lock();
    if(!flight) { flight = std::make_shared<CacheFlight>(); flights[key] = flight; }
    if(flights.size() > 256)
        for(auto it = flights.begin(); it != flights.end();)
            if(it->second.expired()) it = flights.erase(it); else ++it;
    return flight;
}

std::string fetchCacheKey(const std::string &url, const std::string &proxy, const string_icase_map *headers)
{
    std::string identity = url + "\n" + proxy;
    if(headers)
        for(const auto &[key, value] : *headers) identity += "\n" + toLower(key) + ":" + value;
    if(!headers || !headers->contains("User-Agent")) identity += "\nuser-agent:" + std::string(user_agent_str);
    return getMD5(identity);
}

class DownloadPermit
{
    static std::mutex mutex;
    static std::condition_variable ready;
    static int active;
public:
    static void settingsChanged()
    {
        // Synchronize with the wait predicate so a reload cannot lose its wakeup.
        std::lock_guard<std::mutex> lock(mutex);
        ready.notify_all();
    }
    DownloadPermit()
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [] { return active < downloadSettings()->maxParallelDownloads; });
        ++active;
    }
    ~DownloadPermit() { std::lock_guard<std::mutex> lock(mutex); --active; ready.notify_one(); }
};
std::mutex DownloadPermit::mutex;
std::condition_variable DownloadPermit::ready;
int DownloadPermit::active = 0;

void notifyDownloadSettingsChanged() { DownloadPermit::settingsChanged(); }


//std::string user_agent_str = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/74.0.3729.169 Safari/537.36";

struct curl_progress_data
{
    long size_limit = 0L;
};

static inline void curl_init()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_ALL); });
}

static size_t writer(char *data, size_t size, size_t nmemb, std::string *writerData)
{
    if(writerData == nullptr)
        return 0;

    writerData->append(data, size*nmemb);

    return size * nmemb;
}

struct ResponseHeaders
{
    CURL *handle;
    std::string *content;
    bool restrict_origin;
    bool blocked = false;
    bool self_reference = false;
};

static bool selfRequest(const std::string &url);

static bool selfEndpoint(const char *host, int port)
{
    return global.boundListenPort != 0 && port == global.boundListenPort &&
        hostPointsToLocalServer(host, global.boundListenAddress);
}

static curl_socket_t guardedSocket(void *client, curlsocktype, curl_sockaddr *address)
{
    auto *response = static_cast<ResponseHeaders *>(client);
    char host[NI_MAXHOST], port[NI_MAXSERV];
    if(getnameinfo(&address->addr, address->addrlen, host, sizeof(host), port, sizeof(port),
                   NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return CURL_SOCKET_BAD;
    if(selfEndpoint(host, to_int(port)))
    {
        response->blocked = response->self_reference = true;
        return CURL_SOCKET_BAD;
    }
    // Check the actual numeric connection address, including every redirect.
    return socket(address->family, address->socktype, address->protocol);
}

#if LIBCURL_VERSION_NUM >= 0x075000
static int guardedPeer(void *client, char *primary_ip, char *, int primary_port, int)
{
    auto *response = static_cast<ResponseHeaders *>(client);
    // This also runs before requests on reused connections.
    if(selfEndpoint(primary_ip, primary_port))
    {
        response->blocked = response->self_reference = true;
        return CURL_PREREQFUNC_ABORT;
    }
    return CURL_PREREQFUNC_OK;
}
#endif

static size_t headerWriter(char *data, size_t size, size_t nmemb, ResponseHeaders *response)
{
    const auto length = size * nmemb;
    const std::string line(data, length);
    response->content->append(line);
    long status = 0;
    curl_easy_getinfo(response->handle, CURLINFO_RESPONSE_CODE, &status);
    if(status >= 300 && status < 400 && startsWith(toLower(line), "location:"))
    {
        char *current_url = nullptr, *redirect_url = nullptr;
        curl_easy_getinfo(response->handle, CURLINFO_EFFECTIVE_URL, &current_url);
        CURLU *parsed = curl_url();
        defer(curl_url_cleanup(parsed); curl_free(redirect_url);)
        if(current_url && parsed && curl_url_set(parsed, CURLUPART_URL, current_url, 0) == CURLUE_OK &&
           curl_url_set(parsed, CURLUPART_URL, trimWhitespace(line.substr(9), true, true).c_str(), 0) == CURLUE_OK &&
           curl_url_get(parsed, CURLUPART_URL, &redirect_url, 0) == CURLUE_OK)
        {
            response->self_reference = selfRequest(redirect_url);
            if(response->self_reference || (response->restrict_origin && sourceOrigin(current_url) != sourceOrigin(redirect_url)))
            {
                response->blocked = true;
                return 0;
            }
        }
    }
    return length;
}

static size_t dummy_writer(char *, size_t size, size_t nmemb, void *)
{
    /// dummy writer, do not save anything
    return size * nmemb;
}

//static int size_checker(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
static int size_checker(void *clientp, curl_off_t, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    if(clientp)
    {
        auto *data = reinterpret_cast<curl_progress_data*>(clientp);
        if(data->size_limit)
        {
            if(dlnow > data->size_limit)
                return 1;
        }
    }
    return 0;
}

static inline void curl_set_common_options(CURL *curl_handle, const char *url, curl_progress_data *data)
{
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    // cURL debug text may echo cookie values or URL credentials even outside headers.
    // Emit our structured metadata instead of raw transport debug output.
    curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    const auto settings = downloadSettings();
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, static_cast<long>(settings->downloadTimeout));
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(settings->connectTimeout));
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl_handle, CURLOPT_COOKIELIST, "ALL");
    if(data)
    {
        if(data->size_limit)
            curl_easy_setopt(curl_handle, CURLOPT_MAXFILESIZE, data->size_limit);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, size_checker);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, data);
    }
}

static bool selfRequest(const std::string &url)
{
    if(global.boundListenPort == 0) return false;
    CURLU *parsed = curl_url();
    defer(curl_url_cleanup(parsed);)
    if(curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) return false;
    char *host = nullptr, *port = nullptr;
    defer(curl_free(host); curl_free(port);)
    if(curl_url_get(parsed, CURLUPART_HOST, &host, 0) != CURLUE_OK ||
       curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) != CURLUE_OK) return false;
    return to_int(port) == global.boundListenPort &&
        hostPointsToLocalServer(host, global.boundListenAddress);
}

static int curlGet(const FetchArgument &argument, FetchResult &result)
{
    const auto start = std::chrono::steady_clock::now();
    latest_fetch_error.clear();
    result.success = false;
    result.error.clear();
    curl_init();
    DownloadPermit permit;
    struct CurlDelete { void operator()(CURL *p) const { curl_easy_cleanup(p); } };
    // Reset request state while retaining this worker's DNS and connection cache.
    static thread_local std::unique_ptr<CURL, CurlDelete> reusable(curl_easy_init());
    CURL *handle = reusable.get();
    curl_easy_reset(handle);
    curl_slist *headers = nullptr;
    defer(curl_slist_free_all(headers);)
    std::string url = argument.url;
    curl_progress_data limit {global.maxAllowedDownloadSize};
    curl_set_common_options(handle, url.c_str(), &limit);
    // Empty means explicitly direct. SYSTEM resolves to a selected proxy before this layer.
    if(startsWith(argument.proxy, "cors:"))
    {
        url = argument.proxy.substr(5) + argument.url;
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        headers = curl_slist_append(headers, "X-Requested-With: subconverter " VERSION);
    }
    else
    {
        curl_easy_setopt(handle, CURLOPT_PROXY, argument.proxy.c_str());
        // A selected proxy is an explicit choice, including loopback destinations.
        curl_easy_setopt(handle, CURLOPT_NOPROXY, "");
    }
    bool has_user_agent = argument.request_headers && argument.request_headers->contains("User-Agent");
    if(!has_user_agent) curl_easy_setopt(handle, CURLOPT_USERAGENT, user_agent_str);
    if(argument.method == HTTP_POST || argument.method == HTTP_PATCH)
        headers = curl_slist_append(headers, "Content-Type: application/json;charset=utf-8");
    if(argument.request_headers)
        for(const auto &[key, value] : *argument.request_headers)
            headers = curl_slist_append(headers, (key + ": " + value).c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    std::string body, response_headers;
    ResponseHeaders response {handle, &response_headers, argument.restrict_origin};
    curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, guardedSocket);
    curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, &response);
#if LIBCURL_VERSION_NUM >= 0x075000
    curl_easy_setopt(handle, CURLOPT_PREREQFUNCTION, guardedPeer);
    curl_easy_setopt(handle, CURLOPT_PREREQDATA, &response);
#else
    // Older cURL has no per-request peer callback; require a guarded new socket.
    curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 1L);
#endif
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writer);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, headerWriter);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response);
    if(argument.cookies)
        for(const auto &cookie : split(*argument.cookies, "\r\n"))
            curl_easy_setopt(handle, CURLOPT_COOKIELIST, cookie.c_str());
    if(argument.method == HTTP_POST) curl_easy_setopt(handle, CURLOPT_POST, 1L);
    if(argument.method == HTTP_PATCH) curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PATCH");
    if(argument.method == HTTP_HEAD) curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    if(argument.post_data && (argument.method == HTTP_POST || argument.method == HTTP_PATCH))
    {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, argument.post_data->c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(argument.post_data->size()));
    }
    long code = 0;
    CURLcode transfer = CURLE_OK;
    const bool self = selfRequest(url);
    const bool blocked_cors = startsWith(argument.proxy, "cors:") && response.restrict_origin &&
        sourceOrigin(url) != sourceOrigin(argument.url);
    const unsigned int attempts = (argument.method == HTTP_GET || argument.method == HTTP_HEAD) ? 2 : 1;
    unsigned int tried = 0;
    for(; tried < attempts;)
    {
        ++tried;
        body.clear(); response_headers.clear();
        transfer = self || blocked_cors ? CURLE_TOO_MANY_REDIRECTS : curl_easy_perform(handle);
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
        if(transfer == CURLE_OK || transfer == CURLE_FILESIZE_EXCEEDED ||
           transfer == CURLE_ABORTED_BY_CALLBACK || self || blocked_cors || response.blocked) break;
    }
    result.transport_code = transfer;
    result.success = transfer == CURLE_OK && code >= 200 && code < 300;
    if(transfer != CURLE_OK) result.error = self || response.self_reference ? "Self-referencing conversion request rejected" : curl_easy_strerror(transfer);
    else if(!result.success) result.error = "HTTP " + std::to_string(code);
    if(response.blocked && !response.self_reference) result.error = "Cross-origin redirect with source-specific headers rejected";
    if(blocked_cors) result.error = "CORS relay with source-specific headers rejected";
    if(result.success && argument.validate_content && !argument.validate_content(body))
    {
        result.success = false;
        result.error = "Invalid ruleset content";
    }
    if(result.status_code) *result.status_code = static_cast<int>(code);
    if(result.content) *result.content = result.success || argument.keep_resp_on_fail ? body : "";
    if(result.response_headers) *result.response_headers = response_headers;
    if(result.cookies)
    {
        result.cookies->clear();
        curl_slist *cookies = nullptr;
        curl_easy_getinfo(handle, CURLINFO_COOKIELIST, &cookies);
        for(auto *each = cookies; each; each = each->next) *result.cookies += std::string(each->data) + "\r\n";
        curl_slist_free_all(cookies);
    }
    result.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.bytes = body.size();
    latest_fetch_error = result.error.empty() ? "" : "Download failed: " + result.error + " (" + safeSource(argument.url) + ")";
    const nlohmann::json event = {{"phase", currentPhase()}, {"source", safeSource(argument.url)}, {"source_id", getMD5(argument.url)},
        {"proxy", argument.proxy.empty() ? "direct" : safeSource(argument.proxy)},
        {"http_status", code}, {"transport_code", result.transport_code}, {"success", result.success},
        {"error", result.error}, {"duration_ms", result.duration_ms}, {"bytes", result.bytes}, {"attempts", tried}, {"cache", "network"}};
    recordDownload(event);
    writeLog(0, event.dump(), LOG_LEVEL_VERBOSE);
    if(!latest_fetch_error.empty()) writeLog(0, latest_fetch_error, LOG_LEVEL_WARNING);
    return transfer == CURLE_OK ? static_cast<int>(code) : 0;
}

// data:[<mediatype>][;base64],<data>
static std::string dataGet(const std::string &url)
{
    if (!startsWith(url, "data:"))
        return "";
    std::string::size_type comma = url.find(',');
    if (comma == std::string::npos || comma == url.size() - 1)
        return "";

    std::string data = urlDecode(url.substr(comma + 1));
    if (endsWith(url.substr(0, comma), ";base64")) {
        return urlSafeBase64Decode(data);
    } else {
        return data;
    }
}

std::string buildSocks5ProxyString(const std::string &addr, int port, const std::string &username, const std::string &password)
{
    std::string authstr = username.size() && password.size() ? username + ":" + password + "@" : "";
    std::string proxystr = "socks5://" + authstr + addr + ":" + std::to_string(port);
    return proxystr;
}

static bool readCache(const std::string &path, std::string &content, std::string &headers)
{
    const auto packed = fileGet(path, true);
    const auto line = packed.find('\n');
    if(line == std::string::npos) return false;
    try
    {
        const auto header_size = std::stoull(packed.substr(0, line));
        if(header_size > packed.size() - line - 1) return false;
        headers = packed.substr(line + 1, header_size);
        content = packed.substr(line + 1 + header_size);
        return !content.empty();
    }
    catch(const std::exception &) { return false; }
}

static void writeCache(const std::string &path, const std::string &body, const std::string &headers)
{
    const auto packed = std::to_string(headers.size()) + "\n" + headers + body;
    std::lock_guard<std::mutex> lock(cache_rw_lock);
    if(fileWriteAtomic(path, packed) != 0)
        writeLog(0, "Cannot write download cache", LOG_LEVEL_WARNING);
}

std::string webGet(const std::string &url, const std::string &proxy, unsigned int cache_ttl, std::string *response_headers, string_icase_map *request_headers, const std::function<bool(const std::string &)> &validate_content, bool restrict_origin)
{
    latest_fetch_error.clear();
    const auto valid = [&](const std::string &body) { return !validate_content || validate_content(body); };
    if(startsWith(url, "data:"))
    {
        auto body = dataGet(url);
        return valid(body) ? body : "";
    }
    const auto context = currentDiagnostics();
    const bool force = context && context->force_refresh;
    const bool rules = currentPhase() == "rules_download";
    const bool stale = rules && context && context->use_stale;
    const bool fallback = stale || (!rules && global.serveCacheOnFetchFail);
    int code = 0;
    std::string content, headers;
    FetchArgument argument {HTTP_GET, url, proxy, nullptr, request_headers, nullptr, cache_ttl, false, validate_content, restrict_origin};
    FetchResult result {&code, &content, &headers};
    if(cache_ttl == 0)
    {
        curlGet(argument, result);
        if(response_headers) *response_headers = headers;
        return content;
    }
    md("cache");
    const auto key = fetchCacheKey(url, proxy, request_headers);
    const auto path = "cache/v2-" + key;
    auto flight = cacheFlight(key);
    const auto generation = flight->generation.load();
    std::unique_lock<std::mutex> lock(flight->mutex);
    const auto cached = [&](const std::string &state, const std::string &body, const std::string &response) {
        if(response_headers) *response_headers = response;
        recordDownload({{"phase", currentPhase()}, {"source", safeSource(url)}, {"source_id", getMD5(url)},
            {"http_status", 200}, {"success", true}, {"bytes", body.size()}, {"duration_ms", 0}, {"cache", state}});
        return body;
    };
    if(flight->generation.load() != generation)
    {
        latest_fetch_error = flight->error;
        if(flight->success && valid(flight->content)) return cached("shared", flight->content, flight->headers);
        recordDownload({{"phase", currentPhase()}, {"source", safeSource(url)}, {"source_id", getMD5(url)},
            {"http_status", flight->status_code}, {"transport_code", flight->transport_code},
            {"success", false}, {"error", flight->error}, {"bytes", 0}, {"duration_ms", 0}, {"cache", "shared"}});
        if(fallback && readCache(path, content, headers) && valid(content)) return cached("stale", content, headers);
        return "";
    }
    struct stat info {};
    if(!force && stat(path.c_str(), &info) == 0 && (stale || difftime(time(nullptr), info.st_mtime) <= cache_ttl) && readCache(path, content, headers) && valid(content))
        return cached(stale ? "stale" : "hit", content, headers);
    curlGet(argument, result);
    flight->status_code = code;
    flight->transport_code = result.transport_code;
    if(result.success && !content.empty())
    {
        writeCache(path, content, headers);
        flight->success = true; flight->content = content; flight->headers = headers; flight->error.clear();
    }
    else
    {
        if(latest_fetch_error.empty()) latest_fetch_error = "Download returned empty content (" + safeSource(url) + ")";
        flight->success = false; flight->error = latest_fetch_error;
        // Required rules only fall back when explicitly requested, regardless of legacy defaults.
        if(fallback && readCache(path, content, headers) && valid(content))
            content = cached("stale", content, headers);
        else content.clear();
    }
    ++flight->generation;
    if(response_headers) *response_headers = headers;
    return content;
}

void flushCache()
{
    std::lock_guard<std::mutex> lock(cache_rw_lock);
    operateFiles("cache", [](const std::string &file){ std::remove(("cache/" + file).c_str()); return 0; });
}

int webPost(const std::string &url, const std::string &data, const std::string &proxy, const string_icase_map &request_headers, std::string *retData)
{
    //return curlPost(url, data, proxy, request_headers, retData);
    int return_code = 0;
    FetchArgument argument {HTTP_POST, url, proxy, &data, &request_headers, nullptr, 0, true};
    FetchResult fetch_res {&return_code, retData, nullptr, nullptr};
    return webGet(argument, fetch_res);
}

int webPatch(const std::string &url, const std::string &data, const std::string &proxy, const string_icase_map &request_headers, std::string *retData)
{
    //return curlPatch(url, data, proxy, request_headers, retData);
    int return_code = 0;
    FetchArgument argument {HTTP_PATCH, url, proxy, &data, &request_headers, nullptr, 0, true};
    FetchResult fetch_res {&return_code, retData, nullptr, nullptr};
    return webGet(argument, fetch_res);
}

int webHead(const std::string &url, const std::string &proxy, const string_icase_map &request_headers, std::string &response_headers)
{
    //return curlHead(url, proxy, request_headers, response_headers);
    int return_code = 0;
    FetchArgument argument {HTTP_HEAD, url, proxy, nullptr, &request_headers, nullptr, 0};
    FetchResult fetch_res {&return_code, nullptr, &response_headers, nullptr};
    return webGet(argument, fetch_res);
}

string_array headers_map_to_array(const string_map &headers)
{
    string_array result;
    for(auto &kv : headers)
        result.push_back(kv.first + ": " + kv.second);
    return result;
}

int webGet(const FetchArgument& argument, FetchResult &result)
{
    return curlGet(argument, result);
}
