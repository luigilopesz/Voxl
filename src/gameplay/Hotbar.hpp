#pragma once

// The nine-slot block selection bar.
//
// Pure gameplay state: no GL, no ImGui, no input library. Input code translates
// a key or a wheel event into one of the calls below, and src/ui/Hud.cpp reads
// the result. Keeping it dependency-free is what makes it unit-testable.
//
// Thread safety: none. Main-thread state, mutated from the input handler and
// read by the HUD in the same frame.

#include "world/Block.hpp"

#include <array>
#include <cstddef>

namespace voxl {

class Hotbar {
public:
    /// Nine, because the number row is the selection mechanism and 0 is reserved
    /// for a future "off-hand"/utility slot rather than being slot 10.
    static constexpr std::size_t kSlotCount = 9;

    /// Populated with the starter loadout (see kDefaultLoadout in the .cpp).
    Hotbar() noexcept;

    /// Out-of-range indices resolve to air instead of asserting: the HUD may
    /// iterate a wider range than the bar while a layout is being tweaked.
    [[nodiscard]] BlockId slot(std::size_t index) const noexcept;

    /// Ignored when `index` is out of range.
    void setSlot(std::size_t index, BlockId id) noexcept;

    [[nodiscard]] std::size_t selectedIndex() const noexcept { return m_selected; }
    [[nodiscard]] BlockId     selectedBlock() const noexcept { return m_slots[m_selected]; }

    /// Ignored when `index` is out of range, so a stray key code cannot leave
    /// the bar pointing at nothing.
    void select(std::size_t index) noexcept;

    /// Mouse-wheel cycling. `delta` is the raw wheel offset: a positive value
    /// (wheel away from the user) moves towards *lower* slot indices, which is
    /// the direction every other first-person voxel game uses. Wraps, and
    /// handles the multi-notch deltas a high-resolution wheel reports.
    void cycle(int delta) noexcept;

    /// Number-row selection. `digit` is 1-9 as printed on the key. Returns false
    /// for anything else so a caller can chain this into a key dispatcher
    /// without pre-validating.
    bool selectFromDigit(int digit) noexcept;

    [[nodiscard]] const std::array<BlockId, kSlotCount>& slots() const noexcept { return m_slots; }

private:
    std::array<BlockId, kSlotCount> m_slots{};
    std::size_t                     m_selected = 0;
};

}  // namespace voxl
