#include "pch.h"
#include "HelperSupport.h"

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <vector>

namespace helper_support
{
namespace
{
constexpr wchar_t PLUGIN_CACHE_DIR_NAME[] = L"trafficmonitor-claude-usage-plugin";
constexpr unsigned long long FILETIME_UNIX_EPOCH = 116444736000000000ULL;
// Same tolerance as the helper scripts' watch lock check.
constexpr long long WATCH_LOCK_START_TOLERANCE_SECONDS = 30;

bool FileTimeToUnixSeconds(const FILETIME& file_time, long long& unix_seconds)
{
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < FILETIME_UNIX_EPOCH)
        return false;
    unix_seconds = static_cast<long long>((value.QuadPart - FILETIME_UNIX_EPOCH) / 10000000ULL);
    return true;
}

bool TryGetLockStartedAt(const std::string& lock_json, long long& unix_seconds)
{
    const size_t key_pos = lock_json.find("\"started_at\"");
    if (key_pos == std::string::npos)
        return false;
    const size_t colon_pos = lock_json.find(':', key_pos + 12);
    if (colon_pos == std::string::npos)
        return false;
    const size_t value_start = lock_json.find('"', colon_pos + 1);
    if (value_start == std::string::npos)
        return false;
    const size_t value_end = lock_json.find('"', value_start + 1);
    if (value_end == std::string::npos)
        return false;
    return TryParseIso8601UtcSeconds(lock_json.substr(value_start + 1, value_end - value_start - 1), unix_seconds);
}

std::wstring GetCurrentModulePath()
{
    HMODULE module_handle{};
    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetCurrentModulePath),
        &module_handle))
    {
        return std::wstring();
    }

    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(module_handle, &path[0], static_cast<DWORD>(path.size()));
    while (length >= path.size() - 1)
    {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(module_handle, &path[0], static_cast<DWORD>(path.size()));
    }
    if (length == 0)
        return std::wstring();

    path.resize(length);
    return path;
}

std::wstring GetDirectoryPath(const std::wstring& path)
{
    const size_t separator_pos = path.find_last_of(L"\\/");
    return separator_pos == std::wstring::npos ? std::wstring() : path.substr(0, separator_pos);
}
}

std::wstring GetEnvVar(const wchar_t* name)
{
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length <= 1)
        return std::wstring();

    std::wstring value(length - 1, L'\0');
    GetEnvironmentVariableW(name, &value[0], length);
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

std::wstring GetPluginCacheDir()
{
    const std::wstring local_app_data = TrimString(GetEnvVar(L"LOCALAPPDATA"));
    if (!local_app_data.empty())
        return JoinPath(local_app_data, PLUGIN_CACHE_DIR_NAME);

    const std::wstring home = TrimString(GetEnvVar(L"USERPROFILE"));
    if (home.empty())
        return std::wstring();
    return JoinPath(JoinPath(home, L".cache"), PLUGIN_CACHE_DIR_NAME);
}

std::wstring GetPluginCachePath(const wchar_t* file_name)
{
    const std::wstring dir = GetPluginCacheDir();
    return dir.empty() ? std::wstring() : JoinPath(dir, file_name);
}

bool GetFileWriteTime(const std::wstring& path, unsigned long long& file_time)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return false;
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;

    ULARGE_INTEGER value{};
    value.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    value.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    file_time = value.QuadPart;
    return true;
}

bool ReadFileShared(const std::wstring& path, unsigned long long max_size, std::string& bytes)
{
    bytes.clear();
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && static_cast<unsigned long long>(size.QuadPart) <= max_size;
    if (ok && size.QuadPart > 0)
    {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read_total{};
        while (ok && read_total < bytes.size())
        {
            DWORD read_now{};
            ok = ReadFile(file, &bytes[read_total], static_cast<DWORD>(bytes.size() - read_total), &read_now, nullptr) != FALSE;
            if (read_now == 0)
                break;
            read_total += read_now;
        }
        bytes.resize(read_total);
    }
    CloseHandle(file);

    if (ok && bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }
    return ok;
}

