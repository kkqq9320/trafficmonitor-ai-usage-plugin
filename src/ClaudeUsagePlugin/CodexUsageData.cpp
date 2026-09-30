#include "pch.h"
#include "CodexUsageData.h"
#include "HelperSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
using Metric = CCodexUsageData::Metric;
using RateLimitRecord = CCodexUsageData::RateLimitRecord;

constexpr unsigned long long CHECK_INTERVAL_MS = 5ULL * 1000ULL;
constexpr unsigned long long JSONL_REFRESH_INTERVAL_MS = 60ULL * 1000ULL;
constexpr unsigned long long REBUILD_INTERVAL_MS = 60ULL * 1000ULL;
constexpr long long STALE_AFTER_SECONDS = 30LL * 60LL;
constexpr unsigned long long MAX_SMALL_FILE_SIZE = 1024ULL * 1024ULL;
constexpr unsigned long long JSONL_CHUNK_BYTES = 1024ULL * 1024ULL;
constexpr unsigned long long JSONL_TAIL_MAX_BYTES = 16ULL * 1024ULL * 1024ULL;
// Codex keeps session files open while appending and Windows does not advance their mtime until
// the handle closes, so mtime only approximates "recently opened". The newest files are scanned
// and events are compared by their own timestamps.
constexpr size_t JSONL_SCAN_MAX_FILES = 32;
constexpr long long FIVE_HOUR_WINDOW_MINUTES = 300LL;
constexpr long long SEVEN_DAY_WINDOW_MINUTES = 10080LL;
constexpr long long WINDOW_TOLERANCE_MINUTES = 1LL;
constexpr double MAX_CREDITS_BALANCE = 1e15;
constexpr char CODEX_LIMIT_ID[] = "codex";
constexpr wchar_t SNAPSHOT_FILE_NAME[] = L"codex-usage.json";
constexpr wchar_t STATUS_FILE_NAME[] = L"codex-usage-helper-status.json";
constexpr wchar_t WATCH_LOCK_FILE_NAME[] = L"codex-usage-helper-watch.lock";
constexpr wchar_t HELPER_SCRIPT_NAME[] = L"codex-usage-helper.ps1";

// --- minimal JSON field access ------------------------------------------------

bool FindJsonKey(const std::string& json, const char* key, size_t& value_pos)
{
    std::string token = "\"";
    token += key;
    token += "\"";

    const size_t key_pos = json.find(token);
    if (key_pos == std::string::npos)
        return false;

    size_t colon_pos = key_pos + token.size();
    while (colon_pos < json.size() && isspace(static_cast<unsigned char>(json[colon_pos])))
        ++colon_pos;
    if (colon_pos >= json.size() || json[colon_pos] != ':')
        return false;

    value_pos = colon_pos + 1;
    while (value_pos < json.size() && isspace(static_cast<unsigned char>(json[value_pos])))
        ++value_pos;
    return value_pos < json.size();
}

size_t FindMatchingBracket(const std::string& text, size_t open_pos, char open_char, char close_char)
{
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    for (size_t index = open_pos; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (in_string)
        {
            if (escape)
                escape = false;
            else if (ch == '\\')
                escape = true;
            else if (ch == '"')
                in_string = false;
            continue;
        }
        if (ch == '"')
            in_string = true;
        else if (ch == open_char)
            ++depth;
        else if (ch == close_char && --depth == 0)
            return index;
    }
    return std::string::npos;
}

bool TryGetJsonDouble(const std::string& json, const char* key, double& value)
{
    size_t value_pos{};
    if (!FindJsonKey(json, key, value_pos))
        return false;
    const char ch = json[value_pos];
    if (ch != '-' && (ch < '0' || ch > '9'))
        return false;
    char* end_ptr{};
    value = strtod(json.c_str() + value_pos, &end_ptr);
    return end_ptr != json.c_str() + value_pos && std::isfinite(value);
}

bool TryGetJsonInt64(const std::string& json, const char* key, long long& value)
{
    double number{};
    if (!TryGetJsonDouble(json, key, number))
        return false;
    value = static_cast<long long>(number);
    return true;
}

bool TryGetJsonBool(const std::string& json, const char* key, bool& value)
{
    size_t value_pos{};
    if (!FindJsonKey(json, key, value_pos))
        return false;
    if (json.compare(value_pos, 4, "true") == 0)
    {
        value = true;
        return true;
    }
    if (json.compare(value_pos, 5, "false") == 0)
    {
        value = false;
        return true;
    }
    return false;
}

