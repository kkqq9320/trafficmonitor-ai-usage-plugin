// DLL-level regression tests for Codex usage selection.
// Usage: CodexUsagePluginTests.exe <plugin-dll> <scenario>
//        CodexUsagePluginTests.exe <plugin-dll> --print   (live environment, prints values and tooltip)
// Scenarios run inside a temporary CODEX_HOME and LOCALAPPDATA so real helper files are never read.

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "PluginInterface.h"

namespace fs = std::filesystem;

namespace
{
using GetPluginInstance = ITMPlugin* (*)();

struct Fixture
{
    fs::path root;
    fs::path sessions;
    fs::path plugin_cache;
};

long long NowUnix()
{
    return static_cast<long long>(std::time(nullptr));
}

std::string IsoUtc(long long unix_seconds)
{
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    gmtime_s(&utc, &value);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
    return buffer;
}

std::string TokenCount(const std::string& timestamp, const std::string& rate_limits)
{
    return R"({"timestamp":")" + timestamp + R"(","type":"event_msg","payload":{"type":"token_count","info":null,"rate_limits":)" + rate_limits + "}}\n";
}

std::string TokenCountAt(long long unix_seconds, const std::string& rate_limits)
{
    return TokenCount(IsoUtc(unix_seconds), rate_limits);
}

std::string Weekly(int used, const char* limit_id = "codex", long long resets_at = 1893456000)
{
    return std::string(R"({"limit_id":")") + limit_id + R"(","primary":{"used_percent":)" + std::to_string(used) +
        R"(,"window_minutes":10080,"resets_at":)" + std::to_string(resets_at) + R"(},"secondary":null})";
}

void WriteText(const fs::path& path, const std::string& text, bool append = false)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    output << text;
}

void SetWriteTime(const fs::path& path, long long unix_seconds)
{
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<unsigned long long>(unix_seconds) * 10000000ULL + 116444736000000000ULL;
    FILETIME time{};
    time.dwLowDateTime = value.LowPart;
    time.dwHighDateTime = value.HighPart;
    SetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
}

std::string HelperSnapshot(long long data_at, const std::string& seven_day, const char* source, const char* method, const char* reached_type, bool limit_reached,
    const std::string& reset_credits = "null")
{
    return std::string("{\n") +
        R"(  "schema": 1, "limit_id": "codex", "source": ")" + source + R"(", "method": ")" + method + "\",\n" +
        R"(  "data_at": ")" + IsoUtc(data_at) + R"(", "data_at_unix": )" + std::to_string(data_at) + ",\n" +
        R"(  "fetched_at_unix": )" + std::to_string(data_at) + R"(, "plan_type": "pro", "rate_limit_reached_type": )" +
        (reached_type ? std::string("\"") + reached_type + "\"" : std::string("null")) + ",\n" +
        R"(  "limit_reached": )" + (limit_reached ? "true" : "false") + ",\n" +
        R"(  "five_hour": null,)" + "\n" +
        R"(  "seven_day": )" + seven_day + ",\n" +
        R"(  "reset_credits": )" + reset_credits + "\n}\n";
}

// Same shape as the Claude web helper's claude-web-usage.json.
std::string ClaudeSnapshot(int five_hour, int seven_day, const std::string& reset_credits = "null")
{
    auto window = [](int utilization) {
        return R"({ "utilization": )" + std::to_string(utilization) +
            R"(, "resets_at": "2030-01-01T00:00:00.000000+00:00", "limit_dollars": null, "used_dollars": null, "remaining_dollars": null, "locked_reason": null })";
    };
    return std::string("{\n") +
        R"(  "source": "claude-web-helper", "generated_at": ")" + IsoUtc(NowUnix()) + "\",\n" +
        R"(  "five_hour": )" + window(five_hour) + ",\n" +
        R"(  "seven_day": )" + window(seven_day) + ",\n" +
        R"(  "seven_day_sonnet": null, "extra_usage": null, "refresh_ms": 300000,)" + "\n" +
        R"(  "reset_credits": )" + reset_credits + "\n}\n";
}

std::wstring ClaudeTooltip(ITMPlugin* plugin)
{
    const std::wstring tooltip = plugin->GetTooltipInfo();
    return tooltip.substr(0, tooltip.find(L"Codex usage"));
}

