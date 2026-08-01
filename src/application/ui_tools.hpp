#pragma once

// ---------------------------------------------------------------------------------------------
// The player-facing side of the editing system.
// ---------------------------------------------------------------------------------------------
// Three things live here: which tool is selected, how big it is, and the heads-up display that
// shows both. The brushes themselves are GPU code (voxels/brushes.glsl) and their contract --
// the ids, the radius bounds, which tools are one-shot, what the secondary button does -- is
// voxels/brushes.inl. THIS FILE OWNS NONE OF THAT. It picks a number out of that enumeration and
// a radius inside those bounds, puts both in GpuInput::brush_selection, and draws them.
//
// Everything that could disagree with the shader is therefore read from brushes.inl rather than
// restated: names come from BRUSH_ID_NAMES, the belt order is the id order, the radius clamp is
// BRUSH_RADIUS_MIN/MAX applied with the same multiplicative BRUSH_RADIUS_STEP the shader uses,
// and the right-click label is resolved through BRUSH_SECONDARY_ID. Adding a brush over there
// puts it on the belt over here with no edit but a badge and a colour.
//
// WHY IT IS NOT PART OF ui.cpp. ui.cpp is the *menu*: pause screen, settings tree, F3 debug
// overlay -- things you look at when you have stopped playing. The tool HUD is the opposite: it
// is only up while you are playing, and it is the one piece of UI a player reads every few
// seconds.
//
// WHY THE HUD DRAWS ITSELF FROM A HOOK. ImGui draw calls are only legal between NewFrame() and
// Render(), and in this engine both of those are inside AppUi::update() (application/ui.cpp),
// which this workstream does not own. install_hud() registers an ImGuiContextHookType_NewFramePost
// callback instead, so ImGui calls us at the right moment. See the comment at the hook in
// ui_tools.cpp.

#include <application/input.inl>

#include <cstddef>
#include <string_view>

struct ImFont;
struct ImGuiContext;
struct ImGuiContextHook;

/// How one tool is *drawn*. The tool's behaviour is entirely in brushes.inl; this is the part
/// that only a HUD cares about, indexed by BRUSH_ID_*.
struct ToolStyle {
    std::string_view badge; ///< 1-2 chars in the belt cell; colour carries the family
    daxa_u32 accent;        ///< ImU32 (0xAABBGGRR), shared by every tool of the same family
};

struct ToolBelt {
    /// Number-row selection covers the first ten slots (1-9 then 0, as every first-person voxel
    /// game has trained people to expect). BRUSH_ID_COUNT is 11, so the last tool is wheel-only
    /// and the HUD leaves its digit corner blank -- honest about there being no shortcut, rather
    /// than inventing a chord nobody will find. The belt order is the brush id order because
    /// brushes.inl declares that order to be part of the interface.
    static constexpr std::size_t DIGIT_SLOTS = 10;

    [[nodiscard]] static auto tool_count() -> std::size_t { return BRUSH_ID_COUNT; }

    [[nodiscard]] auto selected_index() const -> std::size_t { return m_selected; }
    [[nodiscard]] auto selected_brush() const -> daxa_u32 { return static_cast<daxa_u32>(m_selected); }
    /// The radius the player dialled, in metres.
    [[nodiscard]] auto radius() const -> float { return m_radius; }
    /// What the selected brush will actually use. Trees are capped at BRUSH_TREE_RADIUS_MAX by
    /// brushes.glsl, so for those two the dialled radius and the applied one differ and the HUD
    /// must show the one that is true.
    [[nodiscard]] auto effective_radius() const -> float;

    void select(std::size_t index);
    /// `notches` is the raw wheel offset. Positive (wheel pushed away) moves towards *lower*
    /// slots, which is the direction the genre settled on. Wraps.
    void cycle(int notches);
    /// `digit` is 1-9 and 0 as printed on the key; 0 means the tenth slot. Returns false for
    /// anything else, so a key dispatcher can chain into this without pre-validating.
    auto select_from_digit(int digit) -> bool;
    /// Multiplicative, matching perframe.comp.glsl exactly: the range spans a factor of 64, and a
    /// linear step is either unusable at the bottom or interminable at the top.
    void adjust_radius(int notches);
    /// Set the radius to an exact value, clamped to the brush contract's bounds. The wheel path
    /// above is multiplicative and so cannot land on a round number; --edit needs one it can put
    /// in a filename and a caption.
    void set_radius(float metres);

    // --- input -------------------------------------------------------------------------------
    /// Call from the GLFW key callback. Returns true when the key belonged to the belt, so the
    /// caller can stop looking. Does nothing while paused: the number row belongs to whatever
    /// the menu has open.
    auto on_key(int glfw_key, int glfw_action, bool paused) -> bool;
    /// Call from the GLFW scroll callback. With the size modifier held the wheel resizes the
    /// brush; without it, it changes tool.
    ///
    /// WHY THE MODIFIER IS QUERIED BY THE CALLER rather than bound as a GAME_ACTION: GLFW's
    /// scroll callback carries no modifier mask, and a keybind would have to live in the shared
    /// user_settings.json that parallel work in this repo is currently rewriting underneath every
    /// run. A held key read straight from GLFW cannot be corrupted by that.
    void on_scroll(float dy, bool size_modifier_held);

    // --- per frame ---------------------------------------------------------------------------
    /// Publishes the selection into `gpu_input.brush_selection`, services the tool-cycle action,
    /// and records whether the HUD should be up. `in_play` is `!ui.paused`.
    void perframe(bool in_play, GpuInput &gpu_input);

    /// Registers the HUD with ImGui. Call once, after the ImGui context exists. The fonts are
    /// AppUi's; passing them in rather than loading our own keeps one atlas.
    void install_hud(ImFont *label_font, ImFont *mono_font);

  private:
    void draw_hud() const;
    static void hud_hook(ImGuiContext *ctx, ImGuiContextHook *hook);

    /// BRUSH_ID_REMOVE_BALL. The default is "whatever the left mouse button already did": before
    /// any of this existed brushgen_a was hardcoded to brush_remove_ball, so a returning player's
    /// muscle memory survives the feature landing.
    std::size_t m_selected = BRUSH_ID_REMOVE_BALL;
    float m_radius = BRUSH_RADIUS_DEFAULT;

    // Wheel events are accumulated rather than rounded per event. A high-resolution wheel reports
    // fractions of a notch, and rounding each one to +-1 turns a gentle scroll into a sprint
    // across the belt while rounding each one to zero makes the wheel dead. Two accumulators
    // rather than one, so releasing the size modifier mid-scroll cannot spend a part-notch on the
    // wrong axis.
    float m_cycle_accum = 0.0f;
    float m_size_accum = 0.0f;

    /// Edge latch for GAME_ACTION_TOGGLE_BRUSH. The action array holds the raw GLFW action, which
    /// is a LEVEL (1 press, 2 auto-repeat, 0 release), so without this one press of the cycle key
    /// walks the whole belt. The shader keeps its own latch for when it drives the selection
    /// itself; this is the same rule on the side that now owns it.
    bool m_cycle_action_was_down = false;

    bool m_hud_visible = false;
    bool m_hud_installed = false;
    ImFont *m_label_font = nullptr;
    ImFont *m_mono_font = nullptr;
};
