#include "ui/PauseMenu.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <utility>

namespace voxl {
namespace {

/// Dim, not opaque. The player must still recognise where they are standing -
/// half the reason to pause is to look at something - and a menu that blacks the
/// world out loses that.
constexpr ImVec4 kScrim{0.043f, 0.055f, 0.071f, 0.58f};

[[nodiscard]] bool menuButton(const char* label)
{
    return ImGui::Button(label, ImVec2{-FLT_MIN, ImGui::GetFrameHeight() * 1.3f});
}

void centreCursorX(float width)
{
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
}

}  // namespace

void PauseMenu::setWorldName(std::string name)
{
    m_worldName = std::move(name);
}

void PauseMenu::setStatusMessage(std::string text)
{
    m_status = std::move(text);
}

void PauseMenu::reset(SettingsPanel& panel)
{
    panel.close();
    m_confirmQuit = false;
    m_status.clear();
}

PauseMenuResult PauseMenu::draw(SettingsPanel& panel, Settings& settings)
{
    PauseMenuResult result;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // ---- scrim over the live frame ----
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kScrim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##voxl_pause_scrim", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // ---- the panel itself ----
    const ImVec2 centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2{0.5f, 0.5f});
    // Width in font sizes, height automatic: the button list is short and a
    // fixed height would leave a slab of empty panel under it at small scales
    // and clip it at large ones.
    //
    // min-of-max rather than std::clamp: at a large GUI scale in a small window
    // the preferred and available widths cross over, and std::clamp with
    // `low > high` is undefined behaviour.
    const float em     = ImGui::GetFontSize();
    const float width  = std::min(std::max(em * 22.0f, 300.0f),
                                  std::max(viewport->Size.x - em * 4.0f, em * 6.0f));
    ImGui::SetNextWindowSize(ImVec2{width, 0.0f}, ImGuiCond_Always);

    ImGui::Begin("##voxl_pause_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.9f);
        const char*  title = "Paused";
        const ImVec2 size  = ImGui::CalcTextSize(title);
        centreCursorX(size.x);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
    }

    if (!m_worldName.empty()) {
        const ImVec2 size = ImGui::CalcTextSize(m_worldName.c_str());
        centreCursorX(size.x);
        ImGui::TextDisabled("%s", m_worldName.c_str());
    }

    ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.7f});
    ImGui::Separator();
    ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.3f});

    if (menuButton("Resume")) {
        result.action = PauseMenuAction::Resume;
    }
    if (menuButton("Settings")) {
        panel.open(settings);
        m_confirmQuit = false;
    }

    ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.4f});

    if (menuButton("Save and quit to title")) {
        result.action = PauseMenuAction::SaveAndQuitToMenu;
    }

    if (!m_confirmQuit) {
        if (menuButton("Quit to desktop")) {
            m_confirmQuit = true;
        }
    } else {
        const float* accent = SettingsPanel::accentColour();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{accent[0], accent[1], accent[2], 0.85f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{accent[0], accent[1], accent[2], 1.0f});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.05f, 0.05f, 0.06f, 1.0f});
        if (menuButton("Really quit? Your world is saved first")) {
            result.action = PauseMenuAction::QuitToDesktop;
        }
        const bool overConfirm = ImGui::IsItemHovered();
        ImGui::PopStyleColor(3);
        // A click that lands anywhere but on the confirmation backs out of it,
        // so the armed button cannot sit there waiting to be hit by accident
        // several seconds later.
        if (!overConfirm && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_confirmQuit = false;
        }
    }

    if (!m_status.empty()) {
        ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.3f});
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_status.c_str());
    }

    ImGui::End();

    // Layered last so it covers the pause panel.
    const SettingsPanelResult panelResult = panel.draw(settings);
    result.dirty                          = panelResult.dirty;
    result.saveSettingsRequested          = panelResult.saveRequested;

    return result;
}

}  // namespace voxl