bool TryGetJsonObject(const std::string& json, const char* key, std::string& value)
{
    size_t value_pos{};
    if (!FindJsonKey(json, key, value_pos) || json[value_pos] != '{')
        return false;
    const size_t end_pos = FindMatchingBracket(json, value_pos, '{', '}');
    if (end_pos == std::string::npos)
        return false;
    value = json.substr(value_pos, end_pos - value_pos + 1);
    return true;
}

bool TryGetJsonString(const std::string& json, const char* key, std::string& value)
{
    size_t value_pos{};
    if (!FindJsonKey(json, key, value_pos) || json[value_pos] != '"')
        return false;

    std::string parsed;
    bool escape = false;
    for (size_t index = value_pos + 1; index < json.size(); ++index)
    {
        const char ch = json[index];
        if (escape)
        {
            parsed.push_back(ch);
            escape = false;
        }
        else if (ch == '\\')
        {
            escape = true;
        }
        else if (ch == '"')
        {
            value = parsed;
            return true;
        }
        else
        {
            parsed.push_back(ch);
        }
    }
    return false;
}

bool TryParseIso8601UtcSeconds(const std::string& timestamp, long long& unix_seconds)
{
    int year{}, month{}, day{}, hour{}, minute{}, second{};
    if (sscanf_s(timestamp.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6)
        return false;

    SYSTEMTIME utc_time{};
    utc_time.wYear = static_cast<WORD>(year);
    utc_time.wMonth = static_cast<WORD>(month);
    utc_time.wDay = static_cast<WORD>(day);
    utc_time.wHour = static_cast<WORD>(hour);
    utc_time.wMinute = static_cast<WORD>(minute);
    utc_time.wSecond = static_cast<WORD>(second);

    FILETIME file_time{};
    if (!SystemTimeToFileTime(&utc_time, &file_time))
        return false;
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < 116444736000000000ULL)
        return false;
    unix_seconds = static_cast<long long>((value.QuadPart - 116444736000000000ULL) / 10000000ULL);
    return true;
}

// --- rate limit parsing -------------------------------------------------------

bool LoadMetricObject(const std::string& metric_json, Metric& metric)
{
    double used_percent{};
    double remaining_percent{};
    if (TryGetJsonDouble(metric_json, "used_percent", used_percent))
        metric.percentage = used_percent;
    else if (TryGetJsonDouble(metric_json, "remaining_percent", remaining_percent))
        metric.percentage = 100.0 - remaining_percent;
    else
        return false;
    metric.percentage = (std::max)(0.0, (std::min)(100.0, metric.percentage));
    metric.available = true;

    long long window_minutes{};
    if (TryGetJsonInt64(metric_json, "window_minutes", window_minutes) && window_minutes > 0)
    {
        metric.has_window_minutes = true;
        metric.window_minutes = window_minutes;
    }

    long long reset_at{};
    if ((TryGetJsonInt64(metric_json, "resets_at", reset_at) || TryGetJsonInt64(metric_json, "reset_at", reset_at)) && reset_at > 0)
    {
        metric.has_reset_time = true;
        metric.reset_at_unix_seconds = reset_at;
    }
    return true;
}

bool ClassifyWindow(long long minutes, bool& is_five_hour)
{
    if (llabs(minutes - FIVE_HOUR_WINDOW_MINUTES) <= WINDOW_TOLERANCE_MINUTES)
    {
        is_five_hour = true;
        return true;
    }
    if (llabs(minutes - SEVEN_DAY_WINDOW_MINUTES) <= WINDOW_TOLERANCE_MINUTES)
    {
        is_five_hour = false;
        return true;
    }
    return false;
}

// Classifies by window length regardless of primary/secondary position; unknown lengths are dropped.
// Without window_minutes (legacy payloads) primary is 5h and secondary is 7d.
void AssignWindows(const Metric* primary, const Metric* secondary, RateLimitRecord& record)
{
    const Metric* slots[] = { primary, secondary };
    for (const Metric* metric : slots)
    {
        bool is_five_hour{};
        if (metric == nullptr || !metric->has_window_minutes || !ClassifyWindow(metric->window_minutes, is_five_hour))
            continue;
        Metric& target = is_five_hour ? record.rolling_5h : record.rolling_7d;
        if (!target.available)
        {
            target = *metric;
            target.window_minutes = is_five_hour ? FIVE_HOUR_WINDOW_MINUTES : SEVEN_DAY_WINDOW_MINUTES;
        }
    }
    if (primary != nullptr && !primary->has_window_minutes && !record.rolling_5h.available)
        record.rolling_5h = *primary;
    if (secondary != nullptr && !secondary->has_window_minutes && !record.rolling_7d.available)
        record.rolling_7d = *secondary;
}

