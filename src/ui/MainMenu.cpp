#include "ui/MainMenu.hpp"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <system_error>
#include <utility>

namespace voxl {
namespace {

// The backdrop's palette is the game's own sky and stone, sampled from
// render/Renderer.hpp's SkySettings defaults. A title screen that does not look
// like the world behind it reads as a launcher rather than as the game.
constexpr ImU32 kSkyTop    = IM_COL32(18, 26, 46, 255);
constexpr ImU32 kSkyBottom = IM_COL32(52, 66, 88, 255);
constexpr ImU32 kHaze      = IM_COL32(96, 116, 142, 255);
constexpr ImU32 kStoneNear = IM_COL32(15, 18, 24, 255);
constexpr ImU32 kStoneFar  = IM_COL32(28, 34, 44, 255);

/// SplitMix64. Used both to scramble a clock value into a seed and to place the
/// backdrop's skyline, so the title screen is identical on every launch (a
/// skyline that reshuffled every frame would be a distraction, and one that
/// reshuffled per run would make screenshots non-reproducible).
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept
{
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Width of the centred content column, expressed in font sizes so that it
/// tracks the GUI scale without a second scale factor to keep in step.
///
/// Written as min-of-max rather than a single std::clamp on purpose: at a large
/// GUI scale in a small window the preferred width and the available width
/// cross over, and std::clamp with `low > high` is undefined behaviour (MSVC's
/// debug runtime asserts on it). Fitting the window always wins.
[[nodiscard]] float columnWidth()
{
    const float em        = ImGui::GetFontSize();
    const float preferred = std::max(em * 24.0f, 300.0f);
    const float available = std::max(ImGui::GetMainViewport()->Size.x - em * 4.0f, em * 6.0f);
    return std::min(preferred, available);
}

/// A menu button: full column width, comfortably tall, centred text.
[[nodiscard]] bool menuButton(const char* label)
{
    const float height = ImGui::GetFrameHeight() * 1.35f;
    return ImGui::Button(label, ImVec2{-FLT_MIN, height});
}

void centreCursorX(float width)
{
    const float available = ImGui::GetWindowWidth();
    ImGui::SetCursorPosX(std::max(0.0f, (available - width) * 0.5f));
}

/// "3 minutes ago" is nicer but needs a clock the menu does not own; a date is
/// unambiguous and never wrong.
[[nodiscard]] std::string formatTimestamp(std::int64_t unixSeconds)
{
    if (unixSeconds <= 0) {
        return std::string{"never played"};
    }
    const std::time_t time = static_cast<std::time_t>(unixSeconds);
    std::tm           parts{};
#ifdef _WIN32
    if (::localtime_s(&parts, &time) != 0) {
        return std::string{"unknown"};
    }
#else
    if (::localtime_r(&time, &parts) == nullptr) {
        return std::string{"unknown"};
    }
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &parts) == 0) {
        return std::string{"unknown"};
    }
    return std::string{buffer};
}

}  // namespace

// ------------------------------------------------------------------ state --

void MainMenu::setWorldProvider(WorldListProvider provider)
{
    m_provider     = std::move(provider);
    m_worldsLoaded = false;
}

void MainMenu::setWorlds(std::vector<WorldEntry> worlds)
{
    m_worlds        = std::move(worlds);
    m_worldsLoaded  = true;
    m_selectedWorld = 0;
}

void MainMenu::refreshWorlds()
{
    if (!m_provider) {
        return;
    }
    m_worlds       = m_provider();
    m_worldsLoaded = true;
    if (m_selectedWorld >= m_worlds.size()) {
        m_selectedWorld = 0;
    }
}

void MainMenu::setVersionLabel(std::string text)
{
    m_versionLabel = std::move(text);
}

void MainMenu::setStatusMessage(std::string text)
{
    m_status = std::move(text);
}

