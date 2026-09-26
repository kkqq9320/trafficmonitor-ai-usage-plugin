#include "pch.h"
#include "ClaudeUsageData.h"
#include "HelperSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <string>

namespace
{
constexpr unsigned long long REFRESH_INTERVAL_MS = 30ULL * 1000ULL;
constexpr unsigned long long RETRY_INTERVAL_MS = 30ULL * 1000ULL;
constexpr unsigned long long BACKOFF_RECHECK_INTERVAL_MS = 5ULL * 1000ULL;
// Between full reloads, a new helper snapshot is noticed by its write time.
constexpr unsigned long long SNAPSHOT_CHANGE_CHECK_INTERVAL_MS = 5ULL * 1000ULL;
// The helper refreshes every 5 minutes by default (configurable), so a snapshot stays usable for
// 30 minutes and is marked stale once it is older than two refresh intervals.
constexpr unsigned long long HELPER_CACHE_MAX_AGE_MS = 30ULL * 60ULL * 1000ULL;
constexpr unsigned long long DEFAULT_HELPER_REFRESH_MS = 5ULL * 60ULL * 1000ULL;
constexpr unsigned long long MAX_JSON_FILE_SIZE = 1024ULL * 1024ULL;
constexpr wchar_t PLUGIN_CACHE_DIR_NAME[] = L"trafficmonitor-claude-usage-plugin";
constexpr wchar_t HELPER_CACHE_FILE_NAME[] = L"claude-web-usage.json";
constexpr wchar_t HELPER_STATUS_FILE_NAME[] = L"claude-web-helper-status.json";
constexpr wchar_t HELPER_WATCH_LOCK_FILE_NAME[] = L"claude-web-helper-watch.lock";

std::wstring GetEnvVar(const wchar_t* name)
{
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0)
        return std::wstring();

    std::wstring value(length - 1, L'\0');
    GetEnvironmentVariableW(name, value.empty() ? nullptr : &value[0], length);
    return value;
}

std::wstring TrimString(const std::wstring& value)
{
    size_t start{};
    while (start < value.size() && iswspace(value[start]))
        ++start;

    size_t end = value.size();
    while (end > start && iswspace(value[end - 1]))
        --end;

    return value.substr(start, end - start);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty())
        return right;
    if (left.back() == L'\\' || left.back() == L'/')
        return left + right;
    return left + L'\\' + right;
}

std::wstring GetCacheDirByName(const wchar_t* dir_name)
{
    const std::wstring local_app_data = TrimString(GetEnvVar(L"LOCALAPPDATA"));
    if (!local_app_data.empty())
        return JoinPath(local_app_data, dir_name);

    const std::wstring home = TrimString(GetEnvVar(L"USERPROFILE"));
    if (home.empty())
        return std::wstring();

    return JoinPath(JoinPath(home, L".cache"), dir_name);
}

std::wstring GetPluginCacheDir()
{
    return GetCacheDirByName(PLUGIN_CACHE_DIR_NAME);
}

std::wstring BuildCachePath(const std::wstring& cache_dir, const wchar_t* file_name)
{
    if (cache_dir.empty())
        return std::wstring();
    return JoinPath(cache_dir, file_name);
}

std::wstring GetHelperCachePath()
{
    return BuildCachePath(GetPluginCacheDir(), HELPER_CACHE_FILE_NAME);
}

std::wstring GetHelperStatusPath()
{
    return BuildCachePath(GetPluginCacheDir(), HELPER_STATUS_FILE_NAME);
}

std::wstring GetHelperWatchLockPath()
{
    return BuildCachePath(GetPluginCacheDir(), HELPER_WATCH_LOCK_FILE_NAME);
}

