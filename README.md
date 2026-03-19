# RLDetect — Bot Detection for Rocket League

**Are your opponents real players or bots (cheater)?** RLDetect analyzes your match replays and tells you instantly — without ever leaving Rocket League.

**Bot detection powered by [whosbotting.com](https://whosbotting.com/) by ZealanL.**  
RLDetect provides the in-game plugin, replay upload flow, and results interface.
---

## Installation

1. Extract this archive.
2. Copy **RLDetect.dll** to:

   `%APPDATA%\bakkesmod\bakkesmod\plugins`

3. Start **BakkesMod** and launch **Rocket League**.

## Enabling the Plugin

1. Press **F2** in-game to open the BakkesMod menu.
2. Open the **Plugins** tab.
3. In **Plugin Manager**, locate **RLDetect**.
4. Enable/load the plugin.


## How It Works

1. **Press F8** during any online match to mark it for analysis
2. **Finish the match** — the plugin automatically exports and uploads the replay
3. **See results instantly** — the analysis window opens with a detailed breakdown of every player

No alt-tabbing, no websites, no manual uploads. Everything happens in-game.

---

## Features

- **One-key analysis** — Press F8 during a match, get results at the end. That's it.
- **Auto-analyze mode** — Optionally analyze every match automatically, no button press needed.
- **Live loading screen** — See the analysis progress in real time with an animated loading view.
- **Per-player breakdown** — Confidence score, input method (controller vs KBM), toggling detection, platform, and Platform ID for each player.
- **Team-sorted results** — Players grouped by Blue/Orange team with color-coded cards.
- **Replay browser** — Browse, select, and analyze any previously saved replay directly from the plugin.
- **Copy Platform IDs** — One-click copy of any player's Platform ID for reporting.
- **Replay metadata** — Map, server region, recorded FPS, score, duration, and more.
- **Toast notifications** — Non-intrusive in-game notifications keep you informed.
- **Customizable keybinds** — Change the menu key (default F7) and mark key (default F8) to anything.
- **Modern tabbed UI** — Clean dark interface with Analysis, Replays, and Settings tabs.

---

## Usage

| Action | How |
|---|---|
| Open plugin window | Press **F7** (customizable) |
| Mark current match | Press **F8** (customizable) during an online match |
| Analyze newest replay | Click the button in the Analysis tab |
| Analyze any saved replay | Select it in the Replays tab, then click Analyze |
| Enable auto-analyze | Toggle in the Settings tab |

---

## What Does the Analysis Tell You?

For each player in the match, you get:

- **Bot confidence** — A percentage score from 0% (clean) to 100% (bot). Players flagged at 50%+ are marked as BOT.
- **Input toggling** — Detects suspicious input switching patterns commonly used by bot software.
- **Input method** — Whether the player used a controller or keyboard & mouse.
- **Platform & ID** — Steam, Epic, PlayStation, Xbox, etc. with their unique ID.

---

## Console Commands

- `wb_mark_for_analysis` — Mark the current match
- `wb_analyze_current` — Export and analyze the current/post-game match
- `wb_analyze_latest` — Analyze the newest saved replay
- `wb_open_replay_folder` — Open the replay folder in Explorer

---

## Requirements

- BakkesMod installed and injected
- Internet connection (replays are uploaded to rldetect.com for analysis)
- Works with Steam and Epic Games versions of Rocket League

---

Detection by [whosbotting.com](https://whosbotting.com/) by **ZealanL**  
In-game integration and interface by [rldetect.com](https://www.rldetect.com)
