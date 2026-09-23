#pragma once

#include <string>

// Shared helpers for the Claude and Codex usage data sources: plugin cache paths, shared-mode
// file reads, bundled helper script discovery/launch, and watch lock checks.
namespace helper_support
{
std::wstring GetEnvVar(const wchar_t* name);
std::wstring TrimString(const std::wstring& value);
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);

// %LOCALAPPDATA%\trafficmonitor-claude-usage-plugin (or %USERPROFILE%\.cache\...)
std::wstring GetPluginCacheDir();
std::wstring GetPluginCachePath(const wchar_t* file_name);

// Last write time as a FILETIME value (100 ns ticks). Returns false when the file is missing.
bool GetFileWriteTime(const std::wstring& path, unsigned long long& file_time);

// Reads a whole file opened with read/write/delete sharing so helpers can replace it meanwhile.
bool ReadFileShared(const std::wstring& path, unsigned long long max_size, std::string& bytes);

long long GetUnixNowSeconds();
std::wstring FormatDurationFromSeconds(unsigned long long total_seconds);
std::wstring FormatAgeText(long long age_seconds);
bool UnixSecondsToLocalText(long long unix_seconds, std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);

// True when the watch lock names a live process id.
bool IsWatchLockProcessRunning(const std::wstring& lock_path);

// Finds <plugins>\ClaudeUsagePlugin\<script_file_name> next to this DLL (or the repo scripts dir).
std::wstring FindBundledScript(const wchar_t* script_file_name);

// Runs "powershell.exe -File <script_path> <argument>" hidden, without waiting.
bool LaunchBundledScript(const std::wstring& script_path, const wchar_t* argument);
}