std::wstring GetDirectoryPath(const std::wstring& path)
{
    const size_t separator_pos = path.find_last_of(L"\\/");
    if (separator_pos == std::wstring::npos)
        return std::wstring();

    return path.substr(0, separator_pos);
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectoryExists(const std::wstring& path)
{
    if (path.empty())
        return false;

    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    size_t start = 0;
    if (path.size() >= 2 && path[1] == L':')
        start = 3;
    else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        const size_t first_sep = path.find(L'\\', 2);
        if (first_sep == std::wstring::npos)
            return false;
        const size_t second_sep = path.find(L'\\', first_sep + 1);
        if (second_sep == std::wstring::npos)
            return false;
        start = second_sep + 1;
    }

    for (size_t index = start; index <= path.size(); ++index)
    {
        if (index != path.size() && path[index] != L'\\' && path[index] != L'/')
            continue;

        const std::wstring segment = path.substr(0, index);
        if (segment.empty())
            continue;

        attributes = GetFileAttributesW(segment.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                return false;
            continue;
        }

        if (!CreateDirectoryW(segment.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }

    return true;
}

bool GetCurrentTimeMs(unsigned long long& current_time_ms)
{
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);

    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    current_time_ms = value.QuadPart / 10000ULL;
    return true;
}

bool GetFileLastWriteTimeMs(const std::wstring& path, unsigned long long& last_write_time_ms)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return false;

    ULARGE_INTEGER value{};
    value.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    value.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    last_write_time_ms = value.QuadPart / 10000ULL;
    return true;
}

bool ReadUtf8File(const std::wstring& path, std::wstring& output)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return false;

    const unsigned long long file_size =
        (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) |
        static_cast<unsigned long long>(attributes.nFileSizeLow);
    if (file_size > MAX_JSON_FILE_SIZE)
        return false;

    FILE* file{};
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
        return false;

    std::string bytes;
    bytes.reserve(static_cast<size_t>(file_size));

    char buffer[4096];
    while (true)
    {
        const size_t read_size = fread(buffer, 1, sizeof(buffer), file);
        if (read_size == 0)
            break;
        bytes.append(buffer, read_size);
    }
    fclose(file);

    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }

    if (bytes.empty())
    {
        output.clear();
        return true;
    }

    const int wide_size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_size <= 0)
        return false;

    output.assign(wide_size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), output.empty() ? nullptr : &output[0], wide_size);
    return true;
}

bool WriteUtf8File(const std::wstring& path, const std::wstring& content)
{
    const size_t separator_pos = path.find_last_of(L"\\/");
    if (separator_pos != std::wstring::npos)
    {
        if (!EnsureDirectoryExists(path.substr(0, separator_pos)))
            return false;
    }

    const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
    if (utf8_size < 0)
        return false;

    std::string utf8(static_cast<size_t>(utf8_size), '\0');
    if (utf8_size > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, content.c_str(), static_cast<int>(content.size()), &utf8[0], utf8_size, nullptr, nullptr);
    }

    FILE* file{};
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
        return false;

    bool success = true;
    if (!utf8.empty())
        success = fwrite(utf8.data(), 1, utf8.size(), file) == utf8.size();

    fclose(file);
    return success;
}

bool FindJsonKey(const std::wstring& json, const wchar_t* key, size_t& value_pos)
{
    std::wstring token = L"\"";
    token += key;
    token += L"\"";

    const size_t key_pos = json.find(token);
    if (key_pos == std::wstring::npos)
        return false;

    const size_t colon_pos = json.find(L':', key_pos + token.size());
    if (colon_pos == std::wstring::npos)
        return false;

    value_pos = colon_pos + 1;
    return true;
}

size_t FindMatchingBracket(const std::wstring& text, size_t open_pos, wchar_t open_char, wchar_t close_char)
{
    bool in_string = false;
    bool escape = false;
    int depth = 0;

    for (size_t index = open_pos; index < text.size(); ++index)
    {
        const wchar_t ch = text[index];
        if (in_string)
        {
            if (escape)
                escape = false;
            else if (ch == L'\\')
                escape = true;
            else if (ch == L'"')
                in_string = false;
            continue;
        }

        if (ch == L'"')
        {
            in_string = true;
            continue;
        }

        if (ch == open_char)
            ++depth;
        else if (ch == close_char)
        {
            --depth;
            if (depth == 0)
                return index;
        }
    }

    return std::wstring::npos;
}

bool TryGetJsonString(const std::wstring& json, const wchar_t* key, std::wstring& value)
{
    size_t value_pos{};
    if (!FindJsonKey(json, key, value_pos))
        return false;

    while (value_pos < json.size() && iswspace(json[value_pos]))
        ++value_pos;

    if (value_pos >= json.size() || json[value_pos] != L'"')
        return false;

    const size_t end_pos = json.find(L'"', value_pos + 1);
    if (end_pos == std::wstring::npos)
        return false;

    value = json.substr(value_pos + 1, end_pos - value_pos - 1);
    return true;
}

