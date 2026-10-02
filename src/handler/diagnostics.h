#ifndef DIAGNOSTICS_H_INCLUDED
#define DIAGNOSTICS_H_INCLUDED

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

struct ConversionDiagnostics
{
    std::mutex mutex;
    std::string id;
    bool force_refresh = false;
    bool use_stale = false;
    nlohmann::json phases = nlohmann::json::array();
    nlohmann::json downloads = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json metrics = nlohmann::json::object();
};

std::shared_ptr<ConversionDiagnostics> currentDiagnostics();
std::string currentPhase();
std::string safeSource(const std::string &url);
std::string redactForLog(const std::string &message);
void recordDownload(const nlohmann::json &event);
void recordWarning(const std::string &message);
void recordMetric(const std::string &name, size_t value);
nlohmann::json diagnosticsJson(const std::shared_ptr<ConversionDiagnostics> &context);

class DiagnosticScope
{
    std::shared_ptr<ConversionDiagnostics> previous;
    std::string previous_phase;
public:
    explicit DiagnosticScope(std::shared_ptr<ConversionDiagnostics> context, std::string phase = "conversion");
    ~DiagnosticScope();
};

class PhaseTimer
{
    std::shared_ptr<ConversionDiagnostics> context;
    std::string phase, previous_phase;
    std::chrono::steady_clock::time_point start;
public:
    explicit PhaseTimer(std::string name);
    ~PhaseTimer();
};

std::shared_ptr<ConversionDiagnostics> newDiagnostics();

#endif