bool ComputeLimitReached(const RateLimitRecord& record, bool reported)
{
    return reported ||
        !record.reached_type.empty() ||
        (record.rolling_5h.available && record.rolling_5h.percentage >= 100.0) ||
        (record.rolling_7d.available && record.rolling_7d.percentage >= 100.0);
}

// Codex credits object { has_credits, unlimited, balance }. The helper writes the balance as a
// number; session events keep the server's string.
void LoadCredits(const std::string& parent_json, RateLimitRecord& record)
{
    std::string credits_json;
    if (!TryGetJsonObject(parent_json, "credits", credits_json))
        return;
    record.has_credits_info = true;
    TryGetJsonBool(credits_json, "has_credits", record.has_credits);
    TryGetJsonBool(credits_json, "unlimited", record.credits_unlimited);

    double balance{};
    std::string balance_text;
    if (!TryGetJsonDouble(credits_json, "balance", balance))
    {
        if (!TryGetJsonString(credits_json, "balance", balance_text) || balance_text.empty())
            return;
        char* end_ptr{};
        balance = strtod(balance_text.c_str(), &end_ptr);
        if (end_ptr != balance_text.c_str() + balance_text.size() || !std::isfinite(balance))
            return;
    }
    // No real balance comes near this; larger values would also overflow the formatting buffers.
    if (std::fabs(balance) >= MAX_CREDITS_BALANCE)
        return;
    record.has_credits_balance = true;
    record.credits_balance = balance;
}

// app-server and wham/usage both read the account from OpenAI; the snapshot file keeps which one.
std::wstring DescribeSource(const std::string& source)
{
    if (source == "server")
        return L"Codex API";
    if (source == "jsonl")
        return L"Codex session log";
    return helper_support::Utf8ToWide(source);
}

bool ParseHelperSnapshot(const std::string& json, RateLimitRecord& record)
{
    record = RateLimitRecord{};
    std::string limit_id;
    if (TryGetJsonString(json, "limit_id", limit_id) && limit_id != CODEX_LIMIT_ID)
        return false;
    if (!TryGetJsonInt64(json, "data_at_unix", record.data_at_unix) || record.data_at_unix <= 0)
        return false;

    std::string window_json;
    Metric five_hour;
    if (TryGetJsonObject(json, "five_hour", window_json) && LoadMetricObject(window_json, five_hour))
    {
        five_hour.has_window_minutes = true;
        five_hour.window_minutes = FIVE_HOUR_WINDOW_MINUTES;
        record.rolling_5h = five_hour;
    }
    Metric seven_day;
    if (TryGetJsonObject(json, "seven_day", window_json) && LoadMetricObject(window_json, seven_day))
    {
        seven_day.has_window_minutes = true;
        seven_day.window_minutes = SEVEN_DAY_WINDOW_MINUTES;
        record.rolling_7d = seven_day;
    }

    std::string source;
    std::string text;
    TryGetJsonString(json, "source", source);
    record.source = DescribeSource(source);
    if (TryGetJsonString(json, "plan_type", text))
        record.plan_type = helper_support::Utf8ToWide(text);
    if (TryGetJsonString(json, "rate_limit_reached_type", text))
        record.reached_type = helper_support::Utf8ToWide(text);
    bool reported_reached = false;
    TryGetJsonBool(json, "limit_reached", reported_reached);
    record.limit_reached = ComputeLimitReached(record, reported_reached);

    std::string credits_json;
    if (TryGetJsonObject(json, "reset_credits", credits_json) &&
        TryGetJsonInt64(credits_json, "available_count", record.reset_credits) && record.reset_credits >= 0)
    {
        record.has_reset_credits = true;
        record.has_reset_credits_expiry =
            TryGetJsonInt64(credits_json, "earliest_expires_at", record.reset_credits_expires_at) && record.reset_credits_expires_at > 0;
    }
    LoadCredits(json, record);
    record.valid = true;
    return true;
}

bool IsCodexRateLimitEventLine(const std::string& line)
{
    return
        line.find("\"event_msg\"") != std::string::npos &&
        line.find("\"token_count\"") != std::string::npos &&
        line.find("\"rate_limits\"") != std::string::npos;
}