bool TryGetJsonDouble(const std::wstring& json, const wchar_t* key, double& value)
{
    size_t value_pos{};
    if (!FindJsonKey(json, key, value_pos))
        return false;

    while (value_pos < json.size() && iswspace(json[value_pos]))
        ++value_pos;

    if (value_pos >= json.size())
        return false;

    wchar_t* end_ptr{};
    value = wcstod(json.c_str() + value_pos, &end_ptr);
    return end_ptr != json.c_str() + value_pos;
}

bool TryGetJsonObject(const std::wstring& json, const wchar_t* key, std::wstring& value)
{
    size_t value_pos{};
    if (!FindJsonKey(json, key, value_pos))
        return false;

    while (value_pos < json.size() && iswspace(json[value_pos]))
        ++value_pos;

    if (value_pos >= json.size() || json[value_pos] != L'{')
        return false;

    const size_t end_pos = FindMatchingBracket(json, value_pos, L'{', L'}');
    if (end_pos == std::wstring::npos)
        return false;

    value = json.substr(value_pos, end_pos - value_pos + 1);
    return true;
}

bool TryParseFixedInt(const std::wstring& text, size_t start, size_t length, int& value)
{
    if (start + length > text.size() || length == 0)
        return false;

    int parsed_value{};
    for (size_t index = start; index < start + length; ++index)
    {
        const wchar_t ch = text[index];
        if (!iswdigit(ch))
            return false;
        parsed_value = parsed_value * 10 + (ch - L'0');
    }

    value = parsed_value;
    return true;
}

bool TryParseFractionalMilliseconds(const std::wstring& text, size_t& position, int& millisecond)
{
    millisecond = 0;
    if (position >= text.size() || text[position] != L'.')
        return true;

    ++position;
    const size_t fraction_start = position;
    while (position < text.size() && iswdigit(text[position]))
        ++position;

    const size_t digit_count = position - fraction_start;
    if (digit_count == 0)
        return false;

    const size_t significant_digits = (digit_count > 3 ? 3 : digit_count);
    int parsed_value{};
    for (size_t index = 0; index < significant_digits; ++index)
        parsed_value = parsed_value * 10 + (text[fraction_start + index] - L'0');

    for (size_t index = significant_digits; index < 3; ++index)
        parsed_value *= 10;

    millisecond = parsed_value;
    return true;
}

bool TryParseTimezoneOffsetMinutes(const std::wstring& text, size_t& position, int& offset_minutes)
{
    offset_minutes = 0;
    if (position >= text.size())
        return true;

    if (text[position] == L'Z' || text[position] == L'z')
    {
        ++position;
        return true;
    }

    if (text[position] != L'+' && text[position] != L'-')
        return false;

    const wchar_t sign = text[position];
    ++position;

    int offset_hour{};
    int offset_minute{};
    if (!TryParseFixedInt(text, position, 2, offset_hour))
        return false;
    position += 2;

    if (position >= text.size() || text[position] != L':')
        return false;
    ++position;

    if (!TryParseFixedInt(text, position, 2, offset_minute))
        return false;
    position += 2;

    offset_minutes = offset_hour * 60 + offset_minute;
    if (sign == L'-')
        offset_minutes = -offset_minutes;
    return true;
}