long long GetUnixNowSeconds()
{
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER value{};
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    if (value.QuadPart < FILETIME_UNIX_EPOCH)
        return 0;
    return static_cast<long long>((value.QuadPart - FILETIME_UNIX_EPOCH) / 10000000ULL);
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

std::wstring FormatAgeText(long long age_seconds)
{
    if (age_seconds < 60)
        return L"just now";
    return FormatDurationFromSeconds(static_cast<unsigned long long>(age_seconds)) + L" ago";
}

bool UnixSecondsToLocalText(long long unix_seconds, std::wstring& text)
{
    if (unix_seconds < 0)
        return false;

    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<unsigned long long>(unix_seconds) * 10000000ULL + FILETIME_UNIX_EPOCH;
    FILETIME file_time{};
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;

    SYSTEMTIME utc_time{};
    SYSTEMTIME local_time{};
    if (!FileTimeToSystemTime(&file_time, &utc_time) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc_time, &local_time))
        return false;

    const int date_length = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local_time, nullptr, nullptr, 0, nullptr);
    const int time_length = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local_time, nullptr, nullptr, 0);
    if (date_length <= 1 || time_length <= 1)
        return false;

    std::wstring date_text(static_cast<size_t>(date_length - 1), L'\0');
    std::wstring time_text(static_cast<size_t>(time_length - 1), L'\0');
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local_time, nullptr, &date_text[0], date_length, nullptr) == 0)
        return false;
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local_time, nullptr, &time_text[0], time_length) == 0)
        return false;

    text = date_text + L" " + time_text;
    return true;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return std::wstring();
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], size);
    return wide;
}

bool IsWatchLockProcessRunning(const std::wstring& lock_path)
{
    std::string lock_json;
    if (lock_path.empty() || !ReadFileShared(lock_path, 64 * 1024, lock_json))
        return false;

    const size_t key_pos = lock_json.find("\"pid\"");
    if (key_pos == std::string::npos)
        return false;
    const size_t colon_pos = lock_json.find(':', key_pos + 5);
    if (colon_pos == std::string::npos)
        return false;

    const unsigned long process_id = strtoul(lock_json.c_str() + colon_pos + 1, nullptr, 10);
    if (process_id == 0)
        return false;

    // The watcher writes started_at as it starts; a PID that Windows reused after a restart belongs to a
    // process that started later.
    long long lock_started_at{};
    if (!TryGetLockStartedAt(lock_json, lock_started_at))
        return false;

    HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process_id));
    if (process_handle == nullptr)
        return false;
    DWORD exit_code{};
    FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
    long long process_started_at{};
    const bool alive = GetExitCodeProcess(process_handle, &exit_code) && exit_code == STILL_ACTIVE &&
        GetProcessTimes(process_handle, &creation_time, &exit_time, &kernel_time, &user_time) &&
        FileTimeToUnixSeconds(creation_time, process_started_at);
    CloseHandle(process_handle);
    return alive && std::llabs(process_started_at - lock_started_at) <= WATCH_LOCK_START_TOLERANCE_SECONDS;
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
    return SystemTimeToFileTime(&utc_time, &file_time) && FileTimeToUnixSeconds(file_time, unix_seconds);
}

std::wstring FindBundledScript(const wchar_t* script_file_name)
{
    const std::wstring module_dir = GetDirectoryPath(GetCurrentModulePath());
    if (module_dir.empty())
        return std::wstring();

    const std::wstring candidates[] = {
        JoinPath(module_dir, std::wstring(L"ClaudeUsagePlugin\\") + script_file_name),
        JoinPath(module_dir, script_file_name),
        JoinPath(module_dir, std::wstring(L"..\\..\\..\\scripts\\") + script_file_name),
        JoinPath(module_dir, std::wstring(L"..\\..\\..\\..\\scripts\\") + script_file_name),
    };
    for (const std::wstring& candidate : candidates)
    {
        if (FileExists(candidate))
            return candidate;
    }
    return std::wstring();
}

bool LaunchBundledScript(const std::wstring& script_path, const wchar_t* argument)
{
    if (script_path.empty() || !FileExists(script_path))
        return false;

    std::wstring powershell_path = JoinPath(TrimString(GetEnvVar(L"SystemRoot")), L"System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    if (!FileExists(powershell_path))
        powershell_path = L"powershell.exe";

    const std::wstring script_dir = GetDirectoryPath(script_path);
    if (script_dir.empty() || !DirectoryExists(script_dir))
        return false;

    std::wstring command_line = L"\"" + powershell_path + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + script_path + L"\" " + argument;
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(nullptr, mutable_command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, script_dir.c_str(), &startup_info, &process_info))
        return false;

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}
}