void MainMenu::reset()
{
    m_screen = Screen::Root;
    m_status.clear();
    m_nameBuffer[0] = '\0';
    m_seedBuffer[0] = '\0';
    // Force a re-enumeration: a world may have been created or saved since the
    // last time the title screen was on display.
    m_worldsLoaded = false;
}

void MainMenu::goTo(Screen screen)
{
    m_screen = screen;
    m_status.clear();
}

// ------------------------------------------------------------------- seed --

std::uint64_t MainMenu::resolveSeed(std::string_view text)
{
    const std::string_view trimmed = trim(text);

    if (trimmed.empty()) {
        // The clock is the ONLY entropy anywhere in world creation, and it is
        // consumed exactly here: from this point on the world is a pure function
        // of the number returned.
        const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
        const std::uint64_t seed = splitmix64(static_cast<std::uint64_t>(ticks));
        return seed != 0 ? seed : 0x9E3779B97F4A7C15ull;
    }

    // A number means itself, so a seed copied from a friend reproduces exactly.
    // Signed first: players share negative seeds, and reinterpreting the two's
    // complement is what makes "-1" and "18446744073709551615" the same world.
    {
        std::int64_t                 value  = 0;
        const std::from_chars_result result = std::from_chars(
            trimmed.data(), trimmed.data() + trimmed.size(), value);
        if (result.ec == std::errc{} && result.ptr == trimmed.data() + trimmed.size()) {
            return static_cast<std::uint64_t>(value);
        }
    }
    {
        std::uint64_t                value  = 0;
        const std::from_chars_result result = std::from_chars(
            trimmed.data(), trimmed.data() + trimmed.size(), value);
        if (result.ec == std::errc{} && result.ptr == trimmed.data() + trimmed.size()) {
            return value;
        }
    }

    // FNV-1a over the bytes, then an avalanche pass. FNV alone clusters badly
    // for short similar strings ("world1"/"world2" differ in one low bit), and
    // the terrain generator hashes the seed per noise field rather than
    // whitening it first.
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (char c : trimmed) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x100000001B3ull;
    }
    const std::uint64_t seed = splitmix64(hash);
    return seed != 0 ? seed : 0x9E3779B97F4A7C15ull;
}

// -------------------------------------------------------------- backdrop --

void MainMenu::drawBackdrop() const
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList*          draw     = ImGui::GetWindowDrawList();

    const ImVec2 topLeft     = viewport->Pos;
    const ImVec2 bottomRight = ImVec2{viewport->Pos.x + viewport->Size.x,
                                      viewport->Pos.y + viewport->Size.y};
    const float  horizon     = viewport->Pos.y + viewport->Size.y * 0.62f;

    draw->AddRectFilledMultiColor(topLeft, ImVec2{bottomRight.x, horizon}, kSkyTop, kSkyTop,
                                  kSkyBottom, kSkyBottom);
    draw->AddRectFilledMultiColor(ImVec2{topLeft.x, horizon}, bottomRight, kStoneFar, kStoneFar,
                                  kStoneNear, kStoneNear);
    // Thin band of haze exactly on the horizon, which is what stops the two
    // gradients from meeting as a hard printed line.
    draw->AddRectFilledMultiColor(ImVec2{topLeft.x, horizon - viewport->Size.y * 0.06f},
                                  ImVec2{bottomRight.x, horizon}, IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 0), kHaze, kHaze);

    // A blocky skyline: columns of a fixed width with hashed heights. Cubes,
    // because this is a voxel game and a smooth curve here would be a lie about
    // what the world looks like.
    const float column = std::max(24.0f, viewport->Size.x / 64.0f);
    const int   count  = static_cast<int>(viewport->Size.x / column) + 2;
    for (int i = 0; i < count; ++i) {
        const std::uint64_t noise = splitmix64(static_cast<std::uint64_t>(i) * 0x2545F491u);
        const float         t     = static_cast<float>(noise & 0xFFFFull) / 65535.0f;
        const float         height = viewport->Size.y * (0.03f + 0.09f * t * t);
        const float         x0     = topLeft.x + static_cast<float>(i) * column;
        draw->AddRectFilled(ImVec2{x0, horizon - height},
                            ImVec2{x0 + column + 1.0f, horizon + 2.0f}, kStoneNear);
    }
}