// Newest-event candidate from one session JSONL line: only the "codex" bucket with a real window.
bool ParseRateLimitEventLine(const std::string& line, RateLimitRecord& record)
{
    if (!IsCodexRateLimitEventLine(line))
        return false;

    std::string rate_limits;
    if (!TryGetJsonObject(line, "rate_limits", rate_limits))
        return false;
    std::string limit_id;
    if (TryGetJsonString(rate_limits, "limit_id", limit_id) && limit_id != CODEX_LIMIT_ID)
        return false;

    record = RateLimitRecord{};
    std::string window_json;
    Metric primary;
    Metric secondary;
    const bool has_primary = TryGetJsonObject(rate_limits, "primary", window_json) && LoadMetricObject(window_json, primary);
    const bool has_secondary = TryGetJsonObject(rate_limits, "secondary", window_json) && LoadMetricObject(window_json, secondary);
    AssignWindows(has_primary ? &primary : nullptr, has_secondary ? &secondary : nullptr, record);
    if (!record.rolling_5h.available && !record.rolling_7d.available)
        return false;

    std::string timestamp;
    if (!TryGetJsonString(line, "timestamp", timestamp) || !TryParseIso8601UtcSeconds(timestamp, record.data_at_unix))
        return false;

    std::string text;
    if (TryGetJsonString(rate_limits, "plan_type", text))
        record.plan_type = helper_support::Utf8ToWide(text);
    if (TryGetJsonString(rate_limits, "rate_limit_reached_type", text))
        record.reached_type = helper_support::Utf8ToWide(text);
    record.limit_reached = ComputeLimitReached(record, false);
    LoadCredits(rate_limits, record);
    record.source = L"Codex session log";
    record.valid = true;
    return true;
}

// --- session JSONL files ------------------------------------------------------

std::wstring NormalizePossibleWslPath(const std::wstring& path)
{
    if (path.size() >= 7 && path.compare(0, 5, L"/mnt/") == 0 && iswalpha(path[5]) && path[6] == L'/')
    {
        std::wstring converted(1, static_cast<wchar_t>(towupper(path[5])));
        converted += L":";
        converted += path.substr(6);
        std::replace(converted.begin(), converted.end(), L'/', L'\\');
        return converted;
    }
    return path;
}

std::wstring ExpandEnvironmentVariablesText(const std::wstring& text)
{
    if (text.find(L'%') == std::wstring::npos)
        return text;
    const DWORD required_size = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (required_size <= 1)
        return text;
    std::wstring expanded(required_size, L'\0');
    if (ExpandEnvironmentStringsW(text.c_str(), &expanded[0], required_size) == 0)
        return text;
    if (!expanded.empty() && expanded.back() == L'\0')
        expanded.pop_back();
    return expanded;
}

std::wstring GetCodexSessionsDir()
{
    std::wstring config_dir = NormalizePossibleWslPath(ExpandEnvironmentVariablesText(helper_support::TrimString(helper_support::GetEnvVar(L"CODEX_HOME"))));
    if (config_dir.empty())
    {
        const std::wstring home = helper_support::TrimString(helper_support::GetEnvVar(L"USERPROFILE"));
        if (home.empty())
            return std::wstring();
        config_dir = helper_support::JoinPath(home, L".codex");
    }
    return helper_support::JoinPath(config_dir, L"sessions");
}

struct SessionFileCandidate
{
    std::wstring path;
    unsigned long long write_time{};
    unsigned long long size{};
};

void CollectJsonlFilesRecursive(const std::wstring& root_dir, std::vector<SessionFileCandidate>& candidates)
{
    WIN32_FIND_DATAW find_data{};
    HANDLE handle = FindFirstFileW(helper_support::JoinPath(root_dir, L"*").c_str(), &find_data);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    do
    {
        const wchar_t* name = find_data.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
            continue;
        const std::wstring child_path = helper_support::JoinPath(root_dir, name);
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            CollectJsonlFilesRecursive(child_path, candidates);
            continue;
        }
        const size_t length = wcslen(name);
        if (length < 6 || _wcsicmp(name + length - 6, L".jsonl") != 0)
            continue;

        SessionFileCandidate candidate;
        candidate.path = child_path;
        candidate.write_time = (static_cast<unsigned long long>(find_data.ftLastWriteTime.dwHighDateTime) << 32) | find_data.ftLastWriteTime.dwLowDateTime;
        candidate.size = (static_cast<unsigned long long>(find_data.nFileSizeHigh) << 32) | find_data.nFileSizeLow;
        candidates.push_back(candidate);
    }
    while (FindNextFileW(handle, &find_data));
    FindClose(handle);
}

