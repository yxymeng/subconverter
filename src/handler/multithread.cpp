#include <future>
#include <thread>
#include <queue>
#include <functional>
#include <condition_variable>
#include "handler/diagnostics.h"

#include "handler/settings.h"
#include "utils/network.h"
#include "webget.h"
#include "multithread.h"
//#include "vfs.h"

//safety lock for multi-thread
std::mutex on_emoji, on_rename, on_stream, on_time;

RegexMatchConfigs safe_get_emojis()
{
    guarded_mutex guard(on_emoji);
    return global.emojis;
}

RegexMatchConfigs safe_get_renames()
{
    guarded_mutex guard(on_rename);
    return global.renames;
}

RegexMatchConfigs safe_get_streams()
{
    guarded_mutex guard(on_stream);
    return global.streamNodeRules;
}

RegexMatchConfigs safe_get_times()
{
    guarded_mutex guard(on_time);
    return global.timeNodeRules;
}

void safe_set_emojis(RegexMatchConfigs data)
{
    guarded_mutex guard(on_emoji);
    global.emojis.swap(data);
}

void safe_set_renames(RegexMatchConfigs data)
{
    guarded_mutex guard(on_rename);
    global.renames.swap(data);
}

void safe_set_streams(RegexMatchConfigs data)
{
    guarded_mutex guard(on_stream);
    global.streamNodeRules.swap(data);
}

void safe_set_times(RegexMatchConfigs data)
{
    guarded_mutex guard(on_time);
    global.timeNodeRules.swap(data);
}

namespace {
class FetchExecutor
{
    std::mutex mutex;
    std::condition_variable ready;
    std::queue<std::function<void()>> tasks;
    std::vector<std::thread> workers;
    bool stopping = false;
public:
    FetchExecutor()
    {
        for(int i = 0; i < downloadSettings()->maxParallelDownloads; ++i)
            workers.emplace_back([this] {
                while(true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        ready.wait(lock, [this] { return stopping || !tasks.empty(); });
                        if(stopping && tasks.empty()) return;
                        task = std::move(tasks.front()); tasks.pop();
                    }
                    task();
                }
            });
    }
    ~FetchExecutor()
    {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        ready.notify_all();
        for(auto &worker : workers) worker.join();
    }
    std::shared_future<std::string> submit(std::function<std::string()> fn)
    {
        auto task = std::make_shared<std::packaged_task<std::string()>>(std::move(fn));
        auto result = task->get_future().share();
        { std::lock_guard<std::mutex> lock(mutex); tasks.emplace([task] { (*task)(); }); }
        ready.notify_one();
        return result;
    }
};
}

std::shared_future<std::string> fetchFileAsync(const std::string &path, const std::string &proxy, int cache_ttl, bool find_local, bool async, std::function<bool(const std::string &)> validate_content)
{
    static FetchExecutor executor;
    auto context = currentDiagnostics();
    auto phase = currentPhase();
    auto result = executor.submit([path, proxy, cache_ttl, find_local, context, phase, validate_content] {
        DiagnosticScope scope(context, phase);
        if(find_local && fileExist(path, true))
        {
            auto content = fileGet(path, true);
            return !validate_content || validate_content(content) ? content : std::string();
        }
        if(isLink(path)) return webGet(path, proxy, cache_ttl, nullptr, nullptr, validate_content);
        return std::string();
    });
    if(!async) result.wait();
    return result;
}

std::string fetchFile(const std::string &path, const std::string &proxy, int cache_ttl, bool find_local)
{
    return fetchFileAsync(path, proxy, cache_ttl, find_local, false).get();
}