bool ExpectClaudeTooltip(ITMPlugin* plugin, const wchar_t* expected, bool present)
{
    const std::wstring tooltip = ClaudeTooltip(plugin);
    if ((tooltip.find(expected) != std::wstring::npos) == present)
        return true;
    std::wcerr << L"Claude tooltip " << (present ? L"is missing" : L"unexpectedly contains") << L" \"" << expected << L"\":\n" << tooltip << L"\n";
    return false;
}

struct Scenario
{
    const wchar_t* name;
    std::function<void(const Fixture&)> build;
    const wchar_t* expected_5h;
    const wchar_t* expected_7d;
    std::vector<std::wstring> tooltip_contains;
    std::vector<std::wstring> tooltip_excludes;
    std::function<bool(const Fixture&, ITMPlugin*)> after;  // optional follow-up checks
};

const char* LEGACY_TIMESTAMP = "2026-07-27T08:12:08Z";

std::vector<Scenario> BuildScenarios()
{
    std::vector<Scenario> scenarios;
    auto legacy = [](const wchar_t* name, const char* rate_limits, const wchar_t* expected_5h, const wchar_t* expected_7d) {
        const std::string payload = rate_limits;
        return Scenario{ name, [payload](const Fixture& fixture) {
            WriteText(fixture.sessions / L"rollout-test.jsonl", TokenCount(LEGACY_TIMESTAMP, payload));
        }, expected_5h, expected_7d, {}, {}, nullptr };
    };

    scenarios.push_back(legacy(L"weekly-primary", R"({"primary":{"used_percent":30,"window_minutes":10080,"resets_at":1893456000},"secondary":null})", L"--", L"30%"));
    scenarios.push_back(legacy(L"both-windows", R"({"primary":{"used_percent":12,"window_minutes":300,"resets_at":1893456000},"secondary":{"used_percent":34,"window_minutes":10080,"resets_at":1893456000}})", L"12%", L"34%"));
    scenarios.push_back(legacy(L"swapped-windows", R"({"primary":{"used_percent":34,"window_minutes":10080,"resets_at":1893456000},"secondary":{"used_percent":12,"window_minutes":300,"resets_at":1893456000}})", L"12%", L"34%"));
    scenarios.push_back(legacy(L"legacy-no-window", R"({"primary":{"used_percent":12,"resets_at":1893456000},"secondary":{"used_percent":34,"resets_at":1893456000}})", L"12%", L"34%"));
    scenarios.push_back(legacy(L"unknown-window", R"({"primary":{"used_percent":50,"window_minutes":1440,"resets_at":1893456000},"secondary":null})", L"--", L"--"));

    // An old session reopened today (fresh mtime, old event) must not hide a newer event in a
    // session whose mtime Windows kept at open time.
    scenarios.push_back(Scenario{ L"mtime-touched-old-file", [](const Fixture& fixture) {
        const long long now = NowUnix();
        const fs::path touched = fixture.sessions / L"rollout-touched.jsonl";
        WriteText(touched, TokenCountAt(now - 3 * 3600, Weekly(89)));
        SetWriteTime(touched, now);
        const fs::path open = fixture.sessions / L"rollout-open.jsonl";
        WriteText(open, TokenCountAt(now - 40 * 60, Weekly(90)) + TokenCountAt(now - 31 * 60, Weekly(94)));
        SetWriteTime(open, now - 2 * 3600);
    }, L"--", L"94%", { L"session JSONL" }, {}, nullptr });

    scenarios.push_back(Scenario{ L"mixed-limit-ids", [](const Fixture& fixture) {
        const long long now = NowUnix();
        WriteText(fixture.sessions / L"rollout-mixed.jsonl",
            TokenCountAt(now - 20 * 60, Weekly(40)) +
            TokenCountAt(now - 10 * 60, R"({"limit_id":"codex_bengalfox","primary":{"used_percent":12,"window_minutes":300,"resets_at":1893456000},"secondary":{"used_percent":70,"window_minutes":10080,"resets_at":1893456000}})") +
            TokenCountAt(now - 5 * 60, R"({"limit_id":"premium","primary":null,"secondary":null,"credits":{"has_credits":false}})"));
    }, L"--", L"40%", {}, {}, nullptr });

    scenarios.push_back(Scenario{ L"null-windows-newer", [](const Fixture& fixture) {
        const long long now = NowUnix();
        WriteText(fixture.sessions / L"rollout-a.jsonl", TokenCountAt(now - 20 * 60, Weekly(77)));
        WriteText(fixture.sessions / L"rollout-routed.jsonl",
            TokenCountAt(now - 2 * 60, R"({"limit_id":"codex","primary":null,"secondary":null,"plan_type":null})"));
    }, L"--", L"77%", {}, {}, nullptr });

    scenarios.push_back(Scenario{ L"large-file-tail", [](const Fixture& fixture) {
        const long long now = NowUnix();
        WriteText(fixture.sessions / L"rollout-small.jsonl", TokenCountAt(now - 60 * 60, Weekly(20)));
        const fs::path big = fixture.sessions / L"rollout-big.jsonl";
        const std::string filler = R"({"timestamp":")" + IsoUtc(now - 30 * 60) + R"(","type":"response_item","payload":{"type":"message","text":")" + std::string(1024 * 1024, 'x') + "\"}}\n";
        {
            std::ofstream output(big, std::ios::binary);
            for (int index = 0; index < 33; ++index)
                output << filler;
            output << TokenCountAt(now - 5 * 60, Weekly(55));
        }
    }, L"--", L"55%", {}, {}, nullptr });

    scenarios.push_back(Scenario{ L"helper-snapshot", [](const Fixture& fixture) {
        const long long now = NowUnix();
        WriteText(fixture.sessions / L"rollout-old.jsonl", TokenCountAt(now - 24 * 3600, Weekly(89)));
        WriteText(fixture.plugin_cache / L"codex-usage.json",
            HelperSnapshot(now - 60, R"({ "used_percent": 100, "window_minutes": 10080, "resets_at": 1893456000 })", "server", "app-server", "rate_limit_reached", true));
    }, L"--", L"100%", { L"Limit reached (rate_limit_reached)", L"server (app-server)", L"plan pro" }, { L"(stale)", L"Reset credits" }, nullptr });

    // Free rate limit resets reported by the server are listed in the tooltip with their expiry.
    scenarios.push_back(Scenario{ L"helper-reset-credits", [](const Fixture& fixture) {
        WriteText(fixture.plugin_cache / L"codex-usage.json",
            HelperSnapshot(NowUnix() - 60, R"({ "used_percent": 100, "window_minutes": 10080, "resets_at": 1893456000 })", "server", "app-server", "rate_limit_reached", true,
                R"({ "available_count": 1, "earliest_expires_at": 1893456000 })"));
    }, L"--", L"100%", { L"Reset credits: 1 (expires " }, {}, nullptr });

    scenarios.push_back(Scenario{ L"helper-reset-credits-no-expiry", [](const Fixture& fixture) {
        WriteText(fixture.plugin_cache / L"codex-usage.json",
            HelperSnapshot(NowUnix() - 60, R"({ "used_percent": 40, "window_minutes": 10080, "resets_at": 1893456000 })", "server", "wham", nullptr, false,
                R"({ "available_count": 2, "earliest_expires_at": null })"));
    }, L"--", L"40%", { L"Reset credits: 2\n" }, { L"Reset credits: 2 (" }, nullptr });

    scenarios.push_back(Scenario{ L"stale-helper-uses-newer-jsonl", [](const Fixture& fixture) {
        const long long now = NowUnix();
        WriteText(fixture.plugin_cache / L"codex-usage.json",
            HelperSnapshot(now - 2 * 3600, R"({ "used_percent": 50, "window_minutes": 10080, "resets_at": 1893456000 })", "server", "wham", nullptr, false));
        WriteText(fixture.sessions / L"rollout-new.jsonl", TokenCountAt(now - 10 * 60, Weekly(60)));
    }, L"--", L"60%", { L"session JSONL" }, { L"(stale)" }, nullptr });

    scenarios.push_back(Scenario{ L"stale-and-reset-passed", [](const Fixture& fixture) {
        const long long now = NowUnix();
        WriteText(fixture.sessions / L"rollout-stale.jsonl", TokenCountAt(now - 2 * 3600, Weekly(70, "codex", now - 600)));
    }, L"--", L"70%", { L"(stale)", L"reset time passed" }, {}, nullptr });

    scenarios.push_back(Scenario{ L"helper-snapshot-update", [](const Fixture& fixture) {
        WriteText(fixture.plugin_cache / L"codex-usage.json",
            HelperSnapshot(NowUnix() - 30, R"({ "used_percent": 40, "window_minutes": 10080, "resets_at": 1893456000 })", "jsonl", "session-event", nullptr, false));
    }, L"--", L"40%", {}, {}, [](const Fixture& fixture, ITMPlugin* plugin) {
        WriteText(fixture.plugin_cache / L"codex-usage.json",
            HelperSnapshot(NowUnix(), R"({ "used_percent": 100, "window_minutes": 10080, "resets_at": 1893456000 })", "server", "app-server", "rate_limit_reached", true));
        Sleep(6000);
        plugin->DataRequired();
        const std::wstring actual = plugin->GetItem(3)->GetItemValueText();
        if (actual != L"100%")
        {
            std::wcerr << L"After snapshot update: expected \"100%\", got \"" << actual << L"\".\n";
            return false;
        }
        return true;
    } });

    // A new Claude helper snapshot must reach the taskbar within seconds, not on the next 30 s reload.
    scenarios.push_back(Scenario{ L"claude-snapshot-update", [](const Fixture& fixture) {
        WriteText(fixture.plugin_cache / L"claude-web-usage.json", ClaudeSnapshot(27, 17));
    }, L"--", L"--", {}, {}, [](const Fixture& fixture, ITMPlugin* plugin) {
        const std::wstring before = plugin->GetItem(0)->GetItemValueText();
        if (before != L"27%")
        {
            std::wcerr << L"Claude 5h before update: expected \"27%\", got \"" << before << L"\".\n";
            return false;
        }
        if (!ExpectClaudeTooltip(plugin, L"Reset credits", false))
            return false;
        WriteText(fixture.plugin_cache / L"claude-web-usage.json", ClaudeSnapshot(38, 19));
        Sleep(6000);
        plugin->DataRequired();
        const std::wstring after = plugin->GetItem(0)->GetItemValueText();
        if (after != L"38%")
        {
            std::wcerr << L"Claude 5h after snapshot update: expected \"38%\", got \"" << after << L"\".\n";
            return false;
        }
        return true;
    } });

    // Usage-limit reset grants reported by claude.ai are listed in the Claude tooltip with their expiry.
    scenarios.push_back(Scenario{ L"claude-reset-credits", [](const Fixture& fixture) {
        WriteText(fixture.plugin_cache / L"claude-web-usage.json",
            ClaudeSnapshot(72, 10, R"({ "available_count": 1, "earliest_expires_at": 1893456000 })"));
    }, L"--", L"--", {}, {}, [](const Fixture&, ITMPlugin* plugin) {
        return ExpectClaudeTooltip(plugin, L"Reset credits: 1 (expires ", true);
    } });

    scenarios.push_back(Scenario{ L"claude-reset-credits-no-expiry", [](const Fixture& fixture) {
        WriteText(fixture.plugin_cache / L"claude-web-usage.json",
            ClaudeSnapshot(72, 10, R"({ "available_count": 0, "earliest_expires_at": null })"));
    }, L"--", L"--", {}, {}, [](const Fixture&, ITMPlugin* plugin) {
        return ExpectClaudeTooltip(plugin, L"Reset credits: 0\n", true) && ExpectClaudeTooltip(plugin, L"Reset credits: 0 (", false);
    } });

    // A new snapshot that cannot be read yet (the file is held open) is read again on the next check.
    scenarios.push_back(Scenario{ L"claude-snapshot-read-retry", [](const Fixture& fixture) {
        WriteText(fixture.plugin_cache / L"claude-web-usage.json", ClaudeSnapshot(27, 17));
    }, L"--", L"--", {}, {}, [](const Fixture& fixture, ITMPlugin* plugin) {
        const fs::path snapshot = fixture.plugin_cache / L"claude-web-usage.json";
        WriteText(snapshot, ClaudeSnapshot(38, 19));
        HANDLE held = CreateFileW(snapshot.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (held == INVALID_HANDLE_VALUE)
        {
            std::wcerr << L"Could not hold the snapshot open.\n";
            return false;
        }
        Sleep(6000);
        plugin->DataRequired();
        CloseHandle(held);
        Sleep(6000);
        plugin->DataRequired();
        const std::wstring after = plugin->GetItem(0)->GetItemValueText();
        if (after != L"38%")
        {
            std::wcerr << L"Claude 5h after the snapshot became readable: expected \"38%\", got \"" << after << L"\".\n";
            return false;
        }
        return true;
    } });

    return scenarios;
}