class SharedReadFile
{
public:
    explicit SharedReadFile(const std::wstring& path)
        : m_handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    ~SharedReadFile()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }
    SharedReadFile(const SharedReadFile&) = delete;
    SharedReadFile& operator=(const SharedReadFile&) = delete;

    bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

    bool ReadAt(unsigned long long offset, std::string& buffer, size_t length)
    {
        buffer.resize(length);
        size_t total{};
        while (total < length)
        {
            OVERLAPPED overlapped{};
            const unsigned long long position = offset + total;
            overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
            overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
            DWORD read_now{};
            if (!ReadFile(m_handle, &buffer[total], static_cast<DWORD>(length - total), &read_now, &overlapped) || read_now == 0)
                break;
            total += read_now;
        }
        buffer.resize(total);
        return total == length;
    }

private:
    HANDLE m_handle;
};

// Walks complete lines from the end of the file (newest first) and keeps the first valid event.
// parsed_size ends right after the last complete line so later refreshes read only appended bytes.
void ScanFileTail(const std::wstring& path, unsigned long long size, bool& has_event, RateLimitRecord& event, unsigned long long& parsed_size)
{
    has_event = false;
    event = RateLimitRecord{};
    parsed_size = 0;
    SharedReadFile file(path);
    if (!file.IsOpen())
        return;

    unsigned long long end = size;
    unsigned long long scanned = 0;
    bool seen_last_newline = false;
    std::string carry;
    std::string chunk;
    while (end > 0 && scanned < JSONL_TAIL_MAX_BYTES)
    {
        const unsigned long long start = end > JSONL_CHUNK_BYTES ? end - JSONL_CHUNK_BYTES : 0;
        if (!file.ReadAt(start, chunk, static_cast<size_t>(end - start)))
            return;
        scanned += end - start;
        std::string data = chunk + carry;

        size_t line_end = data.size();
        if (!seen_last_newline)
        {
            const size_t last_newline = data.rfind('\n');
            if (last_newline == std::string::npos)
            {
                carry.swap(data);
                end = start;
                continue;
            }
            seen_last_newline = true;
            parsed_size = start + last_newline + 1;
            line_end = last_newline;
        }

        while (line_end > 0)
        {
            const size_t newline = data.rfind('\n', line_end - 1);
            if (newline == std::string::npos)
                break;
            RateLimitRecord candidate;
            if (ParseRateLimitEventLine(data.substr(newline + 1, line_end - newline - 1), candidate))
            {
                has_event = true;
                event = candidate;
                return;
            }
            line_end = newline;
        }

        carry = data.substr(0, line_end);
        end = start;
        if (start == 0 && !carry.empty())
        {
            RateLimitRecord candidate;
            if (ParseRateLimitEventLine(carry, candidate))
            {
                has_event = true;
                event = candidate;
            }
            return;
        }
    }
}

// Parses bytes appended since the last refresh.
void ScanAppendedBytes(const std::wstring& path, unsigned long long from, unsigned long long to, bool& has_event, RateLimitRecord& event, unsigned long long& parsed_size)
{
    if (to - from > JSONL_TAIL_MAX_BYTES)
    {
        ScanFileTail(path, to, has_event, event, parsed_size);
        return;
    }
    SharedReadFile file(path);
    std::string data;
    if (!file.IsOpen() || !file.ReadAt(from, data, static_cast<size_t>(to - from)))
        return;
    const size_t last_newline = data.rfind('\n');
    if (last_newline == std::string::npos)
        return;

    size_t line_start = 0;
    while (line_start <= last_newline)
    {
        const size_t newline = data.find('\n', line_start);
        RateLimitRecord candidate;
        if (ParseRateLimitEventLine(data.substr(line_start, newline - line_start), candidate) &&
            (!has_event || candidate.data_at_unix >= event.data_at_unix))
        {
            has_event = true;
            event = candidate;
        }
        line_start = newline + 1;
    }
    parsed_size = from + last_newline + 1;
}

// --- presentation -------------------------------------------------------------

// Non-negative balance as the item and the tooltip write it: 62,500 or 12.75.
std::wstring FormatCreditsExact(double balance)
{
    wchar_t buffer[64];
    swprintf_s(buffer, L"%.2f", balance);
    std::wstring text = buffer;
    const size_t dot = text.find(L'.');
    std::wstring integer = text.substr(0, dot);
    std::wstring fraction = dot == std::wstring::npos ? std::wstring() : text.substr(dot);
    while (!fraction.empty() && (fraction.back() == L'0' || fraction.back() == L'.'))
        fraction.pop_back();
    for (size_t pos = integer.size(); pos > 3; pos -= 3)
        integer.insert(pos - 3, L",");
    return integer + fraction;
}