bool TryParseUtcIso8601(const std::wstring& text, FILETIME& file_time)
{
    size_t position{};
    int year{}, month{}, day{}, hour{}, minute{}, second{}, millisecond{};

    if (!TryParseFixedInt(text, position, 4, year))
        return false;
    position += 4;
    if (position >= text.size() || text[position] != L'-')
        return false;
    ++position;

    if (!TryParseFixedInt(text, position, 2, month))
        return false;
    position += 2;
    if (position >= text.size() || text[position] != L'-')
        return false;
    ++position;

    if (!TryParseFixedInt(text, position, 2, day))
        return false;
    position += 2;
    if (position >= text.size() || (text[position] != L'T' && text[position] != L't'))
        return false;
    ++position;

    if (!TryParseFixedInt(text, position, 2, hour))
        return false;
    position += 2;
    if (position >= text.size() || text[position] != L':')
        return false;
    ++position;

    if (!TryParseFixedInt(text, position, 2, minute))
        return false;
    position += 2;
    if (position >= text.size() || text[position] != L':')
        return false;
    ++position;

    if (!TryParseFixedInt(text, position, 2, second))
        return false;
    position += 2;

    if (!TryParseFractionalMilliseconds(text, position, millisecond))
        return false;

    int offset_minutes{};
    if (!TryParseTimezoneOffsetMinutes(text, position, offset_minutes))
        return false;

    if (position != text.size())
        return false;

    SYSTEMTIME system_time{};
    system_time.wYear = static_cast<WORD>(year);
    system_time.wMonth = static_cast<WORD>(month);
    system_time.wDay = static_cast<WORD>(day);
    system_time.wHour = static_cast<WORD>(hour);
    system_time.wMinute = static_cast<WORD>(minute);
    system_time.wSecond = static_cast<WORD>(second);
    system_time.wMilliseconds = static_cast<WORD>(millisecond);

    if (!SystemTimeToFileTime(&system_time, &file_time))
        return false;

    if (offset_minutes != 0)
    {
        ULARGE_INTEGER raw_time{};
        raw_time.LowPart = file_time.dwLowDateTime;
        raw_time.HighPart = file_time.dwHighDateTime;

        const unsigned long long absolute_offset_minutes =
            static_cast<unsigned long long>(offset_minutes < 0 ? -offset_minutes : offset_minutes);
        const unsigned long long offset_ticks = absolute_offset_minutes * 60ULL * 10000000ULL;

        if (offset_minutes > 0)
            raw_time.QuadPart -= offset_ticks;
        else
            raw_time.QuadPart += offset_ticks;

        file_time.dwLowDateTime = raw_time.LowPart;
        file_time.dwHighDateTime = raw_time.HighPart;
    }

    return true;
}

bool FileTimeToUnixSeconds(const FILETIME& file_time, long long& unix_seconds)
{
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < 116444736000000000ULL)
        return false;

    unix_seconds = static_cast<long long>((value.QuadPart - 116444736000000000ULL) / 10000000ULL);
    return true;
}

std::wstring FormatDurationFromSeconds(unsigned long long total_seconds)
{
    if (total_seconds < 60ULL)
        return L"<1m";

    const unsigned long long total_minutes = total_seconds / 60ULL;
    const unsigned long long days = total_minutes / (24ULL * 60ULL);
    const unsigned long long hours = (total_minutes / 60ULL) % 24ULL;
    const unsigned long long minutes = total_minutes % 60ULL;

    std::wstring text;
    if (days > 0)
    {
        text = std::to_wstring(days) + L"d";
        if (hours > 0)
            text += L" " + std::to_wstring(hours) + L"h";
        return text;
    }

    if (hours > 0)
    {
        text = std::to_wstring(hours) + L"h";
        if (minutes > 0)
            text += L" " + std::to_wstring(minutes) + L"m";
        return text;
    }

    return std::to_wstring(minutes) + L"m";
}

std::wstring FormatResetRemaining(long long reset_at_unix_seconds)
{
    FILETIME now_file_time{};
    GetSystemTimeAsFileTime(&now_file_time);

    long long now_unix_seconds{};
    if (!FileTimeToUnixSeconds(now_file_time, now_unix_seconds))
        return std::wstring();

    if (reset_at_unix_seconds <= now_unix_seconds)
        return L"now";

    return L"in " + FormatDurationFromSeconds(static_cast<unsigned long long>(reset_at_unix_seconds - now_unix_seconds));
}

bool FileTimeToLocalText(const FILETIME& utc_file_time, std::wstring& text)
{
    SYSTEMTIME utc_time{};
    if (!FileTimeToSystemTime(&utc_file_time, &utc_time))
        return false;

    SYSTEMTIME local_time{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc_time, &local_time))
        return false;

    const int date_length = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local_time, nullptr, nullptr, 0, nullptr);
    if (date_length <= 1)
        return false;

    const int time_length = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local_time, nullptr, nullptr, 0);
    if (time_length <= 1)
        return false;

    std::wstring date_text(static_cast<size_t>(date_length - 1), L'\0');
    std::wstring time_text(static_cast<size_t>(time_length - 1), L'\0');
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local_time, nullptr, &date_text[0], date_length, nullptr) == 0)
        return false;
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local_time, nullptr, &time_text[0], time_length) == 0)
        return false;

    text = date_text;
    text += L" ";
    text += time_text;
    return true;
}