void MainMenu::drawTitle() const
{
    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::PushFont(nullptr, style.FontSizeBase * 3.6f);
    const char*  title = "VOXL";
    const ImVec2 size  = ImGui::CalcTextSize(title);
    centreCursorX(size.x);
    // Drop shadow first: the title sits over a gradient whose brightness varies
    // with the window size, and a plain light-on-light title vanishes at some
    // of them.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2{origin.x + 3.0f, origin.y + 3.0f},
                                        IM_COL32(0, 0, 0, 160), title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();

    const float* accent = SettingsPanel::accentColour();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{accent[0], accent[1], accent[2], 0.85f});
    const char*  tagline = "a first-person voxel sandbox";
    const ImVec2 taglineSize = ImGui::CalcTextSize(tagline);
    centreCursorX(taglineSize.x);
    ImGui::TextUnformatted(tagline);
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ draw --

MainMenuResult MainMenu::draw(SettingsPanel& panel, Settings& settings)
{
    MainMenuResult result;

    if (!m_worldsLoaded) {
        refreshWorlds();
        m_worldsLoaded = true;  // even without a provider, so this runs once
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
    ImGui::Begin("##voxl_main_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawBackdrop();

    const float width = columnWidth();
    ImGui::SetCursorPosY(viewport->Size.y * 0.16f);
    drawTitle();
    ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 1.6f});

    centreCursorX(width);
    ImGui::BeginGroup();
    ImGui::PushItemWidth(width);
    // A child of a fixed width is what keeps every screen's controls on the same
    // vertical axis; without it, the Create form (which has labels) and the Root
    // screen (which does not) would sit at different left edges and the menu
    // would visibly jump between them.
    if (ImGui::BeginChild("##menu_column", ImVec2{width, 0.0f},
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        switch (m_screen) {
            case Screen::Root:   drawRoot(result, panel, settings); break;
            case Screen::Create: drawCreate(result); break;
            case Screen::Load:   drawLoad(result); break;
        }
    }
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    if (!m_status.empty()) {
        ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.5f});
        const float* accent = SettingsPanel::accentColour();
        const ImVec2 size   = ImGui::CalcTextSize(m_status.c_str());
        centreCursorX(size.x);
        ImGui::TextColored(ImVec4{accent[0], accent[1], accent[2], 1.0f}, "%s", m_status.c_str());
    }

    // Version label pinned to the bottom-left, out of the way of the column.
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::SetCursorPos(ImVec2{style.WindowPadding.x,
                                   viewport->Size.y - ImGui::GetFrameHeight()});
        ImGui::TextDisabled("%s", m_versionLabel.c_str());
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // Layered after the menu window so it draws on top of it.
    const SettingsPanelResult panelResult = panel.draw(settings);
    result.dirty |= panelResult.dirty;
    result.saveSettingsRequested = panelResult.saveRequested;

    // Escape backs out of a sub-screen, but only when the panel is not the thing
    // in front: the panel handles its own Escape and both reacting would close
    // two levels at once.
    if (!panel.isOpen() && m_screen != Screen::Root &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        goTo(Screen::Root);
    }

    return result;
}

void MainMenu::drawRoot(MainMenuResult& result, SettingsPanel& panel, const Settings& settings)
{
    if (menuButton("Create world")) {
        goTo(Screen::Create);
        m_nameBuffer[0] = '\0';
        m_seedBuffer[0] = '\0';
    }
    if (menuButton("Load world")) {
        refreshWorlds();
        goTo(Screen::Load);
    }
    if (menuButton("Settings")) {
        panel.open(settings);
    }
    ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.4f});
    if (menuButton("Quit")) {
        result.action = MainMenuAction::Quit;
    }
}

