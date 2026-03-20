#include "RLDetect.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <vector>

#include <ShlObj.h>
#include <shellapi.h>

#include "bakkesmod/core/http_structs.h"
#include "bakkesmod/wrappers/GameEvent/GameEventWrapper.h"
#include "bakkesmod/wrappers/GameEvent/ReplayDirectorWrapper.h"
#include "bakkesmod/wrappers/GameEvent/ReplaySoccarWrapper.h"
#include "bakkesmod/wrappers/GameEvent/GameSettingPlaylistWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarWrapper.h"
#include "bakkesmod/wrappers/GameObject/PriWrapper.h"
#include "bakkesmod/wrappers/GameObject/ReplayManagerWrapper.h"
#include "bakkesmod/wrappers/http/HttpWrapper.h"
#include "bakkesmod/wrappers/includes.h"

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

BAKKESMOD_PLUGIN(RLDetect, "RLDetect", plugin_version, PLUGINTYPE_FREEPLAY)

// ===========================================================================
// Anonymous helpers (unchanged business logic)
// ===========================================================================

namespace
{
    constexpr const char* kAnalyzeUrl = "https://www.rldetect.com/bm_api";
    constexpr const char* kCommandMark = "wb_mark_for_analysis";
    constexpr const char* kCommandAnalyzeCurrent = "wb_analyze_current";
    constexpr const char* kCommandAnalyzeLatest = "wb_analyze_latest";
    constexpr const char* kCommandOpenReplayFolder = "wb_open_replay_folder";

    constexpr const char* kCvarEnabled = "wb_enabled";
    constexpr const char* kCvarAutoAnalyze = "wb_auto_analyze";
    constexpr const char* kCvarOpenResults = "wb_open_results";
    constexpr const char* kCvarShowToasts = "wb_show_toasts";
    constexpr const char* kCvarReplayPollSeconds = "wb_replay_poll_seconds";
    constexpr const char* kCvarMenuBind = "wb_menu_bind";
    constexpr const char* kCvarMarkBind = "wb_mark_bind";
    constexpr const char* kCvarUiScale = "wb_ui_scale";

    std::string TrimLocal(std::string value)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    ImVec4 MixColor(const ImVec4& a, const ImVec4& b, float t)
    {
        const float clamped = std::max(0.0f, std::min(1.0f, t));
        return ImVec4(
            a.x + (b.x - a.x) * clamped,
            a.y + (b.y - a.y) * clamped,
            a.z + (b.z - a.z) * clamped,
            a.w + (b.w - a.w) * clamped);
    }

    bool IsBlankKey(const std::string& value)
    {
        const std::string trimmed = TrimLocal(value);
        return trimmed.empty() || trimmed == "None" || trimmed == "none";
    }

    bool GetBoolCvar(const std::shared_ptr<CVarManagerWrapper>& manager, const char* name, bool fallback = false)
    {
        if (!manager)
        {
            return fallback;
        }
        CVarWrapper cvar = manager->getCvar(name);
        return cvar ? cvar.getBoolValue() : fallback;
    }

    int GetIntCvar(const std::shared_ptr<CVarManagerWrapper>& manager, const char* name, int fallback = 0)
    {
        if (!manager)
        {
            return fallback;
        }
        CVarWrapper cvar = manager->getCvar(name);
        return cvar ? cvar.getIntValue() : fallback;
    }

    std::string GetStringCvar(const std::shared_ptr<CVarManagerWrapper>& manager, const char* name, const std::string& fallback = "")
    {
        if (!manager)
        {
            return fallback;
        }
        CVarWrapper cvar = manager->getCvar(name);
        return cvar ? cvar.getStringValue() : fallback;
    }

    std::string CleanDisplayText(std::string value)
    {
        value = TrimLocal(value);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2);
        }
        return TrimLocal(value);
    }

    std::string FriendlyLabel(std::string key)
    {
        key = CleanDisplayText(std::move(key));
        if (key == "Replay name") return "Replay";
        if (key == "Recorded FPS") return "Recorded FPS";
        if (key == "Blue's final score") return "Blue score";
        if (key == "Orange's final score") return "Orange score";
        if (key == "Map name") return "Map";
        if (key == "Replay version ID") return "Replay version";
        if (key == "Replay total recorded time") return "Duration";
        if (key == "Game server ID") return "Server ID";
        if (key == "Server region") return "Region";
        if (key == "Server name") return "Server";
        return key;
    }

    std::string FriendlyValue(std::string value)
    {
        value = CleanDisplayText(std::move(value));
        if (value.empty())
        {
            return "Unknown";
        }
        return value;
    }

    std::string GetSafePlayerName(const PlayerAnalysisRow& row)
    {
        const std::string cleaned = CleanDisplayText(row.name);
        if (cleaned.empty())
        {
            return "Unknown player";
        }
        return cleaned;
    }

    std::string GetPlatformLabel(const PlayerAnalysisRow& row)
    {
        if (!row.platformId.empty())
        {
            const std::string platform = FriendlyValue(row.platformId.front());
            if (!platform.empty())
            {
                return platform;
            }
        }
        return "Unknown platform";
    }

    std::string GetAccountId(const PlayerAnalysisRow& row)
    {
        if (row.platformId.size() > 1)
        {
            return FriendlyValue(row.platformId[1]);
        }
        return "Unknown";
    }

    std::string GetInputMethodLabel(const PlayerAnalysisRow& row)
    {
        return row.isKBM ? "Keyboard & mouse" : "Controller";
    }

    std::string GetTogglingLabel(const PlayerAnalysisRow& row)
    {
        if (!row.hasTogglingValue)
        {
            return "No signal";
        }
        return row.isToggling ? "Detected" : "Not detected";
    }

    void CopyTextToClipboard(const std::string& text)
    {
        if (!text.empty())
        {
            ImGui::SetClipboardText(text.c_str());
        }
    }

    // Converts an ImVec4 color to ImU32 (packed RGBA).
    ImU32 ImColorU32(const ImVec4& col)
    {
        return IM_COL32(
            static_cast<int>(col.x * 255.0f + 0.5f),
            static_cast<int>(col.y * 255.0f + 0.5f),
            static_cast<int>(col.z * 255.0f + 0.5f),
            static_cast<int>(col.w * 255.0f + 0.5f));
    }

    ImU32 ImColorU32Alpha(const ImVec4& col, float alpha)
    {
        return IM_COL32(
            static_cast<int>(col.x * 255.0f + 0.5f),
            static_cast<int>(col.y * 255.0f + 0.5f),
            static_cast<int>(col.z * 255.0f + 0.5f),
            static_cast<int>(alpha * 255.0f + 0.5f));
    }
}

// ===========================================================================
// Constructor / destructor / onLoad / onUnload
// ===========================================================================

RLDetect::RLDetect()
{
    menuTitle_ = kPluginTitle;
}

RLDetect::~RLDetect()
{
    StopReplayWatcherThread();
}

void RLDetect::onLoad()
{
    _globalCvarManager = cvarManager;

    RegisterCvars();
    RegisterNotifiers();
    RegisterHooks();

    RefreshReplayFolderPath();
    RefreshRecentReplays();

    SetStage(AnalysisStage::Idle, "Ready");

    // Delay marking init as done so that CVar restoration callbacks (which fire
    // during registerCvar / config load) do NOT trigger writeconfig.  BakkesMod
    // may still be restoring its own default binds (F2, F6, etc.) at this point,
    // and an early writeconfig would overwrite the config with missing binds.
    gameWrapper->SetTimeout([this](GameWrapper*)
    {
        cvarInitDone_ = true;
    }, 1.0f);

    gameWrapper->SetTimeout([this](GameWrapper*)
    {
        ShowToast("RLDetect", "Plugin loaded. Use the menu or bind a key to mark matches.");
    }, 0.2f);
}

void RLDetect::onUnload()
{
    StopReplayWatcherThread();

    _globalCvarManager.reset();
}

// ===========================================================================
// CVars / Notifiers / Hooks  (unchanged)
// ===========================================================================

void RLDetect::RegisterCvars()
{
    CVarWrapper enabled = cvarManager->registerCvar(kCvarEnabled, "1", "Enable RLDetect", true, true, 0.0f, true, 1.0f);
    CVarWrapper autoAnalyze = cvarManager->registerCvar(kCvarAutoAnalyze, "0", "Automatically save and analyze every finished match", true, true, 0.0f, true, 1.0f);
    CVarWrapper openResults = cvarManager->registerCvar(kCvarOpenResults, "1", "Open the result window automatically after analysis", true, true, 0.0f, true, 1.0f);
    CVarWrapper showToasts = cvarManager->registerCvar(kCvarShowToasts, "1", "Show toast notifications", true, true, 0.0f, true, 1.0f);
    CVarWrapper pollSeconds = cvarManager->registerCvar(kCvarReplayPollSeconds, "35", "Legacy timeout setting from the queued-save workflow", true, true, 5.0f, true, 120.0f);
    CVarWrapper menuBind = cvarManager->registerCvar(kCvarMenuBind, "F7", "Key used to open the plugin menu");
    CVarWrapper markBind = cvarManager->registerCvar(kCvarMarkBind, "F8", "Key used to mark the current match for analysis");
    CVarWrapper uiScaleCvar = cvarManager->registerCvar(kCvarUiScale, "1.0", "UI zoom scale", true, true, 0.6f, true, 1.5f);

    (void)enabled;
    (void)autoAnalyze;
    (void)openResults;
    (void)showToasts;
    (void)pollSeconds;

    uiScale_ = uiScaleCvar.getFloatValue();
    uiScaleCvar.addOnValueChanged([this](std::string, CVarWrapper cvar)
    {
        uiScale_ = cvar.getFloatValue();
    });

    menuBindBuffer_ = menuBind.getStringValue();
    markBindBuffer_ = markBind.getStringValue();

    auto applyBind = [this](CVarWrapper cvar, std::string& appliedCache, const std::string& command)
    {
        const std::string newValue = TrimCopy(cvar.getStringValue());
        if (!appliedCache.empty() && !IsBlankKey(appliedCache))
        {
            cvarManager->removeBind(appliedCache);
        }

        appliedCache = newValue;
        if (!IsBlankKey(newValue))
        {
            cvarManager->setBind(newValue, command);
        }
    };

    menuBind.addOnValueChanged([this, applyBind](std::string, CVarWrapper cvar)
    {
        menuBindBuffer_ = cvar.getStringValue();
        applyBind(cvar, appliedMenuBind_, std::string("togglemenu ") + GetMenuName());
        if (cvarInitDone_)
        {
            cvarManager->executeCommand("writeconfig", false);
        }
    });

    markBind.addOnValueChanged([this, applyBind](std::string, CVarWrapper cvar)
    {
        markBindBuffer_ = cvar.getStringValue();
        applyBind(cvar, appliedMarkBind_, kCommandMark);
        if (cvarInitDone_)
        {
            cvarManager->executeCommand("writeconfig", false);
        }
    });

    applyBind(menuBind, appliedMenuBind_, std::string("togglemenu ") + GetMenuName());
    applyBind(markBind, appliedMarkBind_, kCommandMark);
}