std::wstring FormatResetTime(const std::wstring& raw_value)
{
    FILETIME utc_file_time{};
    if (!TryParseUtcIso8601(raw_value, utc_file_time))
        return raw_value;

    std::wstring text;
    if (!FileTimeToLocalText(utc_file_time, text))
        return raw_value;
    return text;
}

std::wstring FormatPercentage(double value)
{
    const double clamped = (value < 0.0 ? 0.0 : (value > 100.0 ? 100.0 : value));
    const double rounded_whole = std::round(clamped);
    wchar_t buffer[32];
    if (std::fabs(clamped - rounded_whole) < 0.05)
        swprintf_s(buffer, L"%.0f%%", rounded_whole);
    else
        swprintf_s(buffer, L"%.1f%%", clamped);
    return buffer;
}

std::wstring BuildMetricTooltip(const wchar_t* label, const CClaudeUsageData::Metric& metric)
{
    std::wstring text(label);
    text += L": ";
    if (!metric.available)
    {
        text += L"unavailable";
        return text;
    }

    text += FormatPercentage(metric.percentage);
    if (metric.has_reset_time && metric.reset_at_unix_seconds <= helper_support::GetUnixNowSeconds())
    {
        text += L" (reset time passed at ";
        text += metric.reset_time_text;
        text += L"; waiting for new data)";
        return text;
    }
    const std::wstring reset_remaining = (metric.has_reset_time ? FormatResetRemaining(metric.reset_at_unix_seconds) : std::wstring());
    if (!reset_remaining.empty() && !metric.reset_time_text.empty())
    {
        text += L" (resets ";
        text += reset_remaining;
        text += L" at ";
        text += metric.reset_time_text;
        text += L")";
    }
    else if (!reset_remaining.empty())
    {
        text += L" (resets ";
        text += reset_remaining;
        text += L")";
    }
    else if (!metric.reset_time_text.empty())
    {
        text += L" (resets at ";
        text += metric.reset_time_text;
        text += L")";
    }
    return text;
}

void ApplyResetAtValue(const std::wstring& reset_at, CClaudeUsageData::Metric& metric)
{
    if (reset_at.empty())
        return;

    FILETIME reset_file_time{};
    if (TryParseUtcIso8601(reset_at, reset_file_time))
    {
        metric.has_reset_time = FileTimeToUnixSeconds(reset_file_time, metric.reset_at_unix_seconds);
        if (!FileTimeToLocalText(reset_file_time, metric.reset_time_text))
            metric.reset_time_text = reset_at;
    }
    else
    {
        metric.reset_time_text = reset_at;
    }
}

bool LoadMetricFromApiSection(const std::wstring& response_json, const wchar_t* section_name, CClaudeUsageData::Metric& metric)
{
    std::wstring section_json;
    if (!TryGetJsonObject(response_json, section_name, section_json))
        return false;

    double utilization{};
    if (!TryGetJsonDouble(section_json, L"utilization", utilization))
        return false;

    metric.available = true;
    metric.percentage = utilization;

    std::wstring resets_at;
    if (TryGetJsonString(section_json, L"resets_at", resets_at))
        ApplyResetAtValue(resets_at, metric);

    return true;
}

void LoadResetCredits(const std::wstring& json, CClaudeUsageData::Snapshot& snapshot)
{
    std::wstring credits_json;
    double count{};
    if (!TryGetJsonObject(json, L"reset_credits", credits_json) ||
        !TryGetJsonDouble(credits_json, L"available_count", count) || !std::isfinite(count) || count < 0)
        return;
    snapshot.has_reset_credits = true;
    snapshot.reset_credits = static_cast<long long>(count);

    double expires_at{};
    if (TryGetJsonDouble(credits_json, L"earliest_expires_at", expires_at) && std::isfinite(expires_at) && expires_at > 0)
    {
        snapshot.has_reset_credits_expiry = true;
        snapshot.reset_credits_expires_at = static_cast<long long>(expires_at);
    }
}

bool LoadSnapshotFromCachedJson(const std::wstring& json, CClaudeUsageData::Snapshot& snapshot)
{
    const bool has_api_5h = LoadMetricFromApiSection(json, L"five_hour", snapshot.rolling_5h);
    const bool has_api_7d = LoadMetricFromApiSection(json, L"seven_day", snapshot.rolling_7d);
    LoadResetCredits(json, snapshot);
    return has_api_5h || has_api_7d;
}

