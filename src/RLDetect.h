#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "GuiBase.h"
#include "MiniJson.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "version.h"

class ServerWrapper;
class ReplaySoccarWrapper;

constexpr auto kPluginTitle = "RLDetect";

enum class AnalysisStage
{
    Idle,
    MatchMarked,
    WaitingForMatchEnd,
    SavingReplay,
    WaitingForReplayFile,
    UploadingReplay,
    ParsingResponse,
    ShowingResults,
    Error
};

struct ReplayFileEntry
{
    std::filesystem::path path;
    uintmax_t fileSize = 0;
    long long modifiedTicks = 0;
    std::time_t modifiedTime = 0;
};

struct PlayerAnalysisRow
{
    std::string name;
    int confidencePercent = 0;
    bool hasTogglingValue = false;
    bool isToggling = false;
    bool isKBM = false;
    int teamNum = -1;
    std::vector<std::string> platformId;
    bool isActualBot = false;
};

struct AnalysisResultSet
{
    std::vector<PlayerAnalysisRow> players;
    int replayParseTimeMs = 0;
    int analysisTimeMs = 0;
    std::map<std::string, std::string> extraInfos;
    std::string rawResponse;
    std::filesystem::path replayPath;
    std::time_t completedAt = 0;
};

struct MatchContextSnapshot
{
    std::string localPlayerName;
    std::string playlistName;
    int localTeam = -1;
    bool valid = false;
};