void RLDetect::RegisterNotifiers()
{
    cvarManager->registerNotifier(kCommandMark, [this](std::vector<std::string>)
    {
        if (!GetBoolCvar(cvarManager, kCvarEnabled, true))
        {
            ShowToast("RLDetect", "Plugin is disabled.");
            return;
        }

        if (!gameWrapper)
        {
            return;
        }

        gameWrapper->Execute([this](GameWrapper*)
        {
            Log("Mark command received.");
            if (!gameWrapper->IsInOnlineGame())
            {
                SetError("No active online match is available to mark right now.");
                return;
            }

            MarkCurrentMatchForAnalysis(true);
        });
    }, "Marks the current match for automatic replay export and upload when it ends", PERMISSION_ALL);

    cvarManager->registerNotifier(kCommandAnalyzeCurrent, [this](std::vector<std::string>)
    {
        StartQueuedSaveForCurrentMatch("Manual analysis request", true);
    }, "Export the current/post-game replay immediately when possible, or mark the live match to export at the end", PERMISSION_ALL);

    cvarManager->registerNotifier(kCommandAnalyzeLatest, [this](std::vector<std::string>)
    {
        RefreshRecentReplays();
        std::vector<ReplayFileEntry> replays;
        {
            std::scoped_lock lock(stateMutex_);
            replays = recentReplays_;
        }

        if (replays.empty())
        {
            SetError("No replay files were found in the Rocket League replay folder.");
            return;
        }

        AnalyzeReplayPath(replays.front().path, true);
    }, "Analyze the newest saved replay file", PERMISSION_ALL);

    cvarManager->registerNotifier(kCommandOpenReplayFolder, [this](std::vector<std::string>)
    {
        RefreshReplayFolderPath();
        if (!replayFolderPath_.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(replayFolderPath_, ec);
            ShellExecuteW(nullptr, L"open", replayFolderPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }, "Open the Rocket League replay folder", PERMISSION_ALL);
}

void RLDetect::RegisterHooks()
{
    gameWrapper->HookEventWithCaller<ServerWrapper>("Function GameEvent_Soccar_TA.Active.StartRound", std::bind(&RLDetect::HandleRoundStart, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    gameWrapper->HookEventWithCaller<ServerWrapper>("Function TAGame.GameEvent_Soccar_TA.OnMatchWinnerSet", std::bind(&RLDetect::HandleMatchComplete, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    gameWrapper->HookEventWithCaller<ServerWrapper>("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded", std::bind(&RLDetect::HandleMatchComplete, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

// ===========================================================================
// Replay folder discovery  (unchanged)
// ===========================================================================

void RLDetect::RefreshReplayFolderPath()
{
    replayFolderCandidates_ = BuildReplayFolderCandidates();

    std::filesystem::path preferredFolder;
    long long newestTick = std::numeric_limits<long long>::min();
    std::error_code ec;

    for (const auto& folder : replayFolderCandidates_)
    {
        if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec))
        {
            ec.clear();
            continue;
        }

        long long folderNewestTick = std::numeric_limits<long long>::min();
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        {
            if (ec)
            {
                ec.clear();
                break;
            }

            if (!entry.is_regular_file())
            {
                continue;
            }

            if (entry.path().extension() != ".replay")
            {
                continue;
            }

            folderNewestTick = std::max(folderNewestTick, ToTicks(entry.last_write_time()));
        }

        if (folderNewestTick > newestTick)
        {
            newestTick = folderNewestTick;
            preferredFolder = folder;
        }
    }

    if (preferredFolder.empty())
    {
        for (const auto& folder : replayFolderCandidates_)
        {
            std::error_code folderEc;
            if (folder.filename() == "DemosEpic" && std::filesystem::exists(folder, folderEc))
            {
                preferredFolder = folder;
                break;
            }
        }

        if (preferredFolder.empty())
        {
            for (const auto& folder : replayFolderCandidates_)
            {
                std::error_code folderEc;
                if (std::filesystem::exists(folder, folderEc))
                {
                    preferredFolder = folder;
                    break;
                }
            }
        }

        if (preferredFolder.empty() && !replayFolderCandidates_.empty())
        {
            preferredFolder = replayFolderCandidates_.front();
        }
    }

    replayFolderPath_ = preferredFolder;
}

std::vector<std::filesystem::path> RLDetect::BuildReplayFolderCandidates() const
{
    std::filesystem::path tagameRoot;

    PWSTR documentsPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documentsPath)))
    {
        tagameRoot = std::filesystem::path(documentsPath) / "My Games" / "Rocket League" / "TAGame";
        CoTaskMemFree(documentsPath);
    }
    else
    {
        wchar_t* userProfile = nullptr;
        size_t userProfileLen = 0;
        if (_wdupenv_s(&userProfile, &userProfileLen, L"USERPROFILE") == 0 && userProfile != nullptr)
        {
            tagameRoot = std::filesystem::path(userProfile) / "Documents" / "My Games" / "Rocket League" / "TAGame";
            free(userProfile);
        }
    }

    if (tagameRoot.empty())
    {
        return {};
    }

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(tagameRoot / "Demos");
    candidates.push_back(tagameRoot / "DemosEpic");

    std::vector<std::filesystem::path> unique;
    for (const auto& folder : candidates)
    {
        if (std::find(unique.begin(), unique.end(), folder) == unique.end())
        {
            unique.push_back(folder);
        }
    }

    return unique;
}

std::vector<ReplayFileEntry> RLDetect::ScanReplayFiles() const
{
    std::vector<ReplayFileEntry> found;
    std::error_code ec;

    for (const auto& folder : replayFolderCandidates_)
    {
        if (folder.empty() || !std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec))
        {
            ec.clear();
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        {
            if (ec)
            {
                ec.clear();
                break;
            }

            if (!entry.is_regular_file())
            {
                continue;
            }

            if (entry.path().extension() != ".replay")
            {
                continue;
            }

            ReplayFileEntry item;
            item.path = entry.path();
            item.fileSize = entry.file_size();
            item.modifiedTicks = ToTicks(entry.last_write_time());
            item.modifiedTime = ToTimeT(entry.last_write_time());
            found.push_back(std::move(item));
        }
    }

    std::sort(found.begin(), found.end(), [](const ReplayFileEntry& a, const ReplayFileEntry& b)
    {
        if (a.modifiedTicks != b.modifiedTicks)
        {
            return a.modifiedTicks > b.modifiedTicks;
        }
        return a.path.filename().string() < b.path.filename().string();
    });

    return found;
}

std::map<std::filesystem::path, long long> RLDetect::BuildReplaySnapshot() const
{
    std::map<std::filesystem::path, long long> snapshot;
    for (const auto& replay : ScanReplayFiles())
    {
        snapshot[replay.path] = replay.modifiedTicks;
    }
    return snapshot;
}

void RLDetect::RefreshRecentReplays()
{
    RefreshReplayFolderPath();
    const auto replays = ScanReplayFiles();

    std::scoped_lock lock(stateMutex_);
    recentReplays_ = replays;
    lastReplayRefreshAt_ = std::chrono::steady_clock::now();
    if (recentReplays_.empty())
    {
        selectedReplayIndex_ = -1;
    }
    else if (selectedReplayIndex_ < 0 || selectedReplayIndex_ >= static_cast<int>(recentReplays_.size()))
    {
        selectedReplayIndex_ = 0;
    }
}

std::string RLDetect::GetReplayFolderSummary() const
{
    if (replayFolderCandidates_.empty())
    {
        return "Replay folder not found.";
    }

    std::ostringstream oss;
    oss << "Active: " << replayFolderPath_.string();
    if (replayFolderCandidates_.size() > 1)
    {
        oss << " | Scanning: ";
        for (size_t i = 0; i < replayFolderCandidates_.size(); ++i)
        {
            if (i > 0)
            {
                oss << " ; ";
            }
            oss << replayFolderCandidates_[i].string();
        }
    }

    return oss.str();
}

// ===========================================================================
// Game event handlers  (unchanged)
// ===========================================================================

void RLDetect::HandleGameJoin(std::string) {}

void RLDetect::HandleRoundStart(ServerWrapper caller, void*, std::string)
{
    const std::string matchGuid = caller ? caller.GetMatchGUID() : std::string();

    bool clearedForNewMatch = false;
    bool stillMarked = false;
    {
        std::scoped_lock lock(stateMutex_);

        if (!matchGuid.empty() && trackedMatchGuid_ != matchGuid)
        {
            trackedMatchGuid_ = matchGuid;
            currentMatchMarked_ = false;
            currentMatchEnded_ = false;
            queuedSaveAttempted_ = false;
            exportTriggeredForCurrentMatch_ = false;
            exportSequenceRunning_ = false;
            ++exportSequenceGeneration_;
            clearedForNewMatch = true;
        }

        stillMarked = currentMatchMarked_;
    }

    CaptureMatchContext(caller);
    Log(std::string("StartRound hook fired. guid=") + (matchGuid.empty() ? "<empty>" : matchGuid) +
        ", clearedForNewMatch=" + (clearedForNewMatch ? "true" : "false") +
        ", marked=" + (stillMarked ? "true" : "false"));

    if (!stillMarked)
    {
        SetStage(AnalysisStage::Idle, "Match started. Ready to mark this game for analysis.", true);
    }
}

void RLDetect::HandleMatchComplete(ServerWrapper caller, void*, std::string eventName)
{
    if (!GetBoolCvar(cvarManager, kCvarEnabled, true))
    {
        Log("Match-complete hook ignored because the plugin is disabled.");
        return;
    }

    CaptureMatchContext(caller);

    const std::string matchGuid = caller ? caller.GetMatchGUID() : std::string();
    const bool autoAnalyze = GetBoolCvar(cvarManager, kCvarAutoAnalyze, false);

    bool marked = false;
    bool alreadyTriggered = false;
    bool alreadyRunning = false;
    bool shouldExport = false;
    {
        std::scoped_lock lock(stateMutex_);
        currentMatchEnded_ = true;
        marked = currentMatchMarked_;
        alreadyTriggered = exportTriggeredForCurrentMatch_;
        alreadyRunning = exportSequenceRunning_;
        shouldExport = marked || autoAnalyze;
    }

    Log(std::string("Match-complete hook fired: ") + eventName +
        ", guid=" + (matchGuid.empty() ? "<empty>" : matchGuid) +
        ", marked=" + (marked ? "true" : "false") +
        ", autoAnalyze=" + (autoAnalyze ? "true" : "false") +
        ", exportTriggered=" + (alreadyTriggered ? "true" : "false") +
        ", exportRunning=" + (alreadyRunning ? "true" : "false"));

    if (!shouldExport || alreadyTriggered || alreadyRunning)
    {
        return;
    }

    // When the user manually marked the match (F8), open the window immediately
    // so they see the loading/analysis progress live — no need to press F7.
    if (marked)
    {
        forceAnalysisTab_ = true;
        OpenMainWindow();
    }

    BeginReplayExportSequenceWithServer(caller, "Match ended. Exporting replay...", true);
}

void RLDetect::HandleMatchWinnerSet(std::string) {}

void RLDetect::HandleLeaveMatch(std::string)
{
    Log("LeaveMatch received. Cancelling any pending export sequence.");
    ClearCurrentMatchMarkers();
    ResetMatchContext();
    SetStage(AnalysisStage::Idle, "Ready", true);
}

// ===========================================================================
// Match marking / context  (unchanged)
// ===========================================================================

void RLDetect::MarkCurrentMatchForAnalysis(bool showToast)
{
    if (!gameWrapper)
    {
        return;
    }

    ServerWrapper server = gameWrapper->IsInOnlineGame() ? gameWrapper->GetOnlineGame() : gameWrapper->GetCurrentGameState();
    if (server)
    {
        CaptureMatchContext(server);
    }

    {
        std::scoped_lock lock(stateMutex_);
        currentMatchMarked_ = true;
        currentMatchEnded_ = false;
        queuedSaveAttempted_ = false;
        exportTriggeredForCurrentMatch_ = false;
        exportSequenceRunning_ = false;
        if (server)
        {
            trackedMatchGuid_ = server.GetMatchGUID();
        }
    }

    Log(std::string("Current match marked. guid=") + (server ? server.GetMatchGUID() : std::string("<null>")));
    SetStage(AnalysisStage::WaitingForMatchEnd, "Current match marked. Replay will be exported automatically when the match ends.", true);

    if (showToast)
    {
        ShowToast("RLDetect", "Current match marked. The replay will be exported and uploaded after the match ends.");
    }
}

void RLDetect::ClearCurrentMatchMarkers()
{
    std::scoped_lock lock(stateMutex_);
    currentMatchMarked_ = false;
    currentMatchEnded_ = false;
    queuedSaveAttempted_ = false;
    exportTriggeredForCurrentMatch_ = false;
    exportSequenceRunning_ = false;
    ++exportSequenceGeneration_;
}

void RLDetect::CaptureMatchContext(ServerWrapper server)
{
    MatchContextSnapshot snapshot;
    snapshot.localTeam = -1;

    if (gameWrapper)
    {
        const std::string gwName = TrimCopy(gameWrapper->GetPlayerName().ToString());
        if (!gwName.empty())
        {
            snapshot.localPlayerName = gwName;
        }
    }

    if (server)
    {
        snapshot.playlistName = GetPlaylistDisplayName(server);

        if (gameWrapper)
        {
            auto localCar = gameWrapper->GetLocalCar();
            if (localCar)
            {
                auto pri = localCar.GetPRI();
                if (pri)
                {
                    const std::string priName = TrimCopy(pri.GetPlayerName().ToString());
                    if (!priName.empty())
                    {
                        snapshot.localPlayerName = priName;
                    }
                    snapshot.localTeam = static_cast<int>(pri.GetTeamNum());
                }
            }
        }

        if (snapshot.localTeam < 0)
        {
            auto localPlayers = server.GetLocalPlayers();
            if (localPlayers.Count() > 0)
            {
                auto controller = localPlayers.Get(0);
                if (controller)
                {
                    auto pri = controller.GetPRI();
                    if (pri)
                    {
                        const std::string priName = TrimCopy(pri.GetPlayerName().ToString());
                        if (!priName.empty())
                        {
                            snapshot.localPlayerName = priName;
                        }
                        snapshot.localTeam = static_cast<int>(pri.GetTeamNum());
                    }
                }
            }
        }
    }

    snapshot.valid = !snapshot.localPlayerName.empty() || !snapshot.playlistName.empty() || snapshot.localTeam >= 0;

    std::scoped_lock lock(stateMutex_);
    matchContext_ = std::move(snapshot);
}

void RLDetect::ResetMatchContext()
{
    std::scoped_lock lock(stateMutex_);
    matchContext_ = MatchContextSnapshot{};
}

// ===========================================================================
// Export / watcher / upload pipeline  (unchanged)
// ===========================================================================

void RLDetect::StartQueuedSaveForCurrentMatch(const std::string& reason, bool showToast)
{
    if (!GetBoolCvar(cvarManager, kCvarEnabled, true))
    {
        SetError("Plugin is disabled.");
        return;
    }

    gameWrapper->Execute([this, reason, showToast](GameWrapper*)
    {
        ServerWrapper server = gameWrapper->GetCurrentGameState();
        if (!server)
        {
            server = gameWrapper->GetOnlineGame();
        }

        if (!server)
        {
            SetError("No active or post-game match is available right now.");
            return;
        }

        CaptureMatchContext(server);

        if (gameWrapper->IsInOnlineGame() && !IsReplayReadyForExport(server))
        {
            MarkCurrentMatchForAnalysis(showToast);
            SetStage(AnalysisStage::WaitingForMatchEnd, "Current match marked. The replay will be exported automatically when the match ends.", true);
            return;
        }

        BeginReplayExportSequence(reason.empty() ? "Exporting replay for analysis..." : reason, showToast);
    });
}

void RLDetect::BeginReplayExportSequence(const std::string& reason, bool showToast)
{
    RefreshReplayFolderPath();
    const auto snapshot = BuildReplaySnapshot();
    const auto startedAt = std::chrono::system_clock::now();

    int generation = 0;
    {
        std::scoped_lock lock(stateMutex_);
        exportTriggeredForCurrentMatch_ = true;
        exportSequenceRunning_ = true;
        generation = ++exportSequenceGeneration_;
    }

    SetStage(AnalysisStage::SavingReplay, reason.empty() ? "Exporting replay..." : reason, true);
    if (showToast)
    {
        ShowToast("RLDetect", "Preparing replay export...");
    }

    ScheduleReplayExportAttempt(reason, generation, 0, snapshot, startedAt);
}

void RLDetect::BeginReplayExportSequenceWithServer(ServerWrapper server, const std::string& reason, bool showToast)
{
    RefreshReplayFolderPath();
    const auto snapshot = BuildReplaySnapshot();
    const auto startedAt = std::chrono::system_clock::now();

    int generation = 0;
    {
        std::scoped_lock lock(stateMutex_);
        exportTriggeredForCurrentMatch_ = true;
        exportSequenceRunning_ = true;
        generation = ++exportSequenceGeneration_;
    }

    SetStage(AnalysisStage::SavingReplay, reason.empty() ? "Exporting replay..." : reason, true);
    if (showToast)
    {
        ShowToast("RLDetect", "Preparing replay export...");
    }

    std::filesystem::path replayPath;
    std::string exportError;
    if (server && TryExportReplayFromServer(server, replayPath, exportError))
    {
        Log("Immediate replay export requested from match-end caller: " + replayPath.string());
        SetStage(AnalysisStage::WaitingForReplayFile, "Waiting for the replay file to finish writing...", true);
        WaitForReplayFileAndAnalyze(replayPath, generation, 0, snapshot, startedAt);
        return;
    }

    if (!exportError.empty())
    {
        Log("Immediate caller export was not ready: " + exportError);
    }

    ScheduleReplayExportAttempt(reason, generation, 0, snapshot, startedAt);
}

void RLDetect::ScheduleReplayExportAttempt(const std::string& reason, int generation, int attempt, std::map<std::filesystem::path, long long> snapshot, std::chrono::system_clock::time_point startedAt)
{
    if (!gameWrapper)
    {
        return;
    }

    static const float delays[] = { 0.10f, 0.30f, 0.55f, 0.85f, 1.20f, 1.60f, 2.10f };
    const size_t delayCount = sizeof(delays) / sizeof(delays[0]);
    const size_t delayIndex = static_cast<size_t>(std::min(attempt, static_cast<int>(delayCount - 1)));

    gameWrapper->SetTimeout([this, reason, generation, attempt, snapshot = std::move(snapshot), startedAt](GameWrapper*)
    {
        ExecuteReplayExportAttempt(reason, generation, attempt, snapshot, startedAt);
    }, delays[delayIndex]);
}

void RLDetect::ExecuteReplayExportAttempt(const std::string& reason, int generation, int attempt, const std::map<std::filesystem::path, long long>& snapshot, std::chrono::system_clock::time_point startedAt)
{
    {
        std::scoped_lock lock(stateMutex_);
        if (generation != exportSequenceGeneration_ || !exportSequenceRunning_)
        {
            return;
        }
    }

    ServerWrapper server = gameWrapper->GetCurrentGameState();
    if (!server)
    {
        server = gameWrapper->GetOnlineGame();
    }

    if (!server)
    {
        if (attempt < 6)
        {
            ScheduleReplayExportAttempt(reason, generation, attempt + 1, snapshot, startedAt);
            return;
        }

        {
            std::scoped_lock lock(stateMutex_);
            exportSequenceRunning_ = false;
            exportTriggeredForCurrentMatch_ = false;
        }
        SetError("Replay export failed because the match state disappeared before the replay became available.");
        return;
    }

    CaptureMatchContext(server);

    std::filesystem::path replayPath;
    std::string exportError;
    if (!TryExportReplayFromServer(server, replayPath, exportError))
    {
        Log("Replay export attempt " + std::to_string(attempt + 1) + " failed: " + exportError);
        if (attempt < 6)
        {
            SetStage(AnalysisStage::SavingReplay, "Waiting for the replay to become exportable...", true);
            ScheduleReplayExportAttempt(reason, generation, attempt + 1, snapshot, startedAt);
            return;
        }

        {
            std::scoped_lock lock(stateMutex_);
            exportSequenceRunning_ = false;
            exportTriggeredForCurrentMatch_ = false;
        }
        SetError(exportError);
        return;
    }

    Log("Replay export requested: " + replayPath.string());
    SetStage(AnalysisStage::WaitingForReplayFile, "Waiting for the replay file to finish writing...", true);
    WaitForReplayFileAndAnalyze(replayPath, generation, 0, snapshot, startedAt);
}

void RLDetect::WaitForReplayFileAndAnalyze(const std::filesystem::path& requestedPath, int generation, int checkCount, const std::map<std::filesystem::path, long long>& snapshot, std::chrono::system_clock::time_point startedAt)
{
    {
        std::scoped_lock lock(stateMutex_);
        if (generation != exportSequenceGeneration_ || !exportSequenceRunning_)
        {
            return;
        }
    }

    std::error_code ec;
    std::filesystem::path resolvedPath = requestedPath;

    auto fileIsReady = [&](const std::filesystem::path& path) -> bool
    {
        ec.clear();
        if (!std::filesystem::exists(path, ec) || ec)
        {
            return false;
        }

        const auto fileSize = std::filesystem::file_size(path, ec);
        return !ec && fileSize > 0;
    };

    if (!fileIsReady(resolvedPath))
    {
        if (const auto freshReplay = DetectFreshReplayFile(snapshot, startedAt); freshReplay.has_value())
        {
            resolvedPath = freshReplay->path;
        }
    }

    if (fileIsReady(resolvedPath))
    {
        {
            std::scoped_lock lock(stateMutex_);
            currentMatchMarked_ = false;
            currentMatchEnded_ = true;
            queuedSaveAttempted_ = false;
            exportTriggeredForCurrentMatch_ = true;
            exportSequenceRunning_ = false;
        }

        RefreshRecentReplays();
        Log("Replay file ready: " + resolvedPath.string());
        AnalyzeReplayPath(resolvedPath, GetBoolCvar(cvarManager, kCvarOpenResults, true));
        return;
    }

    if (checkCount >= 16)
    {
        {
            std::scoped_lock lock(stateMutex_);
            exportSequenceRunning_ = false;
            exportTriggeredForCurrentMatch_ = false;
        }
        SetError("Replay export did not produce a readable .replay file in time. Stay on the post-game screen a bit longer, then try again from the plugin window.");
        return;
    }

    gameWrapper->SetTimeout([this, requestedPath, generation, checkCount, snapshot, startedAt](GameWrapper*)
    {
        WaitForReplayFileAndAnalyze(requestedPath, generation, checkCount + 1, snapshot, startedAt);
    }, 0.25f);
}

void RLDetect::StartReplayWatcherThread(std::map<std::filesystem::path, long long> snapshot, std::chrono::system_clock::time_point startTime)
{
    StopReplayWatcherThread();
    stopWatcher_ = false;

    {
        std::scoped_lock lock(stateMutex_);
        replayWatcherRunning_ = true;
    }

    const auto timeoutSeconds = std::max(5, GetIntCvar(cvarManager, kCvarReplayPollSeconds, 35));
    replayWatcherThread_ = std::thread([this, snapshot = std::move(snapshot), startTime, timeoutSeconds]()
    {
        const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeoutSeconds);

        std::optional<ReplayFileEntry> foundReplay;
        while (!stopWatcher_ && std::chrono::system_clock::now() < deadline)
        {
            foundReplay = DetectFreshReplayFile(snapshot, startTime);
            if (foundReplay.has_value())
            {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(350));
        }

        {
            std::scoped_lock lock(stateMutex_);
            replayWatcherRunning_ = false;
        }

        if (stopWatcher_)
        {
            return;
        }

        if (!foundReplay.has_value())
        {
            gameWrapper->Execute([this](GameWrapper*)
            {
                SetError("Timed out while waiting for the replay file to appear. Make sure Rocket League is allowed to save replays and the replay folder is writable.");
            });
            return;
        }

        gameWrapper->Execute([this, replayPath = foundReplay->path](GameWrapper*)
        {
            RefreshRecentReplays();
            AnalyzeReplayPath(replayPath, GetBoolCvar(cvarManager, kCvarOpenResults, true));
        });
    });
}

void RLDetect::StopReplayWatcherThread()
{
    stopWatcher_ = true;
    if (replayWatcherThread_.joinable())
    {
        replayWatcherThread_.join();
    }

    std::scoped_lock lock(stateMutex_);
    replayWatcherRunning_ = false;
}

std::optional<ReplayFileEntry> RLDetect::DetectFreshReplayFile(
    const std::map<std::filesystem::path, long long>& snapshot,
    std::chrono::system_clock::time_point startTime) const
{
    auto latestFiles = ScanReplayFiles();
    if (latestFiles.empty())
    {
        return std::nullopt;
    }

    const std::time_t earliestTime = std::chrono::system_clock::to_time_t(startTime) - 1;
    for (const auto& item : latestFiles)
    {
        if (item.modifiedTime < earliestTime)
        {
            continue;
        }

        const auto it = snapshot.find(item.path);
        if (it == snapshot.end() || item.modifiedTicks > it->second)
        {
            return item;
        }
    }

    return std::nullopt;
}

// ===========================================================================
// Export helpers  (unchanged)
// ===========================================================================

bool RLDetect::IsReplayReadyForExport(ServerWrapper server) const
{
    if (!server) return false;
    auto replayDirector = server.GetReplayDirector();
    if (!replayDirector) return false;
    auto replay = replayDirector.GetReplay();
    return static_cast<bool>(replay);
}

bool RLDetect::TryExportReplayFromServer(ServerWrapper server, std::filesystem::path& outReplayPath, std::string& outError)
{
    if (!server)
    {
        outError = "Server state is not available for replay export.";
        return false;
    }

    RefreshReplayFolderPath();
    if (replayFolderPath_.empty())
    {
        outError = "Failed to resolve the Rocket League replay folder.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(replayFolderPath_, ec);

    auto replayDirector = server.GetReplayDirector();
    if (!replayDirector)
    {
        outError = "Replay director is not available yet. Stay on the post-game screen for a moment and try again.";
        return false;
    }

    ReplaySoccarWrapper replay = replayDirector.GetReplay();
    if (!replay)
    {
        outError = "Replay data is not available yet. Stay on the post-game screen for a moment and try again.";
        return false;
    }

    const auto exportPath = BuildReplayExportPath(server, replay);
    replay.StopRecord();
    replay.SetReplayName(exportPath.stem().string());

    if (std::filesystem::exists(exportPath, ec))
    {
        std::filesystem::remove(exportPath, ec);
    }

    replay.ExportReplay(exportPath);
    outReplayPath = exportPath;
    return true;
}

std::filesystem::path RLDetect::BuildReplayExportPath(ServerWrapper server, ReplaySoccarWrapper replay) const
{
    const std::string baseName = BuildReplayBaseName(server, replay);
    std::filesystem::path candidate = replayFolderPath_ / (baseName + ".replay");

    std::error_code ec;
    for (int suffix = 2; std::filesystem::exists(candidate, ec); ++suffix)
    {
        candidate = replayFolderPath_ / (baseName + "_" + std::to_string(suffix) + ".replay");
        ec.clear();
    }

    return candidate;
}

std::string RLDetect::BuildReplayBaseName(ServerWrapper server, ReplaySoccarWrapper replay) const
{
    MatchContextSnapshot snapshot;
    {
        std::scoped_lock lock(stateMutex_);
        snapshot = matchContext_;
    }

    std::string playerName = !TrimCopy(snapshot.localPlayerName).empty() ? snapshot.localPlayerName : "Player";
    int localTeam = snapshot.localTeam;

    if (playerName == "Player" && gameWrapper)
    {
        const std::string gwName = TrimCopy(gameWrapper->GetPlayerName().ToString());
        if (!gwName.empty())
        {
            playerName = gwName;
        }
    }

    if (localTeam < 0 && gameWrapper)
    {
        auto localCar = gameWrapper->GetLocalCar();
        if (localCar)
        {
            auto pri = localCar.GetPRI();
            if (pri)
            {
                const std::string priName = TrimCopy(pri.GetPlayerName().ToString());
                if (!priName.empty())
                {
                    playerName = priName;
                }
                localTeam = static_cast<int>(pri.GetTeamNum());
            }
        }
    }

    if (localTeam < 0)
    {
        auto localPlayers = server.GetLocalPlayers();
        if (localPlayers.Count() > 0)
        {
            auto controller = localPlayers.Get(0);
            if (controller)
            {
                auto pri = controller.GetPRI();
                if (pri)
                {
                    const std::string priName = TrimCopy(pri.GetPlayerName().ToString());
                    if (!priName.empty())
                    {
                        playerName = priName;
                    }
                    localTeam = static_cast<int>(pri.GetTeamNum());
                }
            }
        }
    }

    const int blueScore = replay.GetTeam0Score();
    const int orangeScore = replay.GetTeam1Score();

    std::string outcome = "Game";
    if (localTeam == 0 || localTeam == 1)
    {
        const int myScore = localTeam == 0 ? blueScore : orangeScore;
        const int enemyScore = localTeam == 0 ? orangeScore : blueScore;
        outcome = myScore > enemyScore ? "Win" : (myScore < enemyScore ? "Loss" : "Tie");
    }

    std::string playlist = !TrimCopy(snapshot.playlistName).empty() ? snapshot.playlistName : GetPlaylistDisplayName(server);

    std::time_t now = std::time(nullptr);
    std::tm localTm{};
#ifdef _WIN32
    localtime_s(&localTm, &now);
#else
    localTm = *std::localtime(&now);
#endif

    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d_%H-%M-%S")
        << '_' << SanitizeFilename(playerName)
        << '_' << SanitizeFilename(playlist)
        << '_' << outcome
        << '_' << blueScore << '-' << orangeScore;

    return SanitizeFilename(oss.str());
}

std::string RLDetect::GetPlaylistDisplayName(ServerWrapper server) const
{
    auto playlist = server.GetPlaylist();
    if (playlist)
    {
        std::string localized = playlist.GetLocalizedName();
        if (!TrimCopy(localized).empty()) return localized;

        std::string name = playlist.GetName();
        if (!TrimCopy(name).empty()) return name;

        switch (playlist.GetPlaylistId())
        {
        case 1: return "Casual Duel";
        case 2: return "Casual Doubles";
        case 3: return "Casual Standard";
        case 4: return "Casual Chaos";
        case 6: return "Private Match";
        case 10: return "Ranked Duel";
        case 11: return "Ranked Doubles";
        case 12: return "Ranked Solo Standard";
        case 13: return "Ranked Standard";
        default: break;
        }
    }

    return gameWrapper->IsInOnlineGame() ? "Online Match" : "Match";
}

// ===========================================================================
// Upload / parse  (unchanged)
// ===========================================================================

void RLDetect::AnalyzeReplayPath(const std::filesystem::path& replayPath, bool openResultsWindow)
{
    Log("AnalyzeReplayPath requested for: " + replayPath.string());

    std::string replayBytes;
    std::string readError;
    if (!ReadBinaryFile(replayPath, replayBytes, readError))
    {
        SetError(readError);
        return;
    }

    resultsShouldAutoOpen_ = openResultsWindow;
    SetStage(AnalysisStage::UploadingReplay, "Uploading replay for analysis...", true);

    CurlRequest request;
    request.url = kAnalyzeUrl;
    request.verb = "POST";
    request.body = std::move(replayBytes);
    request.headers["accept"] = "*/*";
    request.headers["content-type"] = "application/replay";

    HttpWrapper::SendCurlRequest(request, [this, replayPath](int httpStatusCode, std::string data)
    {
        gameWrapper->Execute([this, httpStatusCode, data = std::move(data), replayPath](GameWrapper*)
        {
            if (httpStatusCode < 200 || httpStatusCode >= 300)
            {
                std::ostringstream oss;
                oss << "Upload failed. HTTP " << httpStatusCode;
                if (!data.empty())
                {
                    oss << " — " << data.substr(0, 300);
                }
                SetError(oss.str());
                return;
            }

            SetStage(AnalysisStage::ParsingResponse, "Parsing analysis response...", true);

            AnalysisResultSet result;
            std::string parseError;
            ParseAnalysisResponse(data, result, parseError);
            if (!parseError.empty())
            {
                SetError(parseError);
                return;
            }

            result.rawResponse = data;
            result.replayPath = replayPath;
            result.completedAt = std::time(nullptr);

            {
                std::scoped_lock lock(stateMutex_);
                currentResult_ = std::move(result);
                resultWindowAutoOpened_ = false;
            }

            RefreshRecentReplays();
            SetStage(AnalysisStage::ShowingResults, "Analysis completed successfully.", true);

            if (resultsShouldAutoOpen_)
            {
                {
                    std::scoped_lock lock(stateMutex_);
                    resultWindowAutoOpened_ = true;
                }
                forceAnalysisTab_ = true;
                OpenMainWindow();
            }

            ShowToast("RLDetect", BuildSummaryText());
        });
    });
}

bool RLDetect::ReadBinaryFile(const std::filesystem::path& path, std::string& outBytes, std::string& outError) const
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        outError = "Failed to open replay file: " + path.string();
        return false;
    }

    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size <= 0)
    {
        outError = "Replay file is empty: " + path.string();
        return false;
    }

    outBytes.resize(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
    if (!stream.good() && !stream.eof())
    {
        outError = "Failed to read replay file: " + path.string();
        return false;
    }

    return true;
}

void RLDetect::ParseAnalysisResponse(const std::string& responseBody, AnalysisResultSet& result, std::string& outError) const
{
    minijson::Value root;
    if (!minijson::Parse(responseBody, root, outError))
    {
        outError = "Failed to parse JSON response: " + outError;
        return;
    }

    if (!root.isObject())
    {
        outError = "Unexpected response: top-level JSON is not an object.";
        return;
    }

    const auto& players = root["player_results"];
    if (!players.isArray())
    {
        outError = "Unexpected response: player_results is missing or invalid.";
        return;
    }

    result.players.clear();
    for (const auto& playerValue : players.asArray())
    {
        if (!playerValue.isObject()) continue;

        PlayerAnalysisRow row;
        row.name = CleanDisplayText(playerValue["name"].stringOr("Unknown"));
        row.confidencePercent = playerValue["confidence_percent"].intOr(0);

        const auto& togglingValue = playerValue["is_toggling"];
        if (togglingValue.isBool())
        {
            row.hasTogglingValue = true;
            row.isToggling = togglingValue.asBool();
        }

        row.isKBM = playerValue["is_kbm"].boolOr(false);
        row.teamNum = playerValue["team_num"].intOr(-1);
        row.isActualBot = playerValue["is_actual_bot"].boolOr(false);

        const auto& platformArray = playerValue["platform_id"];
        if (platformArray.isArray())
        {
            for (const auto& entry : platformArray.asArray())
            {
                if (entry.isString())
                {
                    row.platformId.push_back(CleanDisplayText(entry.asString()));
                }
            }
        }

        result.players.push_back(std::move(row));
    }

    result.replayParseTimeMs = root["replay_parse_time_ms"].intOr(0);
    result.analysisTimeMs = root["analysis_time_ms"].intOr(0);

    result.extraInfos.clear();
    const auto& extraInfos = root["extra_infos"];
    if (extraInfos.isObject())
    {
        for (const auto& [key, value] : extraInfos.asObject())
        {
            result.extraInfos.emplace(CleanDisplayText(key), CleanDisplayText(JsonValueToDisplayString(value)));
        }
    }
}

// ===========================================================================
// State / toast / log helpers  (unchanged)
// ===========================================================================

void RLDetect::SetStage(AnalysisStage stage, const std::string& message, bool clearError)
{
    std::scoped_lock lock(stateMutex_);
    stage_ = stage;
    stageMessage_ = message;
    if (clearError) errorMessage_.clear();
}

void RLDetect::SetError(const std::string& message)
{
    {
        std::scoped_lock lock(stateMutex_);
        stage_ = AnalysisStage::Error;
        stageMessage_ = "An error occurred.";
        errorMessage_ = message;
    }
    ShowToast("RLDetect", message, 2);
}

void RLDetect::ShowToast(const std::string& title, const std::string& message, uint8_t toastType)
{
    if (!GetBoolCvar(cvarManager, kCvarShowToasts, true) || !gameWrapper)
    {
        return;
    }

    gameWrapper->Execute([this, title, message, toastType](GameWrapper*)
    {
        if (gameWrapper)
        {
            gameWrapper->Toast(title, message, "default", 4.0f, toastType, 420.0f, 72.0f);
        }
    });
}

void RLDetect::Log(const std::string& message) const
{
    if (cvarManager) cvarManager->log("[RLDetect] " + message);
}

void RLDetect::OpenMainWindow()
{
    if (!gameWrapper || !cvarManager) return;
    RefreshRecentReplays();
    gameWrapper->SetTimeout([this](GameWrapper*)
    {
        if (!isWindowOpen_) cvarManager->executeCommand("togglemenu " + GetMenuName(), false);
    }, 0.05f);
}

void RLDetect::CloseMainWindow()
{
    if (!gameWrapper || !cvarManager) return;
    gameWrapper->SetTimeout([this](GameWrapper*)
    {
        cvarManager->executeCommand("closemenu " + GetMenuName(), false);
    }, 0.01f);
}

void RLDetect::ToggleMainWindow()
{
    if (!gameWrapper || !cvarManager) return;
    gameWrapper->SetTimeout([this](GameWrapper*)
    {
        cvarManager->executeCommand("togglemenu " + GetMenuName(), false);
    }, 0.05f);
}

void RLDetect::ScheduleAutoCloseForResultWindow(float delaySeconds)
{
    if (!gameWrapper || !cvarManager || delaySeconds <= 0.0f) return;
    gameWrapper->SetTimeout([this](GameWrapper*)
    {
        bool shouldClose = false;
        {
            std::scoped_lock lock(stateMutex_);
            shouldClose = resultWindowAutoOpened_;
            resultWindowAutoOpened_ = false;
        }
        if (shouldClose) cvarManager->executeCommand("closemenu " + GetMenuName(), false);
    }, delaySeconds);
}

// ===========================================================================
// ===========================  U I   R E N D E R I N G  =====================
// ===========================================================================

void RLDetect::ApplyStyle()
{
    // Style is now applied via push/pop in GuiBase::Render() — nothing to do here.
}

// ---------------------------------------------------------------------------
// Reusable UI primitives
// ---------------------------------------------------------------------------

void RLDetect::RenderStatusBadge(const char* label, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3));
    ImGui::SmallButton(label);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}