bool TryLoadHelperUsageSnapshot(CClaudeUsageData::Snapshot& snapshot, bool require_fresh_cache)
{
    const std::wstring helper_cache_path = GetHelperCachePath();
    if (helper_cache_path.empty() || !FileExists(helper_cache_path))
        return false;

    unsigned long long last_write_time_ms{};
    if (!GetFileLastWriteTimeMs(helper_cache_path, last_write_time_ms))
        return false;

    if (require_fresh_cache)
    {
        unsigned long long now_ms{};
        if (!GetCurrentTimeMs(now_ms))
            return false;
        if (now_ms >= last_write_time_ms && now_ms - last_write_time_ms > HELPER_CACHE_MAX_AGE_MS)
        {
            snapshot.helper_write_time_ms = last_write_time_ms;  // seen; only a newer write needs a reload
            return false;
        }
    }

    // A read failure leaves the write time unrecorded, so the next change check reads the file again.
    std::wstring cached_json;
    if (!ReadUtf8File(helper_cache_path, cached_json))
        return false;
    snapshot.helper_write_time_ms = last_write_time_ms;

    CClaudeUsageData::Snapshot cached_snapshot;
    if (!LoadSnapshotFromCachedJson(cached_json, cached_snapshot))
        return false;

    snapshot.rolling_5h = cached_snapshot.rolling_5h;
    snapshot.rolling_7d = cached_snapshot.rolling_7d;
    snapshot.has_reset_credits = cached_snapshot.has_reset_credits;
    snapshot.reset_credits = cached_snapshot.reset_credits;
    snapshot.has_reset_credits_expiry = cached_snapshot.has_reset_credits_expiry;
    snapshot.reset_credits_expires_at = cached_snapshot.reset_credits_expires_at;
    snapshot.source_text = L"Claude web helper";
    snapshot.has_data_time = true;
    snapshot.data_at_unix = static_cast<long long>(last_write_time_ms / 1000ULL) - 11644473600LL;
    double refresh_ms{};
    snapshot.refresh_ms = (TryGetJsonDouble(cached_json, L"refresh_ms", refresh_ms) && refresh_ms > 0)
        ? static_cast<unsigned long long>(refresh_ms)
        : DEFAULT_HELPER_REFRESH_MS;
    return true;
}

unsigned long long GetRefreshIntervalMs(bool last_refresh_succeeded)
{
    return (last_refresh_succeeded ? REFRESH_INTERVAL_MS : RETRY_INTERVAL_MS);
}

bool HelperSnapshotWriteTimeChanged(unsigned long long loaded_write_time_ms)
{
    unsigned long long write_time_ms{};
    return GetFileLastWriteTimeMs(GetHelperCachePath(), write_time_ms) && write_time_ms != loaded_write_time_ms;
}

std::wstring BuildHelperStatusSummary(const std::wstring& state, const std::wstring& error_text)
{
    if (state == L"login_browser_opened")
        return L"Claude web helper login window is open";
    if (state == L"login_required")
        return L"Claude web helper needs login";
    if (state == L"access_denied")
        return L"Claude web helper signed in, but usage access was denied";
    if (state == L"rate_limited")
        return L"Claude web helper hit a rate limit";
    if (state == L"node_missing")
        return L"Claude web helper: Node.js 22+ not found (see helper-config.json)";
    if (state == L"profile_in_use")
        return L"Claude web helper browser profile is still in use";
    if (state == L"cloudflare_blocked")
        return L"Claude web helper is blocked by Cloudflare";
    if (state == L"request_failed")
    {
        std::wstring text = L"Claude web helper could not refresh usage";
        if (!error_text.empty() && error_text.rfind(L"HTTP ", 0) == 0)
        {
            text += L" (";
            text += error_text;
            text += L")";
        }
        return text;
    }
    if (state == L"crashed")
        return L"Claude web helper crashed";
    if (state == L"ok")
        return std::wstring();
    if (state.empty())
        return std::wstring();

    return L"Claude web helper status: " + state;
}

