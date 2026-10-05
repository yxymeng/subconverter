#include <iostream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "handler/diagnostics.h"
#include <string>
#include <unistd.h>
#include <csignal>

#include <sys/types.h>
#include <dirent.h>

#include "config/ruleset.h"
#include "handler/interfaces.h"
#include "handler/webget.h"
#include "handler/settings.h"
#include "script/cron.h"
#include "server/socket.h"
#include "server/webserver.h"
#include "utils/defer.h"
#include "utils/file_extra.h"
#include "utils/logger.h"
#include "utils/network.h"
#include "utils/rapidjson_extra.h"
#include "utils/system.h"
#include "utils/urlencode.h"
#include "version.h"

//#include "vfs.h"

WebServer webServer;
static std::string actual_config_path;

#ifndef _WIN32
void SetConsoleTitle(const std::string &title)
{
    system(std::string("echo \"\\033]0;" + title + R"(\007\c")").data());
}
#endif // _WIN32

void setcd(std::string &file)
{
    char szTemp[1024] = {}, filename[256] = {};
    std::string path;
#ifdef _WIN32
    char *pname = NULL;
    DWORD retVal = GetFullPathName(file.data(), 1023, szTemp, &pname);
    if(!retVal)
        return;
    strcpy(filename, pname);
    strrchr(szTemp, '\\')[1] = '\0';
#else
    char *ret = realpath(file.data(), szTemp);
    if(ret == nullptr)
        return;
    ret = strcpy(filename, strrchr(szTemp, '/') + 1);
    if(ret == nullptr)
        return;
    strrchr(szTemp, '/')[1] = '\0';
#endif // _WIN32
    file.assign(filename);
    path.assign(szTemp);
    chdir(path.data());
}

void chkArg(int argc, char *argv[])
{
    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "-cfw") == 0)
        {
            global.CFWChildProcess = true;
            global.updateRulesetOnRequest = true;
        }
        else if(strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0)
        {
            if(i < argc - 1)
                global.prefPath.assign(argv[++i]);
        }
        else if(strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--gen") == 0)
        {
            global.generatorMode = true;
        }
        else if(strcmp(argv[i], "--artifact") == 0)
        {
            if(i < argc - 1)
                global.generateProfiles.assign(argv[++i]);
        }
        else if(strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0)
        {
            if(i < argc - 1)
                if(freopen(argv[++i], "a", stderr) == nullptr)
                    std::cerr<<"Error redirecting output to file.\n";
        }
    }
}

void signal_handler(int sig)
{
    //std::cerr<<"Interrupt signal "<<sig<<" received. Exiting gracefully...\n";
    writeLog(0, "Interrupt signal " + std::to_string(sig) + " received. Exiting gracefully...", LOG_LEVEL_FATAL);
    switch(sig)
    {
#ifndef _WIN32
    case SIGHUP:
    case SIGQUIT:
#endif // _WIN32
    case SIGTERM:
    case SIGINT:
        webServer.stop_web_server();
        break;
    }
}

void cron_tick_caller()
{
    if(global.enableCron)
        cron_tick();
}

static nlohmann::json startupReport()
{
    const auto settings = downloadSettings();
    const auto endpoint = boundListenEndpoint();
    const auto proxy = [](const std::string &setting) {
        auto resolved = parseProxy(setting);
        return nlohmann::json {{"mode", setting == "NONE" || setting.empty() ? "direct" : setting == "SYSTEM" ? "system" : "explicit"},
            {"selected", resolved.empty() ? "direct" : safeSource(resolved)}};
    };
    return {{"version", VERSION}, {"build_commit", BUILD_COMMIT},
        {"config", actual_config_path},
        {"listen", endpoint->port ? endpoint->address : global.listenAddress},
        {"port", endpoint->port ? endpoint->port : global.listenPort},
        {"proxies", {{"subscription", proxy(global.proxySubscription)}, {"config", proxy(global.proxyConfig)}, {"ruleset", proxy(global.proxyRuleset)}}},
        {"max_parallel_downloads", settings->maxParallelDownloads}, {"download_timeout", settings->downloadTimeout},
        {"connect_timeout", settings->connectTimeout}};
}