// "pro" -> "Pro"
std::wstring PlanName(const std::wstring& plan_type)
{
    std::wstring name = plan_type;
    if (!name.empty())
        name[0] = static_cast<wchar_t>(towupper(name[0]));
    return name;
}

std::wstring BuildHelperNote(bool helper_fresh)
{
    std::string status_json;
    std::string state;
    std::string retry_after;
    if (helper_support::ReadFileShared(helper_support::GetPluginCachePath(STATUS_FILE_NAME), MAX_SMALL_FILE_SIZE, status_json))
    {
        TryGetJsonString(status_json, "state", state);
        std::string server_json;
        if (TryGetJsonObject(status_json, "server", server_json))
            TryGetJsonString(server_json, "retry_after_until", retry_after);
    }

    const bool running = helper_support::IsWatchLockProcessRunning(helper_support::GetPluginCachePath(WATCH_LOCK_FILE_NAME));
    std::wstring note;
    if (state == "rate_limited")
    {
        note = L"Codex usage helper: server rate limited";
        long long retry_unix{};
        std::wstring retry_text;
        if (!retry_after.empty() && TryParseIso8601UtcSeconds(retry_after, retry_unix) && helper_support::UnixSecondsToLocalText(retry_unix, retry_text))
            note += L", retry after " + retry_text;
    }
    else if (state == "auth_required")
        note = L"Codex usage helper: sign in to Codex again";
    else if (state == "request_failed")
        note = L"Codex usage helper: server request failed";
    else if (state == "node_missing")
        note = L"Codex usage helper: Node.js 22+ not found (see helper-config.json)";
    else if (state == "crashed")
        note = L"Codex usage helper crashed";
    else if (!state.empty() && state != "ok")
        note = L"Codex usage helper: " + helper_support::Utf8ToWide(state);

    if (!running && !helper_fresh)
        note += note.empty() ? L"Codex usage helper is not running" : L" (helper not running)";
    return note;
}
} // namespace

CCodexUsageData& CCodexUsageData::Instance()
{
    static CCodexUsageData instance;
    return instance;
}

void CCodexUsageData::AutoStartBundledHelperIfNeeded()
{
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (m_helper_auto_start_attempted)
            return;
        m_helper_auto_start_attempted = true;
    }

    if (helper_support::IsWatchLockProcessRunning(helper_support::GetPluginCachePath(WATCH_LOCK_FILE_NAME)))
        return;
    const std::wstring script_path = helper_support::FindBundledScript(HELPER_SCRIPT_NAME);
    if (!script_path.empty())
        helper_support::LaunchBundledScript(script_path, L"start");
}

void CCodexUsageData::RefreshIfNeeded()
{
    const unsigned long long now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (m_refresh_in_progress)
            return;
        if (m_last_check_tick != 0 && now - m_last_check_tick < CHECK_INTERVAL_MS)
            return;
        m_last_check_tick = now;
        m_refresh_in_progress = true;
    }

    Refresh(now);

    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_refresh_in_progress = false;
}

void CCodexUsageData::Refresh(unsigned long long now_tick)
{
    const bool helper_changed = ReloadHelperSnapshotIfChanged();
    const long long now_unix = helper_support::GetUnixNowSeconds();
    const bool helper_fresh = m_helper_record.valid && now_unix - m_helper_record.data_at_unix <= STALE_AFTER_SECONDS;

    bool scanned = false;
    if (!helper_fresh && (m_last_jsonl_tick == 0 || now_tick - m_last_jsonl_tick >= JSONL_REFRESH_INTERVAL_MS))
    {
        ScanSessionJsonl();
        m_last_jsonl_tick = now_tick;
        scanned = true;
    }

    if (!helper_changed && !scanned && m_last_build_tick != 0 && now_tick - m_last_build_tick < REBUILD_INTERVAL_MS)
        return;
    m_last_build_tick = now_tick;

    const RateLimitRecord* best = m_helper_record.valid ? &m_helper_record : nullptr;
    if (!helper_fresh && m_jsonl_record.valid && (best == nullptr || m_jsonl_record.data_at_unix > best->data_at_unix))
        best = &m_jsonl_record;

    std::wstring error_text = m_jsonl_error;
    if (error_text.empty())
        error_text = L"Codex usage data unavailable";
    const Snapshot snapshot = BuildSnapshot(best, error_text, BuildHelperNote(helper_fresh));

    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_snapshot = snapshot;
}