bool TryLoadHelperStatusSummary(std::wstring& summary)
{
    summary.clear();

    const std::wstring status_path = GetHelperStatusPath();
    if (status_path.empty() || !FileExists(status_path))
        return false;

    std::wstring status_json;
    if (!ReadUtf8File(status_path, status_json))
        return false;

    std::wstring state;
    if (!TryGetJsonString(status_json, L"state", state))
        state.clear();

    std::wstring error_text;
    TryGetJsonString(status_json, L"error", error_text);

    if (state == L"ok")
    {
        const std::wstring helper_cache_path = GetHelperCachePath();
        unsigned long long helper_last_write_time_ms{};
        unsigned long long now_ms{};
        if (GetFileLastWriteTimeMs(helper_cache_path, helper_last_write_time_ms) &&
            GetCurrentTimeMs(now_ms) &&
            now_ms >= helper_last_write_time_ms &&
            now_ms - helper_last_write_time_ms > HELPER_CACHE_MAX_AGE_MS)
        {
            summary = L"Claude web helper snapshot is stale; make sure watch is running";
            return true;
        }

        if (!FileExists(helper_cache_path))
        {
            summary = L"Claude web helper has not produced a snapshot yet";
            return true;
        }

        return false;
    }

    summary = BuildHelperStatusSummary(state, error_text);
    std::wstring retry_after_at;
    if (state == L"rate_limited" && TryGetJsonString(status_json, L"retry_after_at", retry_after_at))
        summary += L" (retry after " + FormatResetTime(retry_after_at) + L")";
    return !summary.empty();
}

}

CClaudeUsageData& CClaudeUsageData::Instance()
{
    static CClaudeUsageData instance;
    return instance;
}

void CClaudeUsageData::AutoStartBundledHelperIfNeeded()
{
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (m_helper_auto_start_attempted)
            return;
        m_helper_auto_start_attempted = true;
    }

    if (helper_support::IsWatchLockProcessRunning(GetHelperWatchLockPath()))
        return;

    const std::wstring script_path = helper_support::FindBundledScript(L"claude-web-helper.ps1");
    if (!script_path.empty())
        helper_support::LaunchBundledScript(script_path, L"start");
}

void CClaudeUsageData::RefreshIfNeeded()
{
    const unsigned long long started_at = GetTickCount64();
    bool allow_api_request = true;
    bool backoff_expired = false;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (m_refresh_in_progress)
            return;

        if (m_next_refresh_tick != 0 && started_at < m_next_refresh_tick)
        {
            allow_api_request = false;
            if (m_last_refresh_tick != 0 && started_at - m_last_refresh_tick < BACKOFF_RECHECK_INTERVAL_MS)
                return;
        }
        else
        {
            backoff_expired = (m_next_refresh_tick != 0 && started_at >= m_next_refresh_tick);
            if (!backoff_expired)
            {
                const unsigned long long refresh_interval_ms = GetRefreshIntervalMs(m_last_refresh_succeeded);
                if (m_last_refresh_tick != 0 && started_at - m_last_refresh_tick < refresh_interval_ms)
                {
                    if (started_at - m_last_change_check_tick < SNAPSHOT_CHANGE_CHECK_INTERVAL_MS)
                        return;
                    m_last_change_check_tick = started_at;
                    if (!HelperSnapshotWriteTimeChanged(m_snapshot.helper_write_time_ms))
                        return;
                }
            }
        }

        m_refresh_in_progress = true;
    }

    unsigned long long retry_after_ms{};
    const bool succeeded = Refresh(retry_after_ms, allow_api_request);
    const unsigned long long completed_at = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_last_refresh_tick = completed_at;
        if (allow_api_request)
            m_last_refresh_succeeded = succeeded;
        if (allow_api_request)
            m_next_refresh_tick = (retry_after_ms == 0 ? 0 : completed_at + retry_after_ms);
        m_refresh_in_progress = false;
    }
}

const std::wstring& CClaudeUsageData::GetValueText(ClaudeUsageWindow window) const
{
    thread_local std::wstring value_text;

    std::lock_guard<std::mutex> lock(m_state_mutex);
    value_text = (window == ClaudeUsageWindow::Rolling5Hours ? m_snapshot.value_5h_text : m_snapshot.value_7d_text);
    return value_text;
}

