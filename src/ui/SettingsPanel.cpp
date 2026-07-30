#include "ui/SettingsPanel.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace voxl {
namespace {

using namespace settings_limits;

// ------------------------------------------------------------- the palette --
//
// "Stone and torchlight". The world is desaturated blue-grey rock under a cool
// sky, lit by warm block light; the UI borrows exactly those two families so a
// menu drawn over a paused frame does not look like a different application.
// Nothing here is a stock ImGui colour - the default dark theme's saturated
// blue is the single strongest tell that a panel is a debug tool.

constexpr ImVec4 kInk{0.043f, 0.055f, 0.071f, 1.00f};   ///< deepest, behind everything
constexpr ImVec4 kPanel{0.086f, 0.102f, 0.129f, 0.98f}; ///< window body
constexpr ImVec4 kPanelRaised{0.118f, 0.141f, 0.176f, 1.00f};
constexpr ImVec4 kFrame{0.145f, 0.173f, 0.212f, 1.00f};
constexpr ImVec4 kFrameHovered{0.184f, 0.220f, 0.267f, 1.00f};
constexpr ImVec4 kFrameActive{0.220f, 0.263f, 0.318f, 1.00f};
constexpr ImVec4 kLine{0.239f, 0.286f, 0.341f, 1.00f};

constexpr ImVec4 kText{0.855f, 0.882f, 0.918f, 1.00f};
constexpr ImVec4 kTextDim{0.455f, 0.502f, 0.561f, 1.00f};

/// Torchlight amber. Used sparingly - selection, the active tab, sliders - so
/// that when it appears the eye already knows it means "this one".
constexpr ImVec4 kAccent{0.918f, 0.639f, 0.290f, 1.00f};
constexpr ImVec4 kAccentBright{0.976f, 0.729f, 0.396f, 1.00f};
constexpr ImVec4 kAccentDeep{0.788f, 0.522f, 0.208f, 1.00f};

constexpr float kAccentRgba[4]{kAccent.x, kAccent.y, kAccent.z, kAccent.w};

[[nodiscard]] constexpr ImVec4 withAlpha(const ImVec4& colour, float alpha) noexcept
{
    return ImVec4{colour.x, colour.y, colour.z, alpha};
}

// ------------------------------------------------------------ grid helpers --
//
// Every settings row is `label | control`, with the control stretching to the
// panel's right edge. A two-column table rather than SameLine arithmetic: the
// labels then align regardless of their length AND regardless of the GUI scale,
// which a hand-tuned column offset would not survive.

[[nodiscard]] bool beginGrid(const char* id)
{
    if (!ImGui::BeginTable(id, 2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX |
                               ImGuiTableFlags_NoClip)) {
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.44f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.56f);
    return true;
}

/// Opens a row and leaves the cursor in the value cell with the item width
/// already set to "fill". `help` becomes a hover tooltip; pass nullptr for none.
void gridRow(const char* label, const char* help)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", help);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

/// A slider over a gain in [0, 1] presented as a percentage.
///
/// ImGui's printf format is applied to the raw value, so a 0..1 float with
/// "%.0f%%" would render 0.8 as "1%". Editing an integer percentage and
/// converting back is the only way to get a readable control, and the
/// conversion is exact for every value the slider can produce.
[[nodiscard]] bool volumeSlider(const char* id, float& value)
{
    int percent = static_cast<int>(std::lround(value * 100.0f));
    if (!ImGui::SliderInt(id, &percent, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        return false;
    }
    value = static_cast<float>(percent) / 100.0f;
    return true;
}

/// A muted caption under a control. Used for the two settings whose effect is
/// genuinely not guessable from the label.
void caption(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/// Full-viewport scrim. Drawn as a window rather than into the background draw
/// list because it must sit ON TOP of the menu behind it (the background list
/// is behind every window) and must swallow the clicks that miss the panel.
void drawDimmer(float alpha)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, withAlpha(kInk, alpha));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##voxl_scrim", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

}  // namespace

// ------------------------------------------------------------------ theme --

void SettingsPanel::applyTheme(float guiScale)
{
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    const float scale = std::clamp(guiScale, kGuiScaleMin, kGuiScaleMax);

    // From a FRESH style, never from the live one: ScaleAllSizes multiplies in
    // place, so restyling on top of an already-scaled style compounds and the
    // panel grows a little every time the GUI-scale slider is touched.
    ImGuiStyle style{};

    style.WindowPadding     = ImVec2{20.0f, 18.0f};
    style.WindowRounding    = 10.0f;
    style.WindowBorderSize  = 1.0f;
    style.WindowTitleAlign  = ImVec2{0.5f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;  // no collapse arrow in a game menu
    style.ChildRounding     = 8.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupRounding     = 8.0f;
    style.PopupBorderSize   = 1.0f;
    style.FramePadding      = ImVec2{12.0f, 7.0f};
    style.FrameRounding     = 6.0f;
    style.FrameBorderSize   = 0.0f;
    style.ItemSpacing       = ImVec2{12.0f, 9.0f};
    style.ItemInnerSpacing  = ImVec2{9.0f, 7.0f};
    style.CellPadding       = ImVec2{8.0f, 5.0f};
    style.IndentSpacing     = 22.0f;
    style.ScrollbarSize     = 13.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabMinSize       = 13.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 7.0f;
    style.TabBarBorderSize  = 2.0f;
    style.TabBarOverlineSize = 2.0f;
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding    = ImVec2{18.0f, 6.0f};
    style.ButtonTextAlign         = ImVec2{0.5f, 0.5f};
    style.SelectableTextAlign     = ImVec2{0.0f, 0.5f};
    style.DisabledAlpha           = 0.42f;

    ImVec4* colours = style.Colors;
    colours[ImGuiCol_Text]                  = kText;
    colours[ImGuiCol_TextDisabled]          = kTextDim;
    colours[ImGuiCol_WindowBg]              = kPanel;
    colours[ImGuiCol_ChildBg]               = withAlpha(kInk, 0.35f);
    colours[ImGuiCol_PopupBg]               = withAlpha(kPanelRaised, 0.98f);
    colours[ImGuiCol_Border]                = withAlpha(kLine, 0.65f);
    colours[ImGuiCol_BorderShadow]          = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
    colours[ImGuiCol_FrameBg]               = kFrame;
    colours[ImGuiCol_FrameBgHovered]        = kFrameHovered;
    colours[ImGuiCol_FrameBgActive]         = kFrameActive;
    colours[ImGuiCol_TitleBg]               = kInk;
    colours[ImGuiCol_TitleBgActive]         = kPanelRaised;
    colours[ImGuiCol_TitleBgCollapsed]      = kInk;
    colours[ImGuiCol_MenuBarBg]             = kPanelRaised;
    colours[ImGuiCol_ScrollbarBg]           = withAlpha(kInk, 0.45f);
    colours[ImGuiCol_ScrollbarGrab]         = kFrameHovered;
    colours[ImGuiCol_ScrollbarGrabHovered]  = kFrameActive;
    colours[ImGuiCol_ScrollbarGrabActive]   = kAccentDeep;
    colours[ImGuiCol_CheckMark]             = kAccentBright;
    colours[ImGuiCol_CheckboxSelectedBg]    = withAlpha(kAccentDeep, 0.30f);
    colours[ImGuiCol_SliderGrab]            = kAccentDeep;
    colours[ImGuiCol_SliderGrabActive]      = kAccentBright;
    colours[ImGuiCol_Button]                = kFrame;
    colours[ImGuiCol_ButtonHovered]         = kFrameActive;
    colours[ImGuiCol_ButtonActive]          = kAccentDeep;
    colours[ImGuiCol_Header]                = withAlpha(kAccentDeep, 0.28f);
    colours[ImGuiCol_HeaderHovered]         = withAlpha(kAccent, 0.36f);
    colours[ImGuiCol_HeaderActive]          = withAlpha(kAccent, 0.52f);
    colours[ImGuiCol_Separator]             = withAlpha(kLine, 0.55f);
    colours[ImGuiCol_SeparatorHovered]      = kAccentDeep;
    colours[ImGuiCol_SeparatorActive]       = kAccent;
    colours[ImGuiCol_ResizeGrip]            = withAlpha(kLine, 0.40f);
    colours[ImGuiCol_ResizeGripHovered]     = withAlpha(kAccent, 0.55f);
    colours[ImGuiCol_ResizeGripActive]      = kAccent;
    colours[ImGuiCol_InputTextCursor]       = kAccentBright;
    colours[ImGuiCol_Tab]                   = withAlpha(kInk, 0.85f);
    colours[ImGuiCol_TabHovered]            = withAlpha(kAccent, 0.30f);
    colours[ImGuiCol_TabSelected]           = kPanelRaised;
    colours[ImGuiCol_TabSelectedOverline]   = kAccent;
    colours[ImGuiCol_TabDimmed]             = withAlpha(kInk, 0.75f);
    colours[ImGuiCol_TabDimmedSelected]     = kFrame;
    colours[ImGuiCol_TabDimmedSelectedOverline] = kAccentDeep;
    colours[ImGuiCol_PlotLines]             = kAccent;
    colours[ImGuiCol_PlotLinesHovered]      = kAccentBright;
    colours[ImGuiCol_PlotHistogram]         = kAccentDeep;
    colours[ImGuiCol_PlotHistogramHovered]  = kAccentBright;
    colours[ImGuiCol_TableHeaderBg]         = kPanelRaised;
    colours[ImGuiCol_TableBorderStrong]     = withAlpha(kLine, 0.80f);
    colours[ImGuiCol_TableBorderLight]      = withAlpha(kLine, 0.35f);
    colours[ImGuiCol_TableRowBg]            = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
    colours[ImGuiCol_TableRowBgAlt]         = withAlpha(kInk, 0.25f);
    colours[ImGuiCol_TextLink]              = kAccentBright;
    colours[ImGuiCol_TextSelectedBg]        = withAlpha(kAccent, 0.35f);
    colours[ImGuiCol_TreeLines]             = withAlpha(kLine, 0.60f);
    colours[ImGuiCol_DragDropTarget]        = kAccent;
    colours[ImGuiCol_NavCursor]             = kAccent;
    colours[ImGuiCol_NavWindowingHighlight] = withAlpha(kAccent, 0.70f);
    colours[ImGuiCol_NavWindowingDimBg]     = withAlpha(kInk, 0.55f);
    colours[ImGuiCol_ModalWindowDimBg]      = withAlpha(kInk, 0.65f);

    style.ScaleAllSizes(scale);
    // Sizes and the font scale separately: ScaleAllSizes deliberately leaves
    // fonts alone so an application can scale one without the other.
    style.FontScaleMain = scale;

    ImGui::GetStyle() = style;
}

const float* SettingsPanel::accentColour() noexcept
{
    return kAccentRgba;
}

// ------------------------------------------------------------- open/close --

void SettingsPanel::open(const Settings& current)
{
    m_snapshot = current;
    m_open     = true;
}

void SettingsPanel::close() noexcept
{
    m_open = false;
}

// -------------------------------------------------------------- the tabs --

SettingsDirty SettingsPanel::drawVideoTab(Settings& settings)
{
    SettingsDirty dirty = SettingsDirty::None;
    if (!beginGrid("video_grid")) {
        return dirty;
    }

    gridRow("Render distance",
            "Chunks streamed around you. The single biggest lever on both memory "
            "and frame time.");
    if (ImGui::SliderInt("##render_distance", &settings.renderDistance, kRenderDistanceMin,
                         kRenderDistanceMax, "%d chunks", ImGuiSliderFlags_AlwaysClamp)) {
        // The far clip plane and the fog band are both derived from the view
        // distance, so all three move together or the world ends in a hard edge.
        dirty |= SettingsDirty::Streaming | SettingsDirty::Fog | SettingsDirty::Camera;
    }

    gridRow("Field of view", nullptr);
    if (ImGui::SliderFloat("##fov", &settings.fovDegrees, kFovMin, kFovMax, "%.0f\xc2\xb0",
                           ImGuiSliderFlags_AlwaysClamp)) {
        dirty |= SettingsDirty::Camera;
    }

    gridRow("Fog distance", "Scales where the distance fog starts and ends.");
    if (ImGui::SliderFloat("##fog_scale", &settings.fogDistanceScale, kFogDistanceScaleMin,
                           kFogDistanceScaleMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
        dirty |= SettingsDirty::Fog;
    }

    gridRow("Anisotropic filtering",
            "Sharpens ground textures seen at a grazing angle. The driver may cap "
            "this below what you ask for.");
    if (ImGui::SliderFloat("##anisotropy", &settings.anisotropy, kAnisotropyMin, kAnisotropyMax,
                           "%.0fx", ImGuiSliderFlags_AlwaysClamp)) {
        dirty |= SettingsDirty::Texture;
    }

    gridRow("Interface scale", nullptr);
    if (ImGui::SliderFloat("##gui_scale", &settings.guiScale, kGuiScaleMin, kGuiScaleMax, "%.2fx",
                           ImGuiSliderFlags_AlwaysClamp)) {
        dirty |= SettingsDirty::Interface;
    }

    gridRow("Vertical sync", nullptr);
    if (ImGui::Checkbox("##vsync", &settings.vsync)) {
        dirty |= SettingsDirty::Video;
    }

    gridRow("Limit frame rate",
            "Vertical sync is only a request - many laptop drivers ignore it. This "
            "cap is enforced by the engine.");
    bool capped = settings.fpsCap != kFpsCapUnlimited;
    if (ImGui::Checkbox("##fps_cap_enable", &capped)) {
        settings.fpsCap = capped ? 60 : kFpsCapUnlimited;
        dirty |= SettingsDirty::Video;
    }

    if (capped) {
        gridRow("Frame rate cap", nullptr);
        if (ImGui::SliderInt("##fps_cap", &settings.fpsCap, kFpsCapMin, kFpsCapMax, "%d fps",
                             ImGuiSliderFlags_AlwaysClamp)) {
            dirty |= SettingsDirty::Video;
        }
    }

    ImGui::EndTable();

    if (settings.vsync && settings.fpsCap == kFpsCapUnlimited) {
        ImGui::Spacing();
        caption(
            "With no cap the engine trusts the driver's swap interval. On hybrid-graphics "
            "laptops that request is often ignored, and the GPU will render thousands of "
            "frames a second that you never see.");
    }
    return dirty;
}

SettingsDirty SettingsPanel::drawControlsTab(Settings& settings)
{
    SettingsDirty dirty = SettingsDirty::None;
    if (!beginGrid("controls_grid")) {
        return dirty;
    }

    gridRow("Mouse sensitivity", "Degrees of rotation per pixel of mouse movement.");
    if (ImGui::SliderFloat("##sensitivity", &settings.mouseSensitivity, kMouseSensitivityMin,
                           kMouseSensitivityMax, "%.3f",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        dirty |= SettingsDirty::Controls;
    }

    gridRow("Invert vertical aim", nullptr);
    if (ImGui::Checkbox("##invert_y", &settings.invertMouseY)) {
        dirty |= SettingsDirty::Controls;
    }

    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::SeparatorText("Keys");
    // Not rebindable yet, and saying so is better than an empty tab: a player
    // who cannot find the rebinding UI will look for it twice.
    caption(
        "WASD move  -  Space jump  -  Shift crouch  -  Ctrl sprint  -  F fly  -  "
        "1-9 hotbar  -  F1 hide HUD  -  F3 debug overlay  -  Esc pause.\n"
        "Rebinding is not implemented.");
    return dirty;
}

SettingsDirty SettingsPanel::drawAudioTab(Settings& settings)
{
    SettingsDirty dirty = SettingsDirty::None;
    if (!beginGrid("audio_grid")) {
        return dirty;
    }

    gridRow("Master", "Multiplies both of the volumes below.");
    if (volumeSlider("##vol_master", settings.masterVolume)) {
        dirty |= SettingsDirty::Audio;
    }

    gridRow("World", "Footsteps, blocks, ambience.");
    if (volumeSlider("##vol_world", settings.worldVolume)) {
        dirty |= SettingsDirty::Audio;
    }

    gridRow("Interface", "Menu and HUD sounds.");
    if (volumeSlider("##vol_ui", settings.uiVolume)) {
        dirty |= SettingsDirty::Audio;
    }

    ImGui::EndTable();
    return dirty;
}

SettingsDirty SettingsPanel::drawWorldTab(Settings& settings)
{
    SettingsDirty dirty = SettingsDirty::None;
    if (!beginGrid("world_grid")) {
        return dirty;
    }

    gridRow("Day length", "Real minutes for one full day and night.");
    if (ImGui::SliderFloat("##day_length", &settings.dayLengthMinutes, kDayLengthMinutesMin,
                           kDayLengthMinutesMax, "%.1f min",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        dirty |= SettingsDirty::World;
    }

    gridRow("Level of detail", "Off draws every chunk at full resolution. Expensive.");
    if (ImGui::Checkbox("##lod_enabled", &settings.lodEnabled)) {
        dirty |= SettingsDirty::Streaming;
    }

    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::SeparatorText("Detail bands");

    ImGui::BeginDisabled(!settings.lodEnabled);
    if (beginGrid("lod_grid")) {
        static_assert(kLodMax == 3, "the three band rows below are written out by hand");
        constexpr const char* kBandLabels[kLodMax]{"Half detail beyond", "Quarter detail beyond",
                                                   "Eighth detail beyond"};
        for (std::size_t i = 0; i < std::size_t{kLodMax}; ++i) {
            gridRow(kBandLabels[i], nullptr);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SliderInt("##band", &settings.lodBandStart[i], 2, kLodBandMax, "%d chunks",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                // Bands must stay ascending and each at least a hysteresis band
                // wide, or a chunk demoted into a too-narrow band can never be
                // promoted back (see world/Lod.hpp). Repairing here means the
                // slider visibly pushes its neighbours instead of letting the
                // player build a configuration the engine would silently fix.
                settings.clampToValidRange();
                dirty |= SettingsDirty::Streaming;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    return dirty;
}

// -------------------------------------------------------------------- draw --

SettingsPanelResult SettingsPanel::draw(Settings& settings)
{
    SettingsPanelResult result;
    if (!m_open) {
        return result;
    }

    drawDimmer(0.62f);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2         centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                                viewport->Pos.y + viewport->Size.y * 0.5f};

    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2{0.5f, 0.5f});
    // A fraction of the viewport with a floor, so it stays usable on a 1280x720
    // window and on a 4K one - but the floor must never win against the window
    // itself, or a settings panel opened in a small window would hang off both
    // edges with its footer buttons unreachable.
    const ImVec2 size{std::min(std::max(560.0f, viewport->Size.x * 0.46f), viewport->Size.x),
                      std::min(std::max(430.0f, viewport->Size.y * 0.70f), viewport->Size.y)};
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    bool stayOpen = true;
    ImGui::Begin("Settings", &stayOpen,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    // Footer height reserved up front so the tab body scrolls instead of pushing
    // the buttons off the bottom at a large GUI scale.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float       footer =
        ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y + style.WindowPadding.y;

    if (ImGui::BeginChild("##settings_body", ImVec2{0.0f, -footer}, ImGuiChildFlags_None)) {
        if (ImGui::BeginTabBar("##settings_tabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Video")) {
                ImGui::Spacing();
                result.dirty |= drawVideoTab(settings);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls")) {
                ImGui::Spacing();
                result.dirty |= drawControlsTab(settings);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio")) {
                ImGui::Spacing();
                result.dirty |= drawAudioTab(settings);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World")) {
                ImGui::Spacing();
                result.dirty |= drawWorldTab(settings);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    bool done      = false;
    bool cancelled = false;

    if (ImGui::Button("Reset to defaults")) {
        settings     = Settings{};
        result.dirty = SettingsDirty::All;
    }

    // Right-aligned pair. Measured rather than guessed so the buttons stay
    // flush to the edge at any GUI scale.
    const float cancelWidth = ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
    const float doneWidth   = ImGui::CalcTextSize("Done").x + style.FramePadding.x * 2.0f;
    const float pairWidth   = cancelWidth + doneWidth + style.ItemSpacing.x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - pairWidth + ImGui::GetCursorPosX());

    if (ImGui::Button("Cancel", ImVec2{cancelWidth, 0.0f})) {
        cancelled = true;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDeep);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentBright);
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    if (ImGui::Button("Done", ImVec2{doneWidth, 0.0f})) {
        done = true;
    }
    ImGui::PopStyleColor(4);

    ImGui::End();

    // Escape ACCEPTS rather than cancels. Every control here is live-applied, so
    // the player has already seen the result of every change; making the most
    // reflexive key in the game silently undo all of it would be a trap. Cancel
    // is an explicit button.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        done = true;
    }
    if (!stayOpen) {  // the title bar's close button
        done = true;
    }

    if (cancelled) {
        settings          = m_snapshot;
        result.dirty      = SettingsDirty::All;
        result.cancelled  = true;
        result.closed     = true;
        m_open            = false;
    } else if (done) {
        // Clamp before handing the values back: the sliders cannot produce an
        // out-of-range value, but "Reset to defaults" followed by a hand-edited
        // settings file loaded underneath us could.
        settings.clampToValidRange();
        result.saveRequested = true;
        result.closed        = true;
        m_open               = false;
    }

    return result;
}

}  // namespace voxl