bool CCodexUsageData::ReloadHelperSnapshotIfChanged()
{
    const std::wstring path = helper_support::GetPluginCachePath(SNAPSHOT_FILE_NAME);
    unsigned long long write_time{};
    if (!helper_support::GetFileWriteTime(path, write_time))
    {
        if (!m_helper_snapshot_exists)
            return false;
        m_helper_snapshot_exists = false;
        m_helper_record = RateLimitRecord{};
        return true;
    }
    if (m_helper_snapshot_exists && write_time == m_helper_snapshot_write_time)
        return false;

    std::string json;
    if (!helper_support::ReadFileShared(path, MAX_SMALL_FILE_SIZE, json))
        return false;  // likely being replaced; retry on the next check

    RateLimitRecord record;
    if (!ParseHelperSnapshot(json, record))
        record = RateLimitRecord{};
    m_helper_record = record;
    m_helper_snapshot_exists = true;
    m_helper_snapshot_write_time = write_time;
    return true;
}

void CCodexUsageData::ScanSessionJsonl()
{
    const std::wstring sessions_dir = GetCodexSessionsDir();
    if (sessions_dir.empty() || !helper_support::DirectoryExists(sessions_dir))
    {
        m_jsonl_record = RateLimitRecord{};
        m_jsonl_error = L"Codex sessions JSONL not found";
        m_session_files.clear();
        return;
    }

    std::vector<SessionFileCandidate> candidates;
    CollectJsonlFilesRecursive(sessions_dir, candidates);
    std::sort(candidates.begin(), candidates.end(), [](const SessionFileCandidate& left, const SessionFileCandidate& right) {
        return left.write_time > right.write_time;
    });
    if (candidates.size() > JSONL_SCAN_MAX_FILES)
        candidates.resize(JSONL_SCAN_MAX_FILES);

    std::map<std::wstring, SessionFileState> next_files;
    RateLimitRecord best;
    for (const SessionFileCandidate& candidate : candidates)
    {
        SessionFileState state;
        const auto cached = m_session_files.find(candidate.path);
        if (cached == m_session_files.end() || candidate.size < cached->second.parsed_size)
        {
            ScanFileTail(candidate.path, candidate.size, state.has_event, state.event, state.parsed_size);
        }
        else
        {
            state = cached->second;
            if (candidate.size > state.parsed_size)
                ScanAppendedBytes(candidate.path, state.parsed_size, candidate.size, state.has_event, state.event, state.parsed_size);
        }

        if (state.has_event && (!best.valid || state.event.data_at_unix > best.data_at_unix))
            best = state.event;
        next_files[candidate.path] = state;
    }
    m_session_files.swap(next_files);

    m_jsonl_record = best;
    m_jsonl_error = best.valid ? std::wstring() : L"Codex sessions JSONL has no codex rate limits yet";
}