void RLDetect::RenderPillBadge(const char* label, const ImVec4& bgColor, const ImVec4& textColor)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float padX = 8.0f;
    const float padY = 3.0f;
    const ImVec2 rectMin(cursor.x, cursor.y);
    const ImVec2 rectMax(cursor.x + textSize.x + padX * 2, cursor.y + textSize.y + padY * 2);
    draw->AddRectFilled(rectMin, rectMax, ImColorU32(bgColor), 10.0f);
    draw->AddText(ImVec2(cursor.x + padX, cursor.y + padY), ImColorU32(textColor), label);
    ImGui::Dummy(ImVec2(textSize.x + padX * 2, textSize.y + padY * 2));
}

void RLDetect::RenderSectionHeader(const char* label)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec4 accentColor(0.286f, 0.561f, 0.882f, 0.80f);
    draw->AddRectFilled(cursor, ImVec2(cursor.x + 3.0f, cursor.y + ImGui::GetTextLineHeight()), ImColorU32(accentColor), 2.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
    ImGui::TextColored(ImVec4(0.82f, 0.86f, 0.93f, 1.00f), "%s", label);
}

void RLDetect::RenderStatCard(const char* label, const char* value, const ImVec4& accentColor)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.098f, 0.114f, 0.157f, 0.95f));
    ImGui::BeginChild(label, ImVec2(0, 38 * uiScale_), true);
    ImGui::TextColored(accentColor, "%s", label);
    ImGui::SameLine();
    ImGui::Text("%s", value);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RLDetect::RenderCustomProgressBar(float fraction, const ImVec4& fillColor, float height)
{
    const float clamped = std::max(0.0f, std::min(1.0f, fraction));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec4 trackColor(0.145f, 0.165f, 0.220f, 1.00f);

    const ImVec2 trackMin(cursor.x, cursor.y);
    const ImVec2 trackMax(cursor.x + width, cursor.y + height);
    draw->AddRectFilled(trackMin, trackMax, ImColorU32(trackColor), height * 0.5f);

    if (clamped > 0.001f)
    {
        const ImVec2 fillMax(cursor.x + width * clamped, cursor.y + height);
        draw->AddRectFilled(trackMin, fillMax, ImColorU32(fillColor), height * 0.5f);
    }

    ImGui::Dummy(ImVec2(width, height));
}

