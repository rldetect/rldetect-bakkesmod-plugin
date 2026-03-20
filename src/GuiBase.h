#pragma once

#include <functional>
#include <memory>
#include <string>

#include "bakkesmod/plugin/PluginSettingsWindow.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "imgui.h"

class CVarManagerWrapper;
extern std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

class SettingsWindowBase : public BakkesMod::Plugin::PluginSettingsWindow
{
public:
    std::string GetPluginName() override;
    void SetImGuiContext(uintptr_t ctx) override;
};

class PluginWindowBase : public BakkesMod::Plugin::PluginWindow
{
public:
    virtual ~PluginWindowBase() = default;

    bool isWindowOpen_ = false;
    bool hasImGuiContext_ = false;
    std::string menuTitle_ = "RLDetect";

    std::string GetMenuName() override;
    std::string GetMenuTitle() override;
    void SetImGuiContext(uintptr_t ctx) override;
    bool ShouldBlockInput() override;
    bool IsActiveOverlay() override;
    void OnOpen() override;
    void OnClose() override;
    void Render() override;

    virtual void RenderWindow() = 0;
};