CCodexUsageData::Snapshot CCodexUsageData::BuildSnapshot(const RateLimitRecord* record, const std::wstring& error_text, const std::wstring& helper_note) const
{
    Snapshot snapshot;
    const long long now_unix = helper_support::GetUnixNowSeconds();
    const bool has_record = record != nullptr && record->valid;
    const bool data_stale = has_record && now_unix - record->data_at_unix > STALE_AFTER_SECONDS;
    if (has_record)
    {
        snapshot.rolling_5h = record->rolling_5h;
        snapshot.rolling_7d = record->rolling_7d;
        Metric* metrics[] = { &snapshot.rolling_5h, &snapshot.rolling_7d };
        for (Metric* metric : metrics)
        {
            metric->stale = metric->available &&
                (data_stale || (metric->has_reset_time && metric->reset_at_unix_seconds <= now_unix));
        }
    }

    snapshot.value_5h_text = snapshot.rolling_5h.available ? usage_tooltip::FormatPercentage(snapshot.rolling_5h.percentage) : L"--";
    snapshot.value_7d_text = snapshot.rolling_7d.available ? usage_tooltip::FormatPercentage(snapshot.rolling_7d.percentage) : L"--";

    std::wstring credits_line;
    if (has_record && record->has_credits_info)
    {
        std::wstring exact_text;
        if (record->credits_unlimited)
        {
            snapshot.credits.value_text = L"\x221E";
            exact_text = L"unlimited";
        }
        else if (record->has_credits_balance)
        {
            exact_text = FormatCreditsExact((std::max)(0.0, record->credits_balance));
            snapshot.credits.value_text = exact_text;
        }
        else if (!record->has_credits)
        {
            snapshot.credits.value_text = L"0";
            exact_text = L"0";
        }
        if (!exact_text.empty())
        {
            snapshot.credits.available = true;
            snapshot.credits.stale = data_stale;
            credits_line = L"Credits: " + exact_text;
        }
    }
    if (has_record && record->has_reset_credits)
    {
        snapshot.reset_credits.available = true;
        snapshot.reset_credits.stale = data_stale;
        snapshot.reset_credits.count = record->reset_credits;
        snapshot.reset_credits.has_expiry = record->has_reset_credits_expiry;
        snapshot.reset_credits.expires_at = record->reset_credits_expires_at;
    }

    std::wstring updated_line;
    if (has_record)
    {
        updated_line = L"Updated: " + helper_support::FormatAgeText(now_unix - record->data_at_unix) + L", " + record->source;
        if (!record->plan_type.empty())
            updated_line += L", Plan " + PlanName(record->plan_type);
        if (data_stale)
            updated_line += L" (stale)";
    }

    std::vector<usage_tooltip::Window> windows;
    if (snapshot.rolling_5h.available)
        windows.push_back({ L"5h", L"5h", snapshot.rolling_5h.percentage, snapshot.rolling_5h.has_reset_time, snapshot.rolling_5h.reset_at_unix_seconds });
    if (snapshot.rolling_7d.available)
        windows.push_back({ L"7d", L"7d", snapshot.rolling_7d.percentage, snapshot.rolling_7d.has_reset_time, snapshot.rolling_7d.reset_at_unix_seconds });

    if (windows.empty())
    {
        snapshot.tooltip_text = std::wstring(usage_tooltip::CHART) + L" Codex usage unavailable";
        if (has_record)
        {
            snapshot.tooltip_text += L"\nNo 5h/7d window reported";
            if (!credits_line.empty())
                snapshot.tooltip_text += L"\n" + credits_line;
            snapshot.tooltip_text += L"\n\n" + updated_line;
        }
        else if (!error_text.empty())
            snapshot.tooltip_text += L"\n" + error_text;
    }
    else
    {
        snapshot.tooltip_text = usage_tooltip::Header(L"Codex", windows);
        for (const usage_tooltip::Window& window : windows)
            snapshot.tooltip_text += L"\n" + usage_tooltip::WindowLine(window, now_unix);
        if (!credits_line.empty())
            snapshot.tooltip_text += L"\n" + credits_line;
        if (record->limit_reached)
        {
            snapshot.tooltip_text += L"\nLimit reached";
            if (!record->reached_type.empty())
                snapshot.tooltip_text += L" (" + record->reached_type + L")";
        }
        snapshot.tooltip_text += L"\n\n" + updated_line;
    }

    if (!helper_note.empty())
        snapshot.tooltip_text += L"\n" + helper_note;
    return snapshot;
}

const std::wstring& CCodexUsageData::GetValueText(CodexUsageWindow window) const
{
    thread_local std::wstring value_text;
    std::lock_guard<std::mutex> lock(m_state_mutex);
    value_text = (window == CodexUsageWindow::Rolling5Hours ? m_snapshot.value_5h_text : m_snapshot.value_7d_text);
    return value_text;
}

const CCodexUsageData::Metric& CCodexUsageData::GetMetric(CodexUsageWindow window) const
{
    thread_local Metric metric;
    std::lock_guard<std::mutex> lock(m_state_mutex);
    metric = (window == CodexUsageWindow::Rolling5Hours ? m_snapshot.rolling_5h : m_snapshot.rolling_7d);
    return metric;
}

const CCodexUsageData::Credits& CCodexUsageData::GetCredits() const
{
    thread_local Credits credits;
    std::lock_guard<std::mutex> lock(m_state_mutex);
    credits = m_snapshot.credits;
    return credits;
}

usage_tooltip::ResetCredits CCodexUsageData::GetResetCredits() const
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_snapshot.reset_credits;
}

const std::wstring& CCodexUsageData::GetTooltipText() const
{
    thread_local std::wstring tooltip_text;
    std::lock_guard<std::mutex> lock(m_state_mutex);
    tooltip_text = m_snapshot.tooltip_text;
    return tooltip_text;
}