void RLDetect::RenderTooltipMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(300.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------------------
// Settings panel (BakkesMod F2 menu — renders WITHOUT our custom window style)
// ---------------------------------------------------------------------------

void RLDetect::RenderSettings()
{
    ApplyStyle();

    ImGui::TextColored(ImVec4(0.62f, 0.82f, 1.00f, 1.00f), "RLDetect v%s", plugin_version);
    ImGui::TextWrapped("Detect bots in Rocket League. Export replays, upload them for analysis, and see results in-game.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool enabled = GetBoolCvar(cvarManager, kCvarEnabled, true);
    if (ImGui::Checkbox("Enable plugin", &enabled))
        cvarManager->getCvar(kCvarEnabled).setValue(enabled ? "1" : "0");

    bool autoAnalyze = GetBoolCvar(cvarManager, kCvarAutoAnalyze, false);
    if (ImGui::Checkbox("Auto-analyze every finished match", &autoAnalyze))
        cvarManager->getCvar(kCvarAutoAnalyze).setValue(autoAnalyze ? "1" : "0");

    bool openResults = GetBoolCvar(cvarManager, kCvarOpenResults, true);
    if (ImGui::Checkbox("Open result window automatically", &openResults))
        cvarManager->getCvar(kCvarOpenResults).setValue(openResults ? "1" : "0");

    bool showToasts = GetBoolCvar(cvarManager, kCvarShowToasts, true);
    if (ImGui::Checkbox("Show toast notifications", &showToasts))
        cvarManager->getCvar(kCvarShowToasts).setValue(showToasts ? "1" : "0");

    ImGui::Spacing();

    int waitSeconds = GetIntCvar(cvarManager, kCvarReplayPollSeconds, 35);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Replay save timeout (s)", &waitSeconds, 5, 120))
        cvarManager->getCvar(kCvarReplayPollSeconds).setValue(waitSeconds);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    RenderBindEditor("Menu key", "e.g. F7", menuBindBuffer_, [this](const std::string& value)
    {
        cvarManager->getCvar(kCvarMenuBind).setValue(value);
    });

    RenderBindEditor("Mark-match key", "e.g. F8", markBindBuffer_, [this](const std::string& value)
    {
        cvarManager->getCvar(kCvarMarkBind).setValue(value);
    });

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Open plugin window"))
        OpenMainWindow();

    ImGui::SameLine();
    if (ImGui::Button("Analyze newest replay"))
    {
        RefreshRecentReplays();
        std::vector<ReplayFileEntry> replays;
        {
            std::scoped_lock lock(stateMutex_);
            replays = recentReplays_;
        }
        if (!replays.empty())
        {
            forceAnalysisTab_ = true;
            AnalyzeReplayPath(replays.front().path, true);
        }
        else
            SetError("No replay files were found in the Rocket League replay folder.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Console: %s  |  %s  |  %s  |  %s",
        kCommandMark, kCommandAnalyzeCurrent, kCommandAnalyzeLatest, kCommandOpenReplayFolder);
}

// ---------------------------------------------------------------------------
// Main window — tab-based layout
// ---------------------------------------------------------------------------

void RLDetect::RenderWindow()
{
    ApplyStyle();

    // Apply UI zoom scale
    ImGui::SetWindowFontScale(uiScale_);

    RenderTopBar();

    // ---- Zoom toolbar (right-aligned, above tabs) ----
    {
        char zoomLabel[16];
        snprintf(zoomLabel, sizeof(zoomLabel), "%d%%", static_cast<int>(uiScale_ * 100.0f + 0.5f));

        const float iconW = ImGui::CalcTextSize("Zoom").x;
        const float btnW = ImGui::CalcTextSize("  -  ").x + 12.0f;
        const float lblW = ImGui::CalcTextSize(zoomLabel).x;
        const float totalW = iconW + btnW * 2 + lblW + 40.0f;
        const float startX = ImGui::GetWindowContentRegionMax().x - totalW;

        ImGui::SetCursorPosX(startX);

        ImGui::TextColored(ImVec4(0.52f, 0.58f, 0.68f, 1.00f), "Zoom");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.20f, 0.28f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.28f, 0.36f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.16f, 0.22f, 1.00f));

        if (ImGui::SmallButton("  -  ##zoom"))
        {
            uiScale_ = std::max(0.6f, uiScale_ - 0.1f);
            cvarManager->getCvar(kCvarUiScale).setValue(uiScale_);
        }
        ImGui::SameLine();

        ImGui::TextColored(ImVec4(0.78f, 0.82f, 0.90f, 1.00f), "%s", zoomLabel);
        ImGui::SameLine();

        if (ImGui::SmallButton("  +  ##zoom"))
        {
            uiScale_ = std::min(1.5f, uiScale_ + 0.1f);
            cvarManager->getCvar(kCvarUiScale).setValue(uiScale_);
        }

        ImGui::PopStyleColor(3);
    }

    if (ImGui::BeginTabBar("RLDetect_Tabs", ImGuiTabBarFlags_NoTooltip))
    {
        // When the window was auto-opened (F8 flow), force-select the Analysis tab
        ImGuiTabItemFlags analysisFlags = 0;
        if (forceAnalysisTab_)
        {
            analysisFlags = ImGuiTabItemFlags_SetSelected;
            forceAnalysisTab_ = false;
        }

        if (ImGui::BeginTabItem("  Analysis  ", nullptr, analysisFlags))
        {
            RenderTabAnalysis();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("  Replays  "))
        {
            RenderTabReplays();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("  Settings  "))
        {
            RenderTabSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("  Credits  "))
        {
            RenderTabCredits();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------
// Top bar — status strip
// ---------------------------------------------------------------------------

void RLDetect::RenderTopBar()
{
    AnalysisStage stage;
    std::string message;
    std::string error;
    bool marked = false;
    bool watcherRunning = false;

    {
        std::scoped_lock lock(stateMutex_);
        stage = stage_;
        message = stageMessage_;
        error = errorMessage_;
        marked = currentMatchMarked_;
        watcherRunning = replayWatcherRunning_;
    }

    // Status dot + badge
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec4 dotColor;
    const char* statusLabel = "READY";

    switch (stage)
    {
    case AnalysisStage::Idle:
        dotColor = ImVec4(0.20f, 0.72f, 0.45f, 1.00f);
        statusLabel = "READY";
        break;
    case AnalysisStage::MatchMarked:
    case AnalysisStage::WaitingForMatchEnd:
        dotColor = ImVec4(0.92f, 0.72f, 0.20f, 1.00f);
        statusLabel = "MARKED";
        break;
    case AnalysisStage::SavingReplay:
    case AnalysisStage::WaitingForReplayFile:
        dotColor = ImVec4(0.29f, 0.56f, 0.88f, 1.00f);
        statusLabel = "SAVING";
        break;
    case AnalysisStage::UploadingReplay:
    case AnalysisStage::ParsingResponse:
        dotColor = ImVec4(0.60f, 0.40f, 0.95f, 1.00f);
        statusLabel = "ANALYZING";
        break;
    case AnalysisStage::ShowingResults:
        dotColor = ImVec4(0.14f, 0.80f, 0.70f, 1.00f);
        statusLabel = "RESULTS READY";
        break;
    case AnalysisStage::Error:
        dotColor = ImVec4(0.90f, 0.25f, 0.30f, 1.00f);
        statusLabel = "ERROR";
        break;
    }

    // Draw status dot
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float radius = 5.0f;
        draw->AddCircleFilled(ImVec2(cursor.x + radius, cursor.y + ImGui::GetTextLineHeight() * 0.5f), radius, ImColorU32(dotColor), 12);
        ImGui::Dummy(ImVec2(radius * 2 + 6, 0));
        ImGui::SameLine();
    }

    RenderPillBadge(statusLabel, ImVec4(dotColor.x, dotColor.y, dotColor.z, 0.18f), dotColor);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", message.c_str());

    if (marked)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.35f, 1.00f), "[Match marked]");
    }

    if (watcherRunning)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.72f, 0.98f, 1.00f), "[Watching...]");
    }

    // Error banner
    if (!error.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.24f, 0.08f, 0.10f, 0.92f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::BeginChild("wb_error_banner", ImVec2(0, 36 * uiScale_), true);
        ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.55f, 1.00f), "Error:");
        ImGui::SameLine();
        ImGui::TextUnformatted(error.c_str());
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

