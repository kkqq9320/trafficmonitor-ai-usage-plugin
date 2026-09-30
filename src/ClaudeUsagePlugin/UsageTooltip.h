#pragma once

#include <string>
#include <vector>

// Tooltip text shared by the Claude and Codex sections and the reset credits section:
//   <chart> Claude left 5h 69% · 7d 55%
//   5h: 31% (1h 58m) at 2026-10-01 13:58 <weekday>
//   ...
//   <ticket> Reset Credits
//   1 · Claude (Expires : 2026-10-23 01:00 <weekday>)
namespace usage_tooltip
{
extern const wchar_t CHART[];
extern const wchar_t TICKET[];

std::wstring FormatPercentage(double value);

struct Window
{
    std::wstring label;         // line label: "5h", "7d", "Fable 7d"
    std::wstring header_label;  // header label: "5h", "7d", "Fable"
    double used_percent{};
    bool has_reset_time{};
    long long reset_at_unix{};
};

// "<chart> Claude left 5h 69% · 7d 55%"
std::wstring Header(const wchar_t* service, const std::vector<Window>& windows);

// "7d: 86% (3d 3h 12m) at 2026-10-04 03:19 <weekday>"; "(reset time passed)" once the reset time is past.
std::wstring WindowLine(const Window& window, long long now_unix);

// Local "yyyy-MM-dd HH:mm" and the weekday name of the user's locale.
bool ResetTimeText(long long unix_seconds, std::wstring& text);

// Usage-limit resets an account holds (count and earliest expiry), for the taskbar item and tooltip.
struct ResetCredits
{
    bool available{};
    bool stale{};
    long long count{};
    bool has_expiry{};
    long long expires_at{};
};

// "1 · Claude (Expires : 2026-10-23 01:00 <weekday>)"; `name` is padded to `name_width` before the expiry.
std::wstring ResetCreditsLine(const std::wstring& name, size_t name_width, const ResetCredits& credits);
}