class RLDetect final :
    public BakkesMod::Plugin::BakkesModPlugin,
    public SettingsWindowBase,
    public PluginWindowBase
{
public:
    RLDetect();
    ~RLDetect() override;

    void onLoad() override;
    void onUnload() override;

    void RenderSettings() override;
    void RenderWindow() override;

private:
    void RegisterCvars();
    void RegisterNotifiers();
    void RegisterHooks();

    void RefreshReplayFolderPath();
    void RefreshRecentReplays();
    std::vector<std::filesystem::path> BuildReplayFolderCandidates() const;
    std::vector<ReplayFileEntry> ScanReplayFiles() const;
    std::map<std::filesystem::path, long long> BuildReplaySnapshot() const;

    void HandleGameJoin(std::string eventName);
    void HandleMatchWinnerSet(std::string eventName);
    void HandleLeaveMatch(std::string eventName);
    void HandleRoundStart(ServerWrapper caller, void* params, std::string eventName);
    void HandleMatchComplete(ServerWrapper caller, void* params, std::string eventName);

    void MarkCurrentMatchForAnalysis(bool showToast);
    void ClearCurrentMatchMarkers();
    void CaptureMatchContext(ServerWrapper server);
    void ResetMatchContext();
    void StartQueuedSaveForCurrentMatch(const std::string& reason, bool showToast);
    void BeginReplayExportSequence(const std::string& reason, bool showToast);
    void BeginReplayExportSequenceWithServer(ServerWrapper server, const std::string& reason, bool showToast);
    void ScheduleReplayExportAttempt(const std::string& reason, int generation, int attempt, std::map<std::filesystem::path, long long> snapshot, std::chrono::system_clock::time_point startedAt);
    void ExecuteReplayExportAttempt(const std::string& reason, int generation, int attempt, const std::map<std::filesystem::path, long long>& snapshot, std::chrono::system_clock::time_point startedAt);
    void WaitForReplayFileAndAnalyze(const std::filesystem::path& requestedPath, int generation, int checkCount, const std::map<std::filesystem::path, long long>& snapshot, std::chrono::system_clock::time_point startedAt);
    void StartReplayWatcherThread(std::map<std::filesystem::path, long long> snapshot, std::chrono::system_clock::time_point startTime);
    void StopReplayWatcherThread();
    std::optional<ReplayFileEntry> DetectFreshReplayFile(
        const std::map<std::filesystem::path, long long>& snapshot,
        std::chrono::system_clock::time_point startTime) const;

    void AnalyzeReplayPath(const std::filesystem::path& replayPath, bool openResultsWindow);
    bool TryExportReplayFromServer(ServerWrapper server, std::filesystem::path& outReplayPath, std::string& outError);
    bool IsReplayReadyForExport(ServerWrapper server) const;
    std::filesystem::path BuildReplayExportPath(ServerWrapper server, ReplaySoccarWrapper replay) const;
    std::string BuildReplayBaseName(ServerWrapper server, ReplaySoccarWrapper replay) const;
    std::string GetPlaylistDisplayName(ServerWrapper server) const;
    std::string GetReplayFolderSummary() const;
    bool ReadBinaryFile(const std::filesystem::path& path, std::string& outBytes, std::string& outError) const;
    void ParseAnalysisResponse(const std::string& responseBody, AnalysisResultSet& result, std::string& outError) const;

    void SetStage(AnalysisStage stage, const std::string& message, bool clearError = false);
    void SetError(const std::string& message);
    void ShowToast(const std::string& title, const std::string& message, uint8_t toastType = 0);
    void Log(const std::string& message) const;

    void OpenMainWindow();
    void CloseMainWindow();
    void ToggleMainWindow();
    void ScheduleAutoCloseForResultWindow(float delaySeconds);

    // ---- UI rendering (new modern layout) ----
    void ApplyStyle();
    void RenderTopBar();
    void RenderTabAnalysis();
    void RenderTabReplays();
    void RenderTabSettings();
    void RenderTabCredits();
    void RenderActionButtons();
    void RenderStatusSection();
    void RenderResultsPanel();
    void RenderLoadingState();
    void RenderPlayersByTeam(const char* title, int teamNum, const ImVec4& accent);
    void RenderPlayerCard(const PlayerAnalysisRow& row, const ImVec4& accent);
    void RenderExtraInfoTable();
    void RenderRecentReplayPanel();
    void RenderBindEditor(const char* label, const char* hint, std::string& buffer, const std::function<void(const std::string&)>& applyFn);

    // ---- UI helper draws ----
    void RenderStatusBadge(const char* label, const ImVec4& color);
    void RenderPillBadge(const char* label, const ImVec4& bgColor, const ImVec4& textColor);
    void RenderSectionHeader(const char* label);
    void RenderStatCard(const char* label, const char* value, const ImVec4& accentColor);
    void RenderCustomProgressBar(float fraction, const ImVec4& fillColor, float height = 6.0f);
    void RenderTooltipMarker(const char* text);

    std::string BuildSummaryText() const;
    static std::string TrimCopy(std::string value);
    static std::string SanitizeFilename(std::string value);
    static std::string ToDisplayTime(std::time_t t);
    static std::string FormatBytes(uintmax_t bytes);
    static long long ToTicks(std::filesystem::file_time_type time);
    static std::time_t ToTimeT(std::filesystem::file_time_type time);
    static std::string JsonValueToDisplayString(const minijson::Value& value);

private:
    mutable std::mutex stateMutex_;

    AnalysisStage stage_ = AnalysisStage::Idle;
    std::string stageMessage_ = "Ready";
    std::string errorMessage_;
    bool currentMatchMarked_ = false;
    bool currentMatchEnded_ = false;
    bool queuedSaveAttempted_ = false;
    bool replayWatcherRunning_ = false;
    bool resultsShouldAutoOpen_ = true;
    bool resultWindowAutoOpened_ = false;
    bool exportTriggeredForCurrentMatch_ = false;
    bool exportSequenceRunning_ = false;
    bool forceAnalysisTab_ = false;
    int exportSequenceGeneration_ = 0;

    std::filesystem::path replayFolderPath_;
    std::vector<std::filesystem::path> replayFolderCandidates_;
    std::vector<ReplayFileEntry> recentReplays_;
    int selectedReplayIndex_ = -1;

    std::optional<AnalysisResultSet> currentResult_;
    MatchContextSnapshot matchContext_;
    std::string trackedMatchGuid_;

    std::string menuBindBuffer_;
    std::string markBindBuffer_;
    std::string appliedMenuBind_;
    std::string appliedMarkBind_;
    bool cvarInitDone_ = false;
    float uiScale_ = 1.0f;
    int listeningBind_ = 0;  // 0=none, 1=menu key, 2=mark key

    std::thread replayWatcherThread_;
    std::atomic<bool> stopWatcher_{ false };
    std::chrono::steady_clock::time_point lastReplayRefreshAt_{};
};