// ---------------------------------------------------------------------------
// Tab: Analysis
// ---------------------------------------------------------------------------

void RLDetect::RenderTabAnalysis()
{
    // Two-column layout: actions left, results right
    const float leftWidth = std::max(230.0f, ImGui::GetContentRegionAvail().x * 0.26f);
    const float height = ImGui::GetContentRegionAvail().y - 4.0f;

    // Left: Actions
    ImGui::BeginChild("analysis_left", ImVec2(leftWidth, height), false);
    RenderActionButtons();
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Results
    ImGui::BeginChild("analysis_right", ImVec2(0, height), false);
    RenderResultsPanel();
    ImGui::EndChild();
}

void RLDetect::RenderActionButtons()
{
    RenderSectionHeader("Quick Actions");

    const ImVec2 btnSize(-1, 26 * uiScale_);

    // Primary action - Mark current
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.92f, 0.68f, 0.16f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.74f, 0.22f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.60f, 0.12f, 1.00f));
    if (ImGui::Button("Mark Current Match", btnSize))
    {
        gameWrapper->Execute([this](GameWrapper*)
        {
            Log("Mark command received.");
            if (!gameWrapper->IsInOnlineGame())
            {
                SetError("There is no active online match to mark right now.");
                return;
            }
            MarkCurrentMatchForAnalysis(true);
            SetStage(AnalysisStage::WaitingForMatchEnd, "Current match marked. Waiting for match end...", true);
        });
    }
    ImGui::PopStyleColor(3);
    RenderTooltipMarker("Press during a live match to automatically save and analyze the replay when the match ends.");

    if (ImGui::Button("Export & Analyze Current Match", btnSize))
    {
        StartQueuedSaveForCurrentMatch("Manual UI request", true);
    }
    RenderTooltipMarker("Immediately export the replay if on the post-game screen, or mark the live match for later.");

    if (ImGui::Button("Analyze Selected Replay", btnSize))
    {
        std::filesystem::path path;
        {
            std::scoped_lock lock(stateMutex_);
            if (selectedReplayIndex_ >= 0 && selectedReplayIndex_ < static_cast<int>(recentReplays_.size()))
                path = recentReplays_[selectedReplayIndex_].path;
        }
        if (path.empty())
            SetError("No replay is selected. Go to the Replays tab first.");
        else
        {
            forceAnalysisTab_ = true;
            AnalyzeReplayPath(path, true);
        }
    }

    if (ImGui::Button("Analyze Newest Replay", btnSize))
    {
        RefreshRecentReplays();
        std::vector<ReplayFileEntry> replays;
        {
            std::scoped_lock lock(stateMutex_);
            replays = recentReplays_;
        }
        if (replays.empty())
            SetError("No replay files were found.");
        else
        {
            forceAnalysisTab_ = true;
            AnalyzeReplayPath(replays.front().path, true);
        }
    }

    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.56f, 0.64f, 1.00f));
    ImGui::TextWrapped("Tip: Stay on the post-game screen until export completes.");
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Results panel  (right side of Analysis tab)
// ---------------------------------------------------------------------------

