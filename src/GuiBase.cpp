#include "GuiBase.h"

#include "bakkesmod/wrappers/cvarmanagerwrapper.h"

std::string SettingsWindowBase::GetPluginName()
{
    return "RLDetect";
}

void SettingsWindowBase::SetImGuiContext(uintptr_t ctx)
{
    if (ctx != 0)
    {
        ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
    }
}

std::string PluginWindowBase::GetMenuName()
{
    return "rldetect";
}

std::string PluginWindowBase::GetMenuTitle()
{
    return menuTitle_;
}

void PluginWindowBase::SetImGuiContext(uintptr_t ctx)
{
    hasImGuiContext_ = (ctx != 0);
    if (hasImGuiContext_)
    {
        ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
    }
}

bool PluginWindowBase::ShouldBlockInput()
{
    if (!hasImGuiContext_ || !isWindowOpen_)
    {
        return false;
    }

    return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

bool PluginWindowBase::IsActiveOverlay()
{
    return isWindowOpen_;
}

void PluginWindowBase::OnOpen()
{
    isWindowOpen_ = true;
}

void PluginWindowBase::OnClose()
{
    isWindowOpen_ = false;
}

// ---------------------------------------------------------------------------
// Scoped style — fully push/pop safe for BakkesMod shared ImGui context.
// ---------------------------------------------------------------------------

static void PushModernStyle(int& outColorCount, int& outVarCount)
{
    outColorCount = 0;
    outVarCount   = 0;

    auto pushCol = [&](ImGuiCol idx, const ImVec4& col)
    {
        ImGui::PushStyleColor(idx, col);
        ++outColorCount;
    };

    auto pushVar = [&](ImGuiStyleVar idx, float val)
    {
        ImGui::PushStyleVar(idx, val);
        ++outVarCount;
    };

    auto pushVarVec = [&](ImGuiStyleVar idx, const ImVec2& val)
    {
        ImGui::PushStyleVar(idx, val);
        ++outVarCount;
    };

    // ---- Palette ----
    const ImVec4 bg          (0.075f, 0.082f, 0.114f, 0.97f);
    const ImVec4 childBg     (0.094f, 0.106f, 0.145f, 0.95f);
    const ImVec4 frameBg     (0.130f, 0.145f, 0.196f, 1.00f);
    const ImVec4 frameHov    (0.175f, 0.196f, 0.256f, 1.00f);
    const ImVec4 frameAct    (0.216f, 0.235f, 0.306f, 1.00f);
    const ImVec4 accent      (0.286f, 0.561f, 0.882f, 1.00f);
    const ImVec4 accentHov   (0.376f, 0.635f, 0.945f, 1.00f);
    const ImVec4 accentAct   (0.216f, 0.486f, 0.816f, 1.00f);
    const ImVec4 text        (0.918f, 0.933f, 0.965f, 1.00f);
    const ImVec4 textDim     (0.525f, 0.561f, 0.635f, 1.00f);
    const ImVec4 border      (0.176f, 0.196f, 0.255f, 0.60f);
    const ImVec4 headerBg    (0.141f, 0.161f, 0.216f, 1.00f);
    const ImVec4 headerHov   (0.196f, 0.220f, 0.290f, 1.00f);
    const ImVec4 headerAct   (0.255f, 0.286f, 0.365f, 1.00f);
    const ImVec4 scrollBg    (0.055f, 0.060f, 0.086f, 0.55f);
    const ImVec4 scrollGrab  (0.235f, 0.265f, 0.335f, 0.78f);
    const ImVec4 scrollGrabH (0.316f, 0.355f, 0.435f, 0.88f);
    const ImVec4 tabBg       (0.106f, 0.122f, 0.165f, 1.00f);
    const ImVec4 tabActive   (0.176f, 0.286f, 0.496f, 1.00f);
    const ImVec4 separator   (0.196f, 0.220f, 0.286f, 0.46f);
    const ImVec4 checkMark   (0.376f, 0.745f, 0.498f, 1.00f);

    // ---- Colors ----
    pushCol(ImGuiCol_Text,                 text);
    pushCol(ImGuiCol_TextDisabled,         textDim);
    pushCol(ImGuiCol_WindowBg,             bg);
    pushCol(ImGuiCol_ChildBg,              childBg);
    pushCol(ImGuiCol_PopupBg,              ImVec4(0.086f, 0.098f, 0.137f, 0.97f));
    pushCol(ImGuiCol_Border,               border);
    pushCol(ImGuiCol_BorderShadow,         ImVec4(0, 0, 0, 0));
    pushCol(ImGuiCol_FrameBg,              frameBg);
    pushCol(ImGuiCol_FrameBgHovered,       frameHov);
    pushCol(ImGuiCol_FrameBgActive,        frameAct);
    pushCol(ImGuiCol_TitleBg,              ImVec4(0.063f, 0.070f, 0.098f, 1.00f));
    pushCol(ImGuiCol_TitleBgActive,        ImVec4(0.118f, 0.137f, 0.188f, 1.00f));
    pushCol(ImGuiCol_TitleBgCollapsed,     ImVec4(0.063f, 0.070f, 0.098f, 0.78f));
    pushCol(ImGuiCol_MenuBarBg,            ImVec4(0.086f, 0.098f, 0.133f, 1.00f));
    pushCol(ImGuiCol_ScrollbarBg,          scrollBg);
    pushCol(ImGuiCol_ScrollbarGrab,        scrollGrab);
    pushCol(ImGuiCol_ScrollbarGrabHovered, scrollGrabH);
    pushCol(ImGuiCol_ScrollbarGrabActive,  accentAct);
    pushCol(ImGuiCol_CheckMark,            checkMark);
    pushCol(ImGuiCol_SliderGrab,           accent);
    pushCol(ImGuiCol_SliderGrabActive,     accentHov);
    pushCol(ImGuiCol_Button,               accent);
    pushCol(ImGuiCol_ButtonHovered,        accentHov);
    pushCol(ImGuiCol_ButtonActive,         accentAct);
    pushCol(ImGuiCol_Header,               headerBg);
    pushCol(ImGuiCol_HeaderHovered,        headerHov);
    pushCol(ImGuiCol_HeaderActive,         headerAct);
    pushCol(ImGuiCol_Separator,            separator);
    pushCol(ImGuiCol_SeparatorHovered,     accentHov);
    pushCol(ImGuiCol_SeparatorActive,      accentAct);
    pushCol(ImGuiCol_ResizeGrip,           ImVec4(accent.x, accent.y, accent.z, 0.18f));
    pushCol(ImGuiCol_ResizeGripHovered,    ImVec4(accent.x, accent.y, accent.z, 0.55f));
    pushCol(ImGuiCol_ResizeGripActive,     ImVec4(accent.x, accent.y, accent.z, 0.86f));
    pushCol(ImGuiCol_Tab,                  tabBg);
    pushCol(ImGuiCol_TabHovered,           accentHov);
    pushCol(ImGuiCol_TabActive,            tabActive);
    pushCol(ImGuiCol_TabUnfocused,         tabBg);
    pushCol(ImGuiCol_TabUnfocusedActive,   ImVec4(0.141f, 0.227f, 0.396f, 1.00f));
    pushCol(ImGuiCol_PlotHistogram,        accent);
    pushCol(ImGuiCol_PlotHistogramHovered, accentHov);
    pushCol(ImGuiCol_PlotLines,            accent);
    pushCol(ImGuiCol_PlotLinesHovered,     accentHov);
    pushCol(ImGuiCol_TextSelectedBg,       ImVec4(accent.x, accent.y, accent.z, 0.32f));

    // ---- Style vars ----
    pushVar(ImGuiStyleVar_WindowRounding,        6.0f);
    pushVar(ImGuiStyleVar_ChildRounding,         5.0f);
    pushVar(ImGuiStyleVar_FrameRounding,         4.0f);
    pushVar(ImGuiStyleVar_PopupRounding,         4.0f);
    pushVar(ImGuiStyleVar_ScrollbarRounding,     6.0f);
    pushVar(ImGuiStyleVar_GrabRounding,          3.0f);
    pushVar(ImGuiStyleVar_TabRounding,           4.0f);
    pushVarVec(ImGuiStyleVar_WindowPadding,      ImVec2(10, 8));
    pushVarVec(ImGuiStyleVar_FramePadding,       ImVec2(8, 4));
    pushVarVec(ImGuiStyleVar_ItemSpacing,        ImVec2(8, 4));
    pushVarVec(ImGuiStyleVar_ItemInnerSpacing,   ImVec2(5, 3));
    pushVar(ImGuiStyleVar_ScrollbarSize,         11.0f);
    pushVar(ImGuiStyleVar_GrabMinSize,           10.0f);
    pushVar(ImGuiStyleVar_WindowBorderSize,      1.0f);
    pushVar(ImGuiStyleVar_ChildBorderSize,       1.0f);
}

static void PopModernStyle(int colorCount, int varCount)
{
    ImGui::PopStyleColor(colorCount);
    ImGui::PopStyleVar(varCount);
}

void PluginWindowBase::Render()
{
    if (!hasImGuiContext_ || !isWindowOpen_)
    {
        return;
    }

    int colCount = 0;
    int varCount = 0;
    PushModernStyle(colCount, varCount);

    ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(680, 420), ImVec2(1600, 1100));

    if (!ImGui::Begin(menuTitle_.c_str(), &isWindowOpen_, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        PopModernStyle(colCount, varCount);
        return;
    }

    RenderWindow();
    ImGui::End();

    PopModernStyle(colCount, varCount);
}
