#include "diagnostics.h"
#include <atomic>
#include <regex>

static thread_local std::shared_ptr<ConversionDiagnostics> active_context;
static thread_local std::string active_phase = "download";

std::shared_ptr<ConversionDiagnostics> currentDiagnostics() { return active_context; }
std::string currentPhase() { return active_phase; }

std::shared_ptr<ConversionDiagnostics> newDiagnostics()
{
    static std::atomic<unsigned long long> sequence {0};
    auto context = std::make_shared<ConversionDiagnostics>();
    context->id = "conversion-" + std::to_string(++sequence);
    return context;
}

std::string safeSource(const std::string &url)
{
    if(url.find("://") == std::string::npos)
        return "[local source]";
    const auto start = url.find("://") + 3;
    auto end = url.find_first_of("/?#", start);
    if(end == std::string::npos) end = url.size();
    auto authority = url.substr(start, end - start);
    const auto credentials = authority.rfind('@');
    if(credentials != std::string::npos) authority.erase(0, credentials + 1);
    return url.substr(0, start) + authority + "/[redacted]";
}

std::string redactForLog(const std::string &message)
{
    static const std::regex nodes(R"((?:vmess|vless|trojan|ss|ssr|ssd|hysteria2?|hy2|tuic|anytls|netch)://[^\s'"<>|]+|data:[^\s'"<>|]+)", std::regex::icase);
    const auto sanitized = std::regex_replace(message, nodes, "[node data redacted]");
    // Source paths can themselves contain subscription credentials. Never log them.
    static const std::regex urls(R"((?:https?|socks[45]h?)://[^\s'"<>|]+)", std::regex::icase);
    static const std::regex sensitive(R"((authorization|proxy-authorization|cookie|set-cookie|password|token|api_key|secret)\s*[:=]\s*[^\r\n|]*)", std::regex::icase);
    std::string output;
    size_t offset = 0;
    for(std::sregex_iterator it(sanitized.begin(), sanitized.end(), urls), end; it != end; ++it)
    {
        output.append(sanitized, offset, it->position() - offset);
        output += safeSource(it->str());
        offset = it->position() + it->length();
    }
    output.append(sanitized, offset, std::string::npos);
    return std::regex_replace(output, sensitive, "$1=[redacted]");
}

DiagnosticScope::DiagnosticScope(std::shared_ptr<ConversionDiagnostics> context, std::string phase)
    : previous(active_context), previous_phase(active_phase)
{
    active_context = std::move(context);
    active_phase = std::move(phase);
}
DiagnosticScope::~DiagnosticScope() { active_context = previous; active_phase = previous_phase; }

PhaseTimer::PhaseTimer(std::string name)
    : context(active_context), phase(std::move(name)), previous_phase(active_phase), start(std::chrono::steady_clock::now())
{ active_phase = phase; }
PhaseTimer::~PhaseTimer()
{
    active_phase = previous_phase;
    if(!context) return;
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard<std::mutex> lock(context->mutex);
    context->phases.push_back({{"phase", phase}, {"duration_ms", ms}});
}

void recordDownload(const nlohmann::json &event)
{
    if(!active_context) return;
    std::lock_guard<std::mutex> lock(active_context->mutex);
    active_context->downloads.push_back(event);
}

void recordWarning(const std::string &message)
{
    if(!active_context) return;
    std::lock_guard<std::mutex> lock(active_context->mutex);
    active_context->warnings.push_back(redactForLog(message));
}

void recordMetric(const std::string &name, size_t value)
{
    if(!active_context) return;
    std::lock_guard<std::mutex> lock(active_context->mutex);
    active_context->metrics[name] = value;
}

nlohmann::json diagnosticsJson(const std::shared_ptr<ConversionDiagnostics> &context)
{
    std::lock_guard<std::mutex> lock(context->mutex);
    return {{"request_id", context->id}, {"phases", context->phases}, {"downloads", context->downloads}, {"warnings", context->warnings}, {"metrics", context->metrics}};
}