void RLDetect::RenderResultsPanel()
{
    // Check if an analysis is currently in progress — show loading overlay
    {
        AnalysisStage stage;
        {
            std::scoped_lock lock(stateMutex_);
            stage = stage_;
        }

        const bool isLoading = (stage == AnalysisStage::SavingReplay
            || stage == AnalysisStage::WaitingForReplayFile
            || stage == AnalysisStage::UploadingReplay
            || stage == AnalysisStage::ParsingResponse
            || stage == AnalysisStage::WaitingForMatchEnd);

        if (isLoading)
        {
            RenderLoadingState();
            return;
        }
    }

    std::optional<AnalysisResultSet> result;
    {
        std::scoped_lock lock(stateMutex_);
        result = currentResult_;
    }

    if (!result.has_value())
    {
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.52f, 0.56f, 0.64f, 1.00f), "No analysis results yet.");
        ImGui::TextWrapped("Run an analysis from the buttons on the left. Results will appear here.");
        return;
    }

    int actualBots = 0;
    int suspicious = 0;
    for (const auto& row : result->players)
    {
        if (row.isActualBot) ++actualBots;
        else if (row.confidencePercent >= 50) ++suspicious;
    }

    const bool hasBots = (actualBots > 0 || suspicious > 0);
    const ImVec4 green(0.22f, 0.78f, 0.48f, 1.00f);
    const ImVec4 red(0.94f, 0.28f, 0.32f, 1.00f);
    const ImVec4 verdictColor = hasBots ? red : green;

    // ---- Verdict header ----
    RenderSectionHeader("Verdict");

    RenderPillBadge(hasBots ? "BOT DETECTED" : "ALL CLEAN", ImVec4(verdictColor.x, verdictColor.y, verdictColor.z, 0.16f), verdictColor);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BuildSummaryText().c_str());

    // ---- Stat cards row ----
    {
        char buf1[32], buf2[32], buf3[32], buf4[32];
        snprintf(buf1, sizeof(buf1), "%d", static_cast<int>(result->players.size()));
        snprintf(buf2, sizeof(buf2), "%d", suspicious + actualBots);
        snprintf(buf3, sizeof(buf3), "%d ms", result->replayParseTimeMs);
        snprintf(buf4, sizeof(buf4), "%d ms", result->analysisTimeMs);

        ImGui::Columns(4, "stat_cards", false);
        RenderStatCard("Players", buf1, ImVec4(0.50f, 0.76f, 1.00f, 1.00f));
        ImGui::NextColumn();
        RenderStatCard("Bots flagged", buf2, red);
        ImGui::NextColumn();
        RenderStatCard("Parse time", buf3, ImVec4(0.95f, 0.84f, 0.49f, 1.00f));
        ImGui::NextColumn();
        RenderStatCard("Analysis", buf4, ImVec4(0.72f, 0.56f, 0.98f, 1.00f));
        ImGui::Columns(1);
    }

    // ---- Copy bots button (prominent, only when bots detected) ----
    if (hasBots)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.22f, 0.26f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.28f, 0.32f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.18f, 0.22f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);

        char copyLabel[64];
        snprintf(copyLabel, sizeof(copyLabel), "Copy %d Bot%s Info to Clipboard",
            suspicious + actualBots, (suspicious + actualBots) == 1 ? "" : "s");

        if (ImGui::Button(copyLabel, ImVec2(-1, 28 * uiScale_)))
        {
            std::ostringstream oss;
            bool first = true;
            for (const auto& row : result->players)
            {
                if (!row.isActualBot && row.confidencePercent < 50) continue;

                if (!first) oss << "\n--\n";
                first = false;

                oss << "Player name : " << GetSafePlayerName(row) << "\n";
                oss << "Platform ID (" << GetPlatformLabel(row) << ") : " << GetAccountId(row);
            }
            CopyTextToClipboard(oss.str());
            ShowToast("RLDetect", "Bot info copied to clipboard.");
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    // ---- Replay info + Watch button ----
    ImGui::TextDisabled("Replay: %s  |  %s",
        CleanDisplayText(result->replayPath.filename().string()).c_str(),
        ToDisplayTime(result->completedAt).c_str());
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.70f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.52f, 0.78f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.38f, 0.62f, 1.00f));
    if (ImGui::SmallButton("Watch Replay##results"))
    {
        gameWrapper->Execute([this, path = result->replayPath](GameWrapper*)
        {
            auto replayMgr = gameWrapper->GetReplayManagerWrapper();
            if (replayMgr)
            {
                replayMgr.PlayReplayFile(path.string());
            }
            else
            {
                SetError("Replay manager is not available right now.");
            }
        });
    }
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    // ---- Scrollable player list ----
    ImGui::BeginChild("wb_player_scroll", ImVec2(0, 0), false);

    RenderPlayersByTeam("Blue Team", 0, ImVec4(0.22f, 0.53f, 1.00f, 1.00f));
    RenderPlayersByTeam("Orange Team", 1, ImVec4(1.00f, 0.58f, 0.18f, 1.00f));
    RenderExtraInfoTable();

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Loading state — animated spinner shown while replay is being exported/uploaded
// ---------------------------------------------------------------------------

