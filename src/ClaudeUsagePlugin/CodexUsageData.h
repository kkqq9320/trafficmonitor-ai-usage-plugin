#pragma once

#include <map>
#include <mutex>
#include <string>

enum class CodexUsageWindow
{
    Rolling5Hours,
    Rolling7Days,
};

// Codex usage limits for the "codex" limit bucket.
// Primary source: the Codex usage helper snapshot (%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\codex-usage.json).
// Fallback: newest token_count rate_limits event in %CODEX_HOME%\sessions\**\*.jsonl.
class CCodexUsageData
{
public:
    struct Metric
    {
        bool available{};
        double percentage{};
        bool has_window_minutes{};
        long long window_minutes{};
        bool has_reset_time{};
        long long reset_at_unix_seconds{};
        std::wstring reset_time_text;
        bool stale{};  // data older than the freshness limit, or its reset time already passed
    };

    // One observation of the "codex" limit bucket.
    struct RateLimitRecord
    {
        bool valid{};
        Metric rolling_5h;
        Metric rolling_7d;
        long long data_at_unix{};
        std::wstring source;
        std::wstring plan_type;
        std::wstring reached_type;
        bool limit_reached{};
    };

    struct Snapshot
    {
        Metric rolling_5h;
        Metric rolling_7d;
        std::wstring value_5h_text{ L"--" };
        std::wstring value_7d_text{ L"--" };
        std::wstring tooltip_text{ L"Codex usage limits unavailable" };
    };

public:
    static CCodexUsageData& Instance();

    void RefreshIfNeeded();
    void AutoStartBundledHelperIfNeeded();
    const std::wstring& GetValueText(CodexUsageWindow window) const;
    const Metric& GetMetric(CodexUsageWindow window) const;
    const std::wstring& GetTooltipText() const;

private:
    struct SessionFileState
    {
        unsigned long long parsed_size{};
        bool has_event{};
        RateLimitRecord event;
    };

    CCodexUsageData() = default;

    void Refresh(unsigned long long now_tick);
    bool ReloadHelperSnapshotIfChanged();
    void ScanSessionJsonl();
    Snapshot BuildSnapshot(const RateLimitRecord* record, const std::wstring& error_text, const std::wstring& helper_note) const;

private:
    mutable std::mutex m_state_mutex;
    Snapshot m_snapshot;
    bool m_refresh_in_progress{};
    bool m_helper_auto_start_attempted{};
    unsigned long long m_last_check_tick{};
    unsigned long long m_last_jsonl_tick{};
    unsigned long long m_last_build_tick{};

    // Only touched by the thread that owns m_refresh_in_progress.
    bool m_helper_snapshot_exists{};
    unsigned long long m_helper_snapshot_write_time{};
    RateLimitRecord m_helper_record;
    RateLimitRecord m_jsonl_record;
    std::wstring m_jsonl_error;
    std::map<std::wstring, SessionFileState> m_session_files;
};

#define g_codex_usage_data CCodexUsageData::Instance()