bool ExpectValue(IPluginItem* item, const wchar_t* expected, const wchar_t* label)
{
    if (item == nullptr)
    {
        std::wcerr << label << L" item was not available.\n";
        return false;
    }
    const std::wstring actual = item->GetItemValueText();
    if (actual == expected)
        return true;
    std::wcerr << label << L": expected \"" << expected << L"\", got \"" << actual << L"\".\n";
    return false;
}

std::wstring CodexTooltip(ITMPlugin* plugin)
{
    const std::wstring tooltip = plugin->GetTooltipInfo();
    const size_t start = tooltip.find(L"Codex usage");
    return start == std::wstring::npos ? tooltip : tooltip.substr(start);
}

ITMPlugin* LoadPlugin(const wchar_t* dll_path)
{
    HMODULE module = LoadLibraryW(dll_path);
    if (module == nullptr)
    {
        std::wcerr << L"Failed to load plugin DLL: " << GetLastError() << L"\n";
        return nullptr;
    }
    const auto get_instance = reinterpret_cast<GetPluginInstance>(GetProcAddress(module, "TMPluginGetInstance"));
    if (get_instance == nullptr)
    {
        std::wcerr << L"TMPluginGetInstance export was not found.\n";
        return nullptr;
    }
    return get_instance();
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        std::wcerr << L"Usage: CodexUsagePluginTests.exe <plugin-dll> <scenario|--print|--list>\n";
        return 2;
    }

    const std::wstring scenario_name = argv[2];
    if (scenario_name == L"--print")
    {
        // Tooltips contain localized dates; write UTF-8 instead of the narrow C locale.
        _setmode(_fileno(stdout), _O_U8TEXT);
        ITMPlugin* plugin = LoadPlugin(argv[1]);
        if (plugin == nullptr)
            return 2;
        plugin->DataRequired();
        std::wcout << L"X5h=" << plugin->GetItem(2)->GetItemValueText() << L" X7d=" << plugin->GetItem(3)->GetItemValueText() << L"\n";
        std::wcout << plugin->GetTooltipInfo() << L"\n";
        return 0;
    }

    const std::vector<Scenario> scenarios = BuildScenarios();
    if (scenario_name == L"--list")
    {
        for (const Scenario& scenario : scenarios)
            std::wcout << scenario.name << L"\n";
        return 0;
    }

    const Scenario* scenario = nullptr;
    for (const Scenario& candidate : scenarios)
    {
        if (scenario_name == candidate.name)
            scenario = &candidate;
    }
    if (scenario == nullptr)
    {
        std::wcerr << L"Unknown scenario: " << scenario_name << L"\n";
        return 2;
    }

    Fixture fixture;
    fixture.root = fs::temp_directory_path() / (L"trafficmonitor-ai-usage-plugin-test-" + std::to_wstring(GetCurrentProcessId()));
    fixture.sessions = fixture.root / L"codex" / L"sessions" / L"2026" / L"09" / L"23";
    fixture.plugin_cache = fixture.root / L"local" / L"trafficmonitor-claude-usage-plugin";
    fs::create_directories(fixture.sessions);
    fs::create_directories(fixture.plugin_cache);
    scenario->build(fixture);

    const fs::path codex_home = fixture.root / L"codex";
    const fs::path local_app_data = fixture.root / L"local";
    if (!SetEnvironmentVariableW(L"CODEX_HOME", codex_home.c_str()) || !SetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.c_str()))
    {
        std::wcerr << L"Failed to set test environment.\n";
        return 2;
    }

    ITMPlugin* plugin = LoadPlugin(argv[1]);
    if (plugin == nullptr)
    {
        fs::remove_all(fixture.root);
        return 2;
    }
    plugin->DataRequired();

    bool passed =
        ExpectValue(plugin->GetItem(2), scenario->expected_5h, L"Codex 5h") &&
        ExpectValue(plugin->GetItem(3), scenario->expected_7d, L"Codex 7d");

    const std::wstring tooltip = CodexTooltip(plugin);
    for (const std::wstring& expected : scenario->tooltip_contains)
    {
        if (tooltip.find(expected) == std::wstring::npos)
        {
            std::wcerr << L"Tooltip is missing \"" << expected << L"\":\n" << tooltip << L"\n";
            passed = false;
        }
    }
    for (const std::wstring& unexpected : scenario->tooltip_excludes)
    {
        if (tooltip.find(unexpected) != std::wstring::npos)
        {
            std::wcerr << L"Tooltip unexpectedly contains \"" << unexpected << L"\":\n" << tooltip << L"\n";
            passed = false;
        }
    }
    if (passed && scenario->after)
        passed = scenario->after(fixture, plugin);

    std::error_code ignored;
    fs::remove_all(fixture.root, ignored);
    if (!passed)
        return 1;

    std::wcout << L"PASS " << scenario->name << L"\n";
    return 0;
}