void RLDetect::RenderLoadingState()
{
    AnalysisStage stage;
    std::string message;
    {
        std::scoped_lock lock(stateMutex_);
        stage = stage_;
        message = stageMessage_;
    }

    const float time = static_cast<float>(ImGui::GetTime());
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float centerX = origin.x + avail.x * 0.5f;
    const float centerY = origin.y + avail.y * 0.38f;

    // ---- Outer pulsing ring ----
    {
        const float pulseAlpha = 0.08f + 0.06f * sinf(time * 2.0f);
        const float pulseRadius = 56.0f + 4.0f * sinf(time * 1.5f);
        draw->AddCircle(ImVec2(centerX, centerY), pulseRadius,
            ImColorU32Alpha(ImVec4(0.286f, 0.561f, 0.882f, 1.0f), pulseAlpha), 48, 2.0f);
    }

    // ---- Spinning dots ----
    {
        constexpr int numDots = 10;
        constexpr float ringRadius = 36.0f;
        const float phase = fmodf(time * 1.8f, 1.0f);
        const ImVec4 dotBaseColor(0.286f, 0.561f, 0.882f, 1.0f);

        for (int i = 0; i < numDots; ++i)
        {
            const float normalizedPos = static_cast<float>(i) / numDots;
            const float angle = normalizedPos * 2.0f * 3.14159265f - phase * 2.0f * 3.14159265f;
            const float dotX = centerX + cosf(angle) * ringRadius;
            const float dotY = centerY + sinf(angle) * ringRadius;

            // Tail fade: dot 0 is brightest, last is dimmest
            const float distFromHead = fmodf(normalizedPos - phase + 1.0f, 1.0f);
            const float alpha = 0.15f + 0.85f * (1.0f - distFromHead);
            const float dotRadius = 2.8f + 1.8f * (1.0f - distFromHead);

            draw->AddCircleFilled(ImVec2(dotX, dotY), dotRadius,
                ImColorU32Alpha(dotBaseColor, alpha), 12);
        }
    }

    // ---- Center icon: animated arc ----
    {
        const float arcStart = time * 3.5f;
        const float arcLen = 1.2f + 0.6f * sinf(time * 2.2f);
        draw->PathArcTo(ImVec2(centerX, centerY), 18.0f,
            arcStart, arcStart + arcLen, 24);
        draw->PathStroke(ImColorU32(ImVec4(0.376f, 0.635f, 0.945f, 0.90f)), false, 3.0f);
    }

    // ---- Stage label (big) ----
    {
        const char* stageLabel = "Loading...";
        switch (stage)
        {
        case AnalysisStage::WaitingForMatchEnd:
            stageLabel = "Match marked — waiting for end...";
            break;
        case AnalysisStage::SavingReplay:
            stageLabel = "Saving replay...";
            break;
        case AnalysisStage::WaitingForReplayFile:
            stageLabel = "Writing replay file...";
            break;
        case AnalysisStage::UploadingReplay:
            stageLabel = "Uploading for analysis...";
            break;
        case AnalysisStage::ParsingResponse:
            stageLabel = "Processing results...";
            break;
        default:
            break;
        }

        const ImVec2 labelSize = ImGui::CalcTextSize(stageLabel);
        const float labelX = centerX - labelSize.x * 0.5f;
        const float labelY = centerY + 62.0f;
        draw->AddText(ImVec2(labelX, labelY),
            ImColorU32(ImVec4(0.82f, 0.86f, 0.93f, 1.00f)), stageLabel);
    }

    // ---- Substage detail text ----
    if (!message.empty())
    {
        const ImVec2 msgSize = ImGui::CalcTextSize(message.c_str());
        const float maxWidth = avail.x * 0.8f;
        const float msgX = centerX - std::min(msgSize.x, maxWidth) * 0.5f;
        const float msgY = centerY + 84.0f;
        draw->AddText(ImVec2(msgX, msgY),
            ImColorU32(ImVec4(0.52f, 0.56f, 0.64f, 0.85f)), message.c_str());
    }

    // ---- Animated dots after label ("...") ----
    {
        const int dotCount = (static_cast<int>(time * 2.0f) % 4);
        std::string dots;
        for (int i = 0; i < dotCount; ++i) dots += '.';
        if (!dots.empty())
        {
            const ImVec2 dotsSize = ImGui::CalcTextSize(dots.c_str());
            const float dotsX = centerX - dotsSize.x * 0.5f;
            const float dotsY = centerY + 102.0f;
            draw->AddText(ImVec2(dotsX, dotsY),
                ImColorU32(ImVec4(0.286f, 0.561f, 0.882f, 0.60f)), dots.c_str());
        }
    }

    // Reserve space so ImGui knows the area is used
    ImGui::Dummy(avail);
}

void RLDetect::RenderPlayersByTeam(const char* title, int teamNum, const ImVec4& accent)
{
    std::optional<AnalysisResultSet> result;
    {
        std::scoped_lock lock(stateMutex_);
        result = currentResult_;
    }
    if (!result.has_value()) return;

    int teamCount = 0;
    for (const auto& row : result->players)
        if (row.teamNum == teamNum) ++teamCount;

    // Team header with colored line
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(cursor, ImVec2(cursor.x + 3.0f, cursor.y + ImGui::GetTextLineHeight()), ImColorU32(accent), 2.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
    ImGui::TextColored(accent, "%s", title);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d player%s)", teamCount, teamCount == 1 ? "" : "s");

    bool hadAny = false;
    for (const auto& row : result->players)
    {
        if (row.teamNum == teamNum)
        {
            hadAny = true;
            RenderPlayerCard(row, accent);
        }
    }

    if (!hadAny)
    {
        ImGui::TextDisabled("  No players on this team.");
    }
}

void RLDetect::RenderPlayerCard(const PlayerAnalysisRow& row, const ImVec4& accent)
{
    const ImVec4 green(0.22f, 0.78f, 0.48f, 1.00f);
    const ImVec4 red(0.94f, 0.28f, 0.32f, 1.00f);

    const float dangerRatio = static_cast<float>(std::max(0, std::min(100, row.confidencePercent))) / 100.0f;
    const ImVec4 confidenceColor = (row.isActualBot || row.confidencePercent >= 50) ? red : MixColor(green, red, dangerRatio);
    const std::string playerName = GetSafePlayerName(row);
    const std::string platformName = GetPlatformLabel(row);
    const std::string accountId = GetAccountId(row);
    const std::string inputMethod = GetInputMethodLabel(row);
    const std::string togglingLabel = GetTogglingLabel(row);
    const std::string copyPayload = platformName + ": " + accountId;

    const bool isBot = row.isActualBot || row.confidencePercent >= 50;

    ImGui::PushID(copyPayload.c_str());

    // Card background with left accent stripe
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.106f, 0.122f, 0.165f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("player_card", ImVec2(0, 86 * uiScale_), true);

    // Draw left accent stripe
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 wMin = ImGui::GetWindowPos();
        ImVec2 wMax(wMin.x + 3.0f, wMin.y + ImGui::GetWindowSize().y);
        draw->AddRectFilled(wMin, wMax, ImColorU32(accent), 3.0f);
    }

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
    ImGui::BeginGroup();

    // Header row: Name + badge
    ImGui::TextColored(accent, "%s", playerName.c_str());
    ImGui::SameLine();

    if (isBot)
        RenderPillBadge("BOT", ImVec4(red.x, red.y, red.z, 0.20f), red);
    else
        RenderPillBadge("Clean", ImVec4(green.x, green.y, green.z, 0.15f), green);

    // Confidence bar
    char confLabel[32];
    snprintf(confLabel, sizeof(confLabel), "Confidence: %d%%", row.confidencePercent);
    ImGui::TextDisabled("%s", confLabel);
    RenderCustomProgressBar(dangerRatio, confidenceColor, 3.0f);

    // Detail columns
    ImGui::Columns(4, "pdetails", false);

    ImGui::TextDisabled("Input");
    ImGui::TextUnformatted(inputMethod.c_str());
    ImGui::NextColumn();

    ImGui::TextDisabled("Toggling");
    if (row.hasTogglingValue && row.isToggling)
        ImGui::TextColored(ImVec4(0.94f, 0.55f, 0.20f, 1.00f), "%s", togglingLabel.c_str());
    else
        ImGui::TextUnformatted(togglingLabel.c_str());
    ImGui::NextColumn();

    ImGui::TextDisabled("Platform");
    ImGui::TextUnformatted(platformName.c_str());
    ImGui::NextColumn();

    ImGui::TextDisabled("Platform ID");
    if (accountId != "Unknown")
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.20f, 0.26f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.27f, 0.34f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.33f, 0.40f, 1.00f));
        if (ImGui::SmallButton("Copy"))
        {
            CopyTextToClipboard(accountId);
            ShowToast("RLDetect", "Platform ID copied.");
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::TextUnformatted(accountId.c_str());
    ImGui::Columns(1);

    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void RLDetect::RenderExtraInfoTable()
{
    std::optional<AnalysisResultSet> result;
    {
        std::scoped_lock lock(stateMutex_);
        result = currentResult_;
    }
    if (!result.has_value() || result->extraInfos.empty()) return;

    if (ImGui::CollapsingHeader("Replay Metadata"))
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.098f, 0.114f, 0.157f, 0.92f));
        ImGui::BeginChild("wb_extra_infos", ImVec2(0, 160 * uiScale_), true);
        ImGui::Columns(2, "extra_cols", true);
        ImGui::SetColumnWidth(0, 130.0f);

        for (const auto& [key, value] : result->extraInfos)
        {
            const std::string label = FriendlyLabel(key);
            const std::string displayValue = FriendlyValue(value);

            ImGui::TextColored(ImVec4(0.52f, 0.56f, 0.64f, 1.00f), "%s", label.c_str());
            ImGui::NextColumn();
            ImGui::TextWrapped("%s", displayValue.c_str());
            ImGui::NextColumn();
        }

        ImGui::Columns(1);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

// ---------------------------------------------------------------------------
// Tab: Replays
// ---------------------------------------------------------------------------

void RLDetect::RenderTabReplays()
{
    RenderRecentReplayPanel();
}