void MainMenu::drawCreate(MainMenuResult& result)
{
    ImGui::SeparatorText("New world");
    ImGui::Spacing();

    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##world_name", "New World", m_nameBuffer, sizeof(m_nameBuffer));

    ImGui::Spacing();
    ImGui::TextUnformatted("Seed");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##world_seed", "leave blank for a random world", m_seedBuffer,
                             sizeof(m_seedBuffer));

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    if (trim(m_seedBuffer).empty()) {
        ImGui::TextWrapped("A seed will be chosen for you.");
    } else {
        // Showing the resolved number matters: typing a word is allowed, and
        // without this the player has no way to write down what they got.
        ImGui::TextWrapped("Seed: %llu",
                           static_cast<unsigned long long>(resolveSeed(m_seedBuffer)));
    }
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.6f});

    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Back", ImVec2{half, ImGui::GetFrameHeight() * 1.2f})) {
        goTo(Screen::Root);
    }
    ImGui::SameLine();
    if (ImGui::Button("Create", ImVec2{half, ImGui::GetFrameHeight() * 1.2f})) {
        const std::string_view name = trim(m_nameBuffer);
        result.action    = MainMenuAction::CreateWorld;
        result.worldName = name.empty() ? std::string{"New World"} : std::string{name};
        result.seed      = resolveSeed(m_seedBuffer);
    }
}

void MainMenu::drawLoad(MainMenuResult& result)
{
    ImGui::SeparatorText("Load world");
    ImGui::Spacing();

    if (m_worlds.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("No saved worlds yet. Create one to get started.");
        ImGui::PopStyleColor();
    } else {
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
        const float listHeight =
            std::min(rowHeight * 6.0f, rowHeight * static_cast<float>(m_worlds.size()) + 4.0f);

        if (ImGui::BeginChild("##world_list", ImVec2{0.0f, listHeight},
                              ImGuiChildFlags_Borders)) {
            for (std::size_t i = 0; i < m_worlds.size(); ++i) {
                const WorldEntry& entry = m_worlds[i];
                ImGui::PushID(static_cast<int>(i));

                const bool selected = (i == m_selectedWorld);
                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2{0.0f, rowHeight})) {
                    m_selectedWorld = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        result.action = MainMenuAction::LoadWorld;
                        result.world  = entry;
                    }
                }

                // Two lines of text drawn over the selectable's rectangle, so the
                // whole row is one hit target rather than a name that is
                // clickable and a subtitle that is not.
                const ImVec2 min = ImGui::GetItemRectMin();
                ImDrawList*  draw = ImGui::GetWindowDrawList();
                const float  pad  = ImGui::GetStyle().FramePadding.x;
                draw->AddText(ImVec2{min.x + pad, min.y + pad * 0.5f},
                              ImGui::GetColorU32(ImGuiCol_Text),
                              entry.name.empty() ? "(unnamed)" : entry.name.c_str());

                char subtitle[128]{};
                std::snprintf(subtitle, sizeof(subtitle), "seed %llu  -  %s",
                              static_cast<unsigned long long>(entry.seed),
                              formatTimestamp(entry.lastPlayedUnixSeconds).c_str());
                draw->AddText(ImVec2{min.x + pad, min.y + pad * 0.5f + ImGui::GetTextLineHeight()},
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), subtitle);

                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    ImGui::Dummy(ImVec2{0.0f, ImGui::GetFontSize() * 0.4f});

    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Back", ImVec2{half, ImGui::GetFrameHeight() * 1.2f})) {
        goTo(Screen::Root);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_worlds.empty());
    if (ImGui::Button("Play", ImVec2{half, ImGui::GetFrameHeight() * 1.2f})) {
        result.action = MainMenuAction::LoadWorld;
        result.world  = m_worlds[std::min(m_selectedWorld, m_worlds.size() - 1)];
    }
    ImGui::EndDisabled();
}

}  // namespace voxl