int main(int argc, char *argv[])
{
#ifndef _DEBUG
    std::string prgpath = argv[0];
    setcd(prgpath); //first switch to program directory
#endif // _DEBUG
    if(fileExist("pref.toml"))
        global.prefPath = "pref.toml";
    else if(fileExist("pref.yml"))
        global.prefPath = "pref.yml";
    else if(!fileExist("pref.ini"))
    {
        if(fileExist("pref.example.toml"))
        {
            fileCopy("pref.example.toml", "pref.toml");
            global.prefPath = "pref.toml";
        }
        else if(fileExist("pref.example.yml"))
        {
            fileCopy("pref.example.yml", "pref.yml");
            global.prefPath = "pref.yml";
        }
        else if(fileExist("pref.example.ini"))
            fileCopy("pref.example.ini", "pref.ini");
    }
    chkArg(argc, argv);
    actual_config_path = std::filesystem::absolute(global.prefPath).string();
    setcd(global.prefPath); //then switch to pref directory
    writeLog(0, "SubConverter " VERSION " starting up..", LOG_LEVEL_INFO);
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
    {
        //std::cerr<<"WSAStartup failed.\n";
        writeLog(0, "WSAStartup failed.", LOG_LEVEL_FATAL);
        return 1;
    }
    UINT origcp = GetConsoleOutputCP();
    defer(SetConsoleOutputCP(origcp);)
    SetConsoleOutputCP(65001);
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
    signal(SIGHUP, signal_handler);
    signal(SIGQUIT, signal_handler);
#endif // _WIN32
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    bool check_only = false;
    for(int i = 1; i < argc; ++i) if(std::string(argv[i]) == "--check") check_only = true;
    if(!check_only) SetConsoleTitle("SubConverter " VERSION);
    if(!readConf()) return 1;
    const auto configured_port = getEnv("PORT");
    if(!configured_port.empty()) global.listenPort = to_int(configured_port, global.listenPort);
    if(global.listenPort < 1 || global.listenPort > 65535) { writeLog(0, "Invalid listening port", LOG_LEVEL_FATAL); return 1; }
    if(check_only) { std::cout << startupReport().dump(2) << "\n"; return 0; }
    //vfs::vfs_read("vfs.ini");
    // Rules are evaluated against their TTL when a conversion needs them.

    std::string env_api_mode = getEnv("API_MODE"), env_managed_prefix = getEnv("MANAGED_PREFIX"), env_token = getEnv("API_TOKEN");
    global.APIMode = tribool().parse(toLower(env_api_mode)).get(global.APIMode);
    if(!env_managed_prefix.empty())
        global.managedConfigPrefix = env_managed_prefix;
    if(!env_token.empty())
        global.accessToken = env_token;

    if(global.generatorMode)
        return simpleGenerator();

    /*
    webServer.append_response("GET", "/", "text/plain", [](RESPONSE_CALLBACK_ARGS) -> std::string
    {
        return "subconverter " VERSION " backend\n";
    });
    */

    webServer.append_response("GET", "/version", "text/plain", [](RESPONSE_CALLBACK_ARGS) -> std::string
    {
        return "subconverter " VERSION " backend\n";
    });

    webServer.append_response("GET", "/refreshrules", "text/plain", [](RESPONSE_CALLBACK_ARGS) -> std::string
    {
        if(!global.accessToken.empty())
        {
            std::string token = getUrlArg(request.argument, "token");
            if(token != global.accessToken)
            {
                response.status_code = 403;
                return "Forbidden\n";
            }
        }
        if(auto context = currentDiagnostics()) context->force_refresh = true;
        PhaseTimer rules_timer("rules_download");
        std::vector<RulesetContent> refreshed;
        refreshRulesets(global.customRulesets, refreshed);
        for(auto &ruleset : refreshed)
            if(ruleset.rule_content.get().empty()) throw std::runtime_error("Required ruleset update failed (" + safeSource(ruleset.rule_path) + ")");
        return "done\n";
    });

    webServer.append_response("GET", "/readconf", "text/plain", [](RESPONSE_CALLBACK_ARGS) -> std::string
    {
        if(!global.accessToken.empty())
        {
            std::string token = getUrlArg(request.argument, "token");
            if(token != global.accessToken)
            {
                response.status_code = 403;
                return "Forbidden\n";
            }
        }
        if(!readConf())
        {
            response.status_code = 400;
            return "Failed to reload configuration\n";
        }
        if(!global.updateRulesetOnRequest)
            refreshRulesets(global.customRulesets, global.rulesetsContent);
        return "done\n";
    });

    webServer.append_response("POST", "/updateconf", "text/plain", [](RESPONSE_CALLBACK_ARGS) -> std::string
    {
        if(!global.accessToken.empty())
        {
            std::string token = getUrlArg(request.argument, "token");
            if(token != global.accessToken)
            {
                response.status_code = 403;
                return "Forbidden\n";
            }
        }
        std::string type = getUrlArg(request.argument, "type");
        if(type == "form" || type == "direct")
        {
            if(!readConf(&request.postdata))
            {
                response.status_code = 400;
                return "Failed to reload configuration\n";
            }
        }
        else
        {
            response.status_code = 501;
            return "Not Implemented\n";
        }

        if(!global.updateRulesetOnRequest)
            refreshRulesets(global.customRulesets, global.rulesetsContent);
        return "done\n";
    });

    webServer.append_response("GET", "/flushcache", "text/plain", [](RESPONSE_CALLBACK_ARGS) -> std::string
    {
        if(getUrlArg(request.argument, "token") != global.accessToken)
        {
            response.status_code = 403;
            return "Forbidden";
        }
        flushCache();
        return "done";
    });

    webServer.append_response("GET", "/status", "application/json", [](RESPONSE_CALLBACK_ARGS) -> std::string
    {
        return startupReport().dump();
    });
    webServer.append_response("GET", "/diagnose", "application/json", subconverter);

    webServer.append_response("GET", "/sub", "text/plain;charset=utf-8", subconverter);

    webServer.append_response("HEAD", "/sub", "text/plain", subconverter);

    webServer.append_response("GET", "/sub2clashr", "text/plain;charset=utf-8", simpleToClashR);

    webServer.append_response("GET", "/surge2clash", "text/plain;charset=utf-8", surgeConfToClash);

    webServer.append_response("GET", "/getruleset", "text/plain;charset=utf-8", getRuleset);

    webServer.append_response("GET", "/getprofile", "text/plain;charset=utf-8", getProfile);

    webServer.append_response("GET", "/render", "text/plain;charset=utf-8", renderTemplate);

    if(!global.APIMode)
    {
        webServer.append_response("GET", "/get", "text/plain;charset=utf-8", [](RESPONSE_CALLBACK_ARGS) -> std::string
        {
            std::string url = urlDecode(getUrlArg(request.argument, "url"));
            return webGet(url, "");
        });

        webServer.append_response("GET", "/getlocal", "text/plain;charset=utf-8", [](RESPONSE_CALLBACK_ARGS) -> std::string
        {
            return fileGet(urlDecode(getUrlArg(request.argument, "path")));
        });
    }

    //webServer.append_response("POST", "/create-profile", "text/plain;charset=utf-8", createProfile);

    //webServer.append_response("GET", "/list-profiles", "text/plain;charset=utf-8", listProfiles);

    std::string env_port = getEnv("PORT");
    if(!env_port.empty())
        global.listenPort = to_int(env_port, global.listenPort);
    listener_args args = {global.listenAddress, global.listenPort, global.maxPendingConns, global.maxConcurThreads, cron_tick_caller, 200};
    //std::cout<<"Serving HTTP @ http://"<<listen_address<<":"<<listen_port<<std::endl;
    const int server_result = webServer.start_web_server_multi(&args);

#ifdef _WIN32
    WSACleanup();
#endif // _WIN32
    return server_result == 0 ? 0 : 1;
}
