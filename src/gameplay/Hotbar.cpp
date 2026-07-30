#include "gameplay/Hotbar.hpp"

namespace voxl {
namespace {

/// Starter loadout. Ordered so the blocks a player reaches for while terraforming
/// sit under the fingers that do not need to leave the movement keys: dirt and
/// stone first, decoration last.
constexpr std::array<BlockId, Hotbar::kSlotCount> kDefaultLoadout{
    blocks::Stone, blocks::Dirt,   blocks::Grass,  blocks::Sand,     blocks::Cobblestone,
    blocks::Planks, blocks::Glass, blocks::Leaves, blocks::Glowstone,
};

}  // namespace

Hotbar::Hotbar() noexcept : m_slots(kDefaultLoadout) {}

BlockId Hotbar::slot(std::size_t index) const noexcept
{
    return index < kSlotCount ? m_slots[index] : blocks::Air;
}

void Hotbar::setSlot(std::size_t index, BlockId id) noexcept
{
    if (index < kSlotCount) {
        m_slots[index] = id;
    }
}

void Hotbar::select(std::size_t index) noexcept
{
    if (index < kSlotCount) {
        m_selected = index;
    }
}

void Hotbar::cycle(int delta) noexcept
{
    if (delta == 0) {
        return;
    }

    // Signed arithmetic on purpose: reducing modulo kSlotCount in std::size_t
    // first would turn a wheel-down step into a huge positive offset. The
    // negation is the "wheel up selects the previous slot" convention.
    const int count   = static_cast<int>(kSlotCount);
    int       wrapped = (static_cast<int>(m_selected) - delta) % count;
    if (wrapped < 0) {
        wrapped += count;
    }
    m_selected = static_cast<std::size_t>(wrapped);
}

bool Hotbar::selectFromDigit(int digit) noexcept
{
    if (digit < 1 || digit > static_cast<int>(kSlotCount)) {
        return false;
    }
    m_selected = static_cast<std::size_t>(digit - 1);
    return true;
}

}  // namespace voxl