void RLDetect::RenderRecentReplayPanel()
{
    std::vector<ReplayFileEntry> replays;
    int selectedIndex = -1;
    {
        std::scoped_lock lock(stateMutex_);
        replays = recentReplays_;
        selectedIndex = selectedReplayIndex_;
    }

    RenderSectionHeader("Recent Saved Replays");

    // Toolbar
    if (ImGui::Button("Refresh"))
        RefreshRecentReplays();

    ImGui::SameLine();
    if (ImGui::Button("Open Folder"))
    {
        RefreshReplayFolderPath();
        if (!replayFolderPath_.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(replayFolderPath_, ec);
            ShellExecuteW(nullptr, L"open", replayFolderPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Analyze Selected"))
    {
        std::filesystem::path path;
        {
            std::scoped_lock lock(stateMutex_);
            if (selectedReplayIndex_ >= 0 && selectedReplayIndex_ < static_cast<int>(recentReplays_.size()))
                path = recentReplays_[selectedReplayIndex_].path;
        }
        if (path.empty())
            SetError("No replay is selected.");
        else
        {
            forceAnalysisTab_ = true;
            AnalyzeReplayPath(path, true);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Watch"))
    {
        std::filesystem::path path;
        {
            std::scoped_lock lock(stateMutex_);
            if (selectedReplayIndex_ >= 0 && selectedReplayIndex_ < static_cast<int>(recentReplays_.size()))
                path = recentReplays_[selectedReplayIndex_].path;
        }
        if (path.empty())
            SetError("No replay is selected.");
        else
        {
            gameWrapper->Execute([this, path](GameWrapper*)
            {
                auto replayMgr = gameWrapper->GetReplayManagerWrapper();
                if (replayMgr)
                {
                    replayMgr.PlayReplayFile(path.string());
                }
                else
                {
                    SetError("Replay manager is not available right now.");
                }
            });
        }
    }

    ImGui::Spacing();

    const std::string folderSummary = GetReplayFolderSummary();
    ImGui::TextDisabled("%s", folderSummary.c_str());

    ImGui::Spacing();

    // Replay list
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.082f, 0.094f, 0.129f, 0.90f));
    ImGui::BeginChild("wb_recent_list", ImVec2(0, 0), true);

    if (replays.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.52f, 0.56f, 0.64f, 1.00f), "No .replay files found.");
    }
    else
    {
        for (int i = 0; i < static_cast<int>(replays.size()); ++i)
        {
            const auto& item = replays[i];
            const bool isSelected = (selectedIndex == i);
            const std::string label = item.path.stem().string() + "##" + std::to_string(i);

            if (ImGui::Selectable(label.c_str(), isSelected, 0, ImVec2(0, 0)))
            {
                std::scoped_lock lock(stateMutex_);
                selectedReplayIndex_ = i;
            }

            // Inline metadata
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 160.0f);
            ImGui::TextDisabled("%s  |  %s",
                ToDisplayTime(item.modifiedTime).c_str(),
                FormatBytes(item.fileSize).c_str());

            if (i < static_cast<int>(replays.size()) - 1)
            {
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.16f, 0.18f, 0.24f, 0.40f));
                ImGui::Separator();
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Tab: Settings
// ---------------------------------------------------------------------------

void RLDetect::RenderTabSettings()
{
    RenderSectionHeader("General");

    bool enabled = GetBoolCvar(cvarManager, kCvarEnabled, true);
    if (ImGui::Checkbox("Enable plugin", &enabled))
        cvarManager->getCvar(kCvarEnabled).setValue(enabled ? "1" : "0");

    bool autoAnalyze = GetBoolCvar(cvarManager, kCvarAutoAnalyze, false);
    if (ImGui::Checkbox("Auto-analyze every finished match", &autoAnalyze))
        cvarManager->getCvar(kCvarAutoAnalyze).setValue(autoAnalyze ? "1" : "0");
    RenderTooltipMarker("When enabled, every completed online match will be automatically exported and analyzed.");

    bool openResults = GetBoolCvar(cvarManager, kCvarOpenResults, true);
    if (ImGui::Checkbox("Open results window automatically", &openResults))
        cvarManager->getCvar(kCvarOpenResults).setValue(openResults ? "1" : "0");

    bool showToasts = GetBoolCvar(cvarManager, kCvarShowToasts, true);
    if (ImGui::Checkbox("Show toast notifications", &showToasts))
        cvarManager->getCvar(kCvarShowToasts).setValue(showToasts ? "1" : "0");

    ImGui::Spacing();

    int waitSeconds = GetIntCvar(cvarManager, kCvarReplayPollSeconds, 35);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Replay save timeout (seconds)", &waitSeconds, 5, 120))
        cvarManager->getCvar(kCvarReplayPollSeconds).setValue(waitSeconds);
    RenderTooltipMarker("How long to wait for the replay file after match end before timing out.");

    RenderSectionHeader("Keybinds");

    RenderBindEditor("Menu key", "e.g. F7", menuBindBuffer_, [this](const std::string& value)
    {
        cvarManager->getCvar(kCvarMenuBind).setValue(value);
    });

    RenderBindEditor("Mark-match key", "e.g. F8", markBindBuffer_, [this](const std::string& value)
    {
        cvarManager->getCvar(kCvarMarkBind).setValue(value);
    });

    RenderSectionHeader("About");

    ImGui::TextColored(ImVec4(0.62f, 0.82f, 1.00f, 1.00f), "RLDetect v%s", plugin_version);
    ImGui::TextWrapped("Detect bots in Rocket League. Export replays, upload them for analysis, and view results in-game.");
    ImGui::TextDisabled("Console commands:");
    ImGui::TextDisabled("  %s", kCommandMark);
    ImGui::TextDisabled("  %s", kCommandAnalyzeCurrent);
    ImGui::TextDisabled("  %s", kCommandAnalyzeLatest);
    ImGui::TextDisabled("  %s", kCommandOpenReplayFolder);
}

// ---------------------------------------------------------------------------
// Bind editor — press-any-key system
// ---------------------------------------------------------------------------

namespace
{
    // Maps a Windows VK code to a BakkesMod/Unreal key name.
    // Returns empty string if the key shouldn't be bindable.
    std::string VkCodeToKeyName(int vk)
    {
        // Function keys
        if (vk >= VK_F1 && vk <= VK_F12)
        {
            return "F" + std::to_string(vk - VK_F1 + 1);
        }

        // Letters A-Z
        if (vk >= 'A' && vk <= 'Z')
        {
            return std::string(1, static_cast<char>(vk));
        }

        // Top-row digits
        static const char* digitNames[] = {
            "Zero", "One", "Two", "Three", "Four",
            "Five", "Six", "Seven", "Eight", "Nine"
        };
        if (vk >= '0' && vk <= '9')
        {
            return digitNames[vk - '0'];
        }

        // Numpad digits
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        {
            return "NumPad" + std::string(digitNames[vk - VK_NUMPAD0]);
        }

        // Numpad operators
        switch (vk)
        {
        case VK_MULTIPLY:  return "Multiply";
        case VK_ADD:       return "Add";
        case VK_SUBTRACT:  return "Subtract";
        case VK_DECIMAL:   return "Decimal";
        case VK_DIVIDE:    return "Divide";
        }

        // Navigation
        switch (vk)
        {
        case VK_INSERT:    return "Insert";
        case VK_DELETE:    return "Delete";
        case VK_HOME:      return "Home";
        case VK_END:       return "End";
        case VK_PRIOR:     return "PageUp";
        case VK_NEXT:      return "PageDown";
        case VK_LEFT:      return "Left";
        case VK_RIGHT:     return "Right";
        case VK_UP:        return "Up";
        case VK_DOWN:      return "Down";
        }

        // Special keys
        switch (vk)
        {
        case VK_SPACE:     return "SpaceBar";
        case VK_RETURN:    return "Enter";
        case VK_TAB:       return "Tab";
        case VK_BACK:      return "BackSpace";
        case VK_CAPITAL:   return "CapsLock";
        case VK_PAUSE:     return "Pause";
        case VK_SCROLL:    return "ScrollLock";
        }

        // OEM keys
        switch (vk)
        {
        case VK_OEM_MINUS:   return "Hyphen";
        case VK_OEM_PLUS:    return "Equals";
        case VK_OEM_4:       return "LeftBracket";
        case VK_OEM_6:       return "RightBracket";
        case VK_OEM_5:       return "Backslash";
        case VK_OEM_1:       return "Semicolon";
        case VK_OEM_7:       return "Apostrophe";
        case VK_OEM_COMMA:   return "Comma";
        case VK_OEM_PERIOD:  return "Period";
        case VK_OEM_2:       return "Slash";
        case VK_OEM_3:       return "Tilde";
        }

        return "";
    }

    // Scans all VK codes and returns the name of the first newly-pressed key, or "".
    std::string DetectKeyPress()
    {
        const ImGuiIO& io = ImGui::GetIO();
        for (int vk = 1; vk < 256; ++vk)
        {
            // Skip modifiers — we don't want to bind Shift/Ctrl/Alt alone
            if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) continue;
            if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) continue;
            if (vk == VK_LWIN || vk == VK_RWIN) continue;

            if (io.KeysDown[vk] && ImGui::IsKeyPressed(vk, false))
            {
                return VkCodeToKeyName(vk);
            }
        }
        return "";
    }
}

void RLDetect::RenderBindEditor(const char* label, const char* hint, std::string& buffer, const std::function<void(const std::string&)>& applyFn)
{
    const int bindId = (std::string(label) == "Menu key") ? 1 : 2;
    const bool isListening = (listeningBind_ == bindId);

    ImGui::TextUnformatted(label);
    ImGui::SameLine(100.0f);

    if (isListening)
    {
        // ---- Listening mode: pulsing button ----
        const float pulse = 0.5f + 0.3f * sinf(static_cast<float>(ImGui::GetTime()) * 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.92f, 0.68f, 0.16f, pulse));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.68f, 0.16f, pulse));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.92f, 0.68f, 0.16f, pulse));
        ImGui::Button(("  Press a key...  ##" + std::string(label)).c_str());
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::SmallButton(("Cancel##" + std::string(label)).c_str()))
        {
            listeningBind_ = 0;
        }

        // Detect key press
        if (const std::string keyName = DetectKeyPress(); !keyName.empty())
        {
            if (keyName == "Escape")
            {
                // Escape cancels
                listeningBind_ = 0;
            }
            else
            {
                buffer = keyName;
                applyFn(keyName);
                listeningBind_ = 0;
            }
        }
    }
    else
    {
        // ---- Normal mode: show current key + Bind/Clear buttons ----
        const std::string displayKey = buffer.empty() || buffer == "None" ? "Not set" : buffer;

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.16f, 0.22f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.16f, 0.22f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.16f, 0.22f, 0.90f));
        ImGui::Button(("  " + displayKey + "  ##display" + std::string(label)).c_str());
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.50f, 0.76f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.56f, 0.82f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.44f, 0.70f, 1.00f));
        if (ImGui::SmallButton(("Bind##" + std::string(label)).c_str()))
        {
            listeningBind_ = bindId;
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.22f, 0.28f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.28f, 0.34f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.18f, 0.24f, 1.00f));
        if (ImGui::SmallButton(("Clear##" + std::string(label)).c_str()))
        {
            buffer = "None";
            applyFn("None");
        }
        ImGui::PopStyleColor(3);
    }
}

void RLDetect::RenderTabCredits()
{
    RenderSectionHeader("Credits");

    ImGui::Spacing();
    ImGui::TextWrapped("Bot detection powered by whosbotting.com by ZealanL. RLDetect provides the in-game plugin, replay upload flow, and results interface.");
}

// ===========================================================================
// Static utility methods  (unchanged)
// ===========================================================================

std::string RLDetect::BuildSummaryText() const
{
    std::optional<AnalysisResultSet> result;
    {
        std::scoped_lock lock(stateMutex_);
        result = currentResult_;
    }

    if (!result.has_value()) return "No result available.";

    int actualBots = 0;
    int suspicious = 0;
    for (const auto& row : result->players)
    {
        if (row.isActualBot) ++actualBots;
        else if (row.confidencePercent >= 50) ++suspicious;
    }

    std::ostringstream oss;
    if (actualBots > 0)
    {
        oss << actualBots << " confirmed bot";
        if (actualBots != 1) oss << 's';
    }
    else if (suspicious > 0)
    {
        oss << suspicious << " bot";
        if (suspicious != 1) oss << 's';
        oss << " flagged";
    }
    else
    {
        oss << "No suspicious bot behavior detected";
    }

    oss << " | parse " << result->replayParseTimeMs << " ms | analysis " << result->analysisTimeMs << " ms";
    return oss.str();
}

std::string RLDetect::TrimCopy(std::string value)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string RLDetect::SanitizeFilename(std::string value)
{
    for (char& ch : value)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
            ch = '_';
        else if (std::isspace(uch))
            ch = '_';
    }

    std::string compact;
    compact.reserve(value.size());
    bool lastUnderscore = false;
    for (char ch : value)
    {
        if (ch == '_')
        {
            if (!lastUnderscore) compact.push_back(ch);
            lastUnderscore = true;
        }
        else
        {
            compact.push_back(ch);
            lastUnderscore = false;
        }
    }

    compact = TrimCopy(compact);
    while (!compact.empty() && compact.front() == '_') compact.erase(compact.begin());
    while (!compact.empty() && compact.back() == '_') compact.pop_back();

    return compact.empty() ? "Replay" : compact;
}

std::string RLDetect::ToDisplayTime(std::time_t t)
{
    if (t <= 0) return "Unknown";

    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &t);
#else
    localtime_r(&t, &localTm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string RLDetect::FormatBytes(uintmax_t bytes)
{
    static const char* units[] = { "B", "KB", "MB", "GB" };
    double value = static_cast<double>(bytes);
    size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 3)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(unitIndex == 0 ? 0 : 2) << value << ' ' << units[unitIndex];
    return oss.str();
}

long long RLDetect::ToTicks(std::filesystem::file_time_type time)
{
    return time.time_since_epoch().count();
}

std::time_t RLDetect::ToTimeT(std::filesystem::file_time_type time)
{
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(systemTime);
}

std::string RLDetect::JsonValueToDisplayString(const minijson::Value& value)
{
    if (value.isString()) return value.asString();
    if (value.isNumber())
    {
        std::ostringstream oss;
        oss << value.asNumber();
        return oss.str();
    }
    if (value.isBool()) return value.asBool() ? "true" : "false";
    if (value.isNull()) return "null";
    return minijson::DumpCompact(value);
}