const CClaudeUsageData::Metric& CClaudeUsageData::GetMetric(ClaudeUsageWindow window) const
{
    thread_local Metric metric;

    std::lock_guard<std::mutex> lock(m_state_mutex);
    metric = (window == ClaudeUsageWindow::Rolling5Hours ? m_snapshot.rolling_5h : m_snapshot.rolling_7d);
    return metric;
}

const std::wstring& CClaudeUsageData::GetTooltipText() const
{
    thread_local std::wstring tooltip_text;

    std::lock_guard<std::mutex> lock(m_state_mutex);
    tooltip_text = m_snapshot.tooltip_text;
    return tooltip_text;
}

bool CClaudeUsageData::Refresh(unsigned long long& retry_after_ms, bool allow_api_request)
{
    Snapshot snapshot;
    const bool succeeded = LoadFromUsageApi(snapshot, retry_after_ms, allow_api_request);
    FinalizeSnapshot(snapshot);

    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_snapshot = snapshot;
    return succeeded;
}

bool CClaudeUsageData::LoadFromUsageApi(Snapshot& snapshot, unsigned long long& retry_after_ms, bool allow_api_request)
{
    retry_after_ms = 0;
    (void)allow_api_request;

    if (TryLoadHelperUsageSnapshot(snapshot, true))
    {
        // Keep showing the snapshot but surface helper problems such as rate limits.
        std::wstring summary;
        if (TryLoadHelperStatusSummary(summary))
            snapshot.error_text = summary;
        return true;
    }

    if (!TryLoadHelperStatusSummary(snapshot.error_text))
        snapshot.error_text = L"Claude web helper snapshot unavailable";
    return false;
}

void CClaudeUsageData::FinalizeSnapshot(Snapshot& snapshot)
{
    const long long now_unix = helper_support::GetUnixNowSeconds();
    const long long stale_after_seconds = (std::max)(180LL, static_cast<long long>(snapshot.refresh_ms / 1000ULL) * 2LL + 60LL);
    const bool data_stale = snapshot.has_data_time && now_unix - snapshot.data_at_unix > stale_after_seconds;
    CClaudeUsageData::Metric* metrics[] = { &snapshot.rolling_5h, &snapshot.rolling_7d };
    for (CClaudeUsageData::Metric* metric : metrics)
    {
        metric->stale = metric->available &&
            (data_stale || (metric->has_reset_time && metric->reset_at_unix_seconds <= now_unix));
    }

    snapshot.value_5h_text = (snapshot.rolling_5h.available ? FormatPercentage(snapshot.rolling_5h.percentage) : L"--");
    snapshot.value_7d_text = (snapshot.rolling_7d.available ? FormatPercentage(snapshot.rolling_7d.percentage) : L"--");

    if (!HasAvailableMetric(snapshot))
    {
        snapshot.tooltip_text = L"Claude usage limits unavailable";
        if (!snapshot.error_text.empty())
        {
            snapshot.tooltip_text += L"\n";
            snapshot.tooltip_text += snapshot.error_text;
        }
        return;
    }

    snapshot.tooltip_text = L"Claude usage limits";
    snapshot.tooltip_text += L"\n";
    snapshot.tooltip_text += BuildMetricTooltip(L"5h", snapshot.rolling_5h);
    snapshot.tooltip_text += L"\n";
    snapshot.tooltip_text += BuildMetricTooltip(L"7d", snapshot.rolling_7d);
    if (snapshot.has_reset_credits)
    {
        snapshot.tooltip_text += L"\nReset credits: " + std::to_wstring(snapshot.reset_credits);
        std::wstring expiry_text;
        if (snapshot.has_reset_credits_expiry && helper_support::UnixSecondsToLocalText(snapshot.reset_credits_expires_at, expiry_text))
            snapshot.tooltip_text += L" (expires " + expiry_text + L")";
    }
    if (snapshot.has_data_time)
    {
        snapshot.tooltip_text += L"\nUpdated " + helper_support::FormatAgeText(now_unix - snapshot.data_at_unix) + L", " + snapshot.source_text;
        if (data_stale)
            snapshot.tooltip_text += L" (stale)";
    }
    if (!snapshot.error_text.empty())
    {
        snapshot.tooltip_text += L"\n";
        snapshot.tooltip_text += snapshot.error_text;
    }
}

bool CClaudeUsageData::HasAvailableMetric(const Snapshot& snapshot)
{
    return snapshot.rolling_5h.available || snapshot.rolling_7d.available;
}
