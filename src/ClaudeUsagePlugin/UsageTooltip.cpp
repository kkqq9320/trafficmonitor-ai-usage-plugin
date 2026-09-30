#include "pch.h"
#include "UsageTooltip.h"

#include <cmath>

namespace usage_tooltip
{
const wchar_t CHART[] = { 0xD83D, 0xDCCA, 0 };   // U+1F4CA
const wchar_t TICKET[] = { 0xD83C, 0xDF9F, 0 };  // U+1F39F

namespace
{
const wchar_t SEPARATOR[] = { L' ', 0x00B7, L' ', 0 };  // " · "
constexpr unsigned long long FILETIME_UNIX_EPOCH = 116444736000000000ULL;

// "6d 0h 18m", "1h 58m", "58m", "<1m"
std::wstring RemainingText(long long seconds)
{
    if (seconds < 60)
        return L"<1m";
    const long long total_minutes = seconds / 60;
    const long long days = total_minutes / (24 * 60);
    const long long hours = (total_minutes / 60) % 24;
    const long long minutes = total_minutes % 60;
    std::wstring text;
    if (days > 0)
        text = std::to_wstring(days) + L"d ";
    if (days > 0 || hours > 0)
        text += std::to_wstring(hours) + L"h ";
    return text + std::to_wstring(minutes) + L"m";
}
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

std::wstring Header(const wchar_t* service, const std::vector<Window>& windows)
{
    std::wstring text = std::wstring(CHART) + L" " + service + L" left ";
    for (size_t index = 0; index < windows.size(); ++index)
    {
        if (index > 0)
            text += SEPARATOR;
        text += windows[index].header_label + L" " + FormatPercentage(100.0 - windows[index].used_percent);
    }
    return text;
}

std::wstring WindowLine(const Window& window, long long now_unix)
{
    std::wstring text = window.label + L": " + FormatPercentage(window.used_percent);
    std::wstring reset_text;
    if (!window.has_reset_time || !ResetTimeText(window.reset_at_unix, reset_text))
        return text;
    if (window.reset_at_unix <= now_unix)
        return text + L" (reset time passed) at " + reset_text;
    return text + L" (" + RemainingText(window.reset_at_unix - now_unix) + L") at " + reset_text;
}

bool ResetTimeText(long long unix_seconds, std::wstring& text)
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

    wchar_t weekday[64]{};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local_time, L"dddd", weekday, 64, nullptr) == 0)
        return false;

    wchar_t buffer[128];
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u %s",
        local_time.wYear, local_time.wMonth, local_time.wDay, local_time.wHour, local_time.wMinute, weekday);
    text = buffer;
    return true;
}

std::wstring ResetCreditsLine(const std::wstring& name, size_t name_width, const ResetCredits& credits)
{
    std::wstring text = std::to_wstring(credits.count) + SEPARATOR + name;
    std::wstring expiry_text;
    if (!credits.has_expiry || !ResetTimeText(credits.expires_at, expiry_text))
        return text;
    if (name.size() < name_width)
        text.append(name_width - name.size(), L' ');
    return text + L" (Expires : " + expiry_text + L")";
}
}
