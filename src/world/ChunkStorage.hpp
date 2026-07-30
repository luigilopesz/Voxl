#pragma once

// Palette-compressed voxel storage plus light storage for one 32^3 section.
//
// THIS HEADER IS A CONTRACT. The save format is a direct serialisation of the
// palette, the packed index words and the light bytes, so the packing rules
// below are versioned data, not an implementation detail.
//
// ============================================================================
//  THREAD SAFETY: NONE. ChunkStorage has no internal synchronisation at all.
//  The owning voxl::Chunk is responsible for making sure a worker thread never
//  reads a section while another thread writes it (see src/world/Chunk.hpp).
// ============================================================================

#include "core/Log.hpp"
#include "world/Block.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace voxl {

/// Voxel + light storage for exactly kChunkVolume (32768) voxels.
///
/// Three representations, chosen automatically:
///
///   1. UNIFORM - `bitsPerIndex() == 0`. The whole section is one block id held
///      in a single field; no heap allocation at all. This is the overwhelming
///      majority of sections in a real world (empty sky, solid stone below the
///      caves), which is why it gets a dedicated case instead of a 1-bit
///      palette: 8 bytes instead of 4 KB, and get() is a load with no shift.
///
///   2. PALETTED - a small `std::vector<BlockId>` palette plus indices packed
///      into 64-bit words at 1, 2, 4, 8 or 16 bits each.
///
///   3. (Not a separate case.) At 16 bits per index the palette can address
///      every BlockId that exists, so the representation never needs to grow
///      further and a "direct" mode would save nothing.
///
/// GROWTH AND REPACK RULE
/// ----------------------
/// bitsPerIndex only ever increases, through the sequence 0 -> 1 -> 2 -> 4 ->
/// 8 -> 16. Writing a block id that is not yet in the palette appends it; if
/// the palette is already full for the current width (2^bits entries) the whole
/// index array is repacked to the next width first. Repacking is O(volume) but
/// can happen at most four times in a section's life, and terrain generation
/// hits it during the first few hundred writes when the array is still small.
///
/// Widths are restricted to divisors of 64 on purpose: no packed index ever
/// straddles a word boundary, so get/set are a single load/store with two
/// shifts and no branch.
///
/// The palette is NOT reference counted. Refcounting every set() would double
/// the cost of the generation inner loop to reclaim memory nobody is short of;
/// instead `optimise()` does a single O(volume) sweep once, after generation or
/// after a batch of player edits, and drops whatever is unused.
class ChunkStorage {
public:
    /// Number of 64-bit words needed at a given width.
    [[nodiscard]] static constexpr std::size_t wordCountFor(std::uint8_t bitsPerIndex) noexcept
    {
        return bitsPerIndex == 0 ? 0 : kChunkVolume / (64u / bitsPerIndex);
    }

    /// Packed light nibble layout, shared with the mesher and the save format.
    static constexpr std::uint8_t kBlockLightShift = 0;
    static constexpr std::uint8_t kSunlightShift   = 4;
    static constexpr std::uint8_t kLightMask       = 0x0Fu;
    static constexpr std::uint8_t kMaxLightLevel   = 15u;

    [[nodiscard]] static constexpr std::uint8_t packLight(std::uint8_t sunlight,
                                                          std::uint8_t blockLight) noexcept
    {
        return static_cast<std::uint8_t>(((sunlight & kLightMask) << kSunlightShift) |
                                         ((blockLight & kLightMask) << kBlockLightShift));
    }
    [[nodiscard]] static constexpr std::uint8_t unpackSunlight(std::uint8_t packed) noexcept
    {
        return static_cast<std::uint8_t>((packed >> kSunlightShift) & kLightMask);
    }
    [[nodiscard]] static constexpr std::uint8_t unpackBlockLight(std::uint8_t packed) noexcept
    {
        return static_cast<std::uint8_t>((packed >> kBlockLightShift) & kLightMask);
    }

    explicit ChunkStorage(BlockId fillWith = blocks::Air) noexcept : m_uniform(fillWith) {}

    // Copyable (world edit undo buffers, tests) and movable (chunk relocation).
    ChunkStorage(const ChunkStorage&)                = default;
    ChunkStorage& operator=(const ChunkStorage&)     = default;
    ChunkStorage(ChunkStorage&&) noexcept            = default;
    ChunkStorage& operator=(ChunkStorage&&) noexcept = default;

    // ------------------------------------------------------------ voxels --

    /// `index` must come from voxl::localIndex(x, y, z) - x fastest, then z,
    /// then y. Out-of-range indices are a programming error, not a runtime one.
    [[nodiscard]] BlockId get(std::size_t index) const noexcept
    {
        VOXL_ASSERT(index < kChunkVolume, "voxel index out of range");
        if (m_bitsPerIndex == 0) {
            return m_uniform;
        }
        return m_palette[readIndex(index)];
    }

    [[nodiscard]] BlockId get(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        VOXL_ASSERT(isLocalPos(x, y, z), "local coordinate out of range");
        return get(localIndex(x, y, z));
    }

    void set(std::size_t index, BlockId id)
    {
        VOXL_ASSERT(index < kChunkVolume, "voxel index out of range");

        if (m_bitsPerIndex == 0) {
            if (id == m_uniform) {
                return;  // by far the common case during generation
            }
            expandFromUniform();
        }

        std::uint32_t paletteIndex = findPaletteIndex(id);
        if (paletteIndex == kNoPaletteEntry) {
            paletteIndex = appendPaletteEntry(id);
        }
        writeIndex(index, paletteIndex);
    }

    void set(std::int32_t x, std::int32_t y, std::int32_t z, BlockId id)
    {
        VOXL_ASSERT(isLocalPos(x, y, z), "local coordinate out of range");
        set(localIndex(x, y, z), id);
    }

    /// Collapses the section back to the uniform representation and releases
    /// the index array. Light storage is untouched.
    void fill(BlockId id)
    {
        m_uniform      = id;
        m_bitsPerIndex = 0;
        m_bitsLog2     = 0;
        std::vector<BlockId>{}.swap(m_palette);
        std::vector<std::uint64_t>{}.swap(m_indices);
    }

    [[nodiscard]] bool isUniform() const noexcept { return m_bitsPerIndex == 0; }

    /// Only meaningful while `isUniform()`.
    [[nodiscard]] BlockId uniformValue() const noexcept
    {
        VOXL_ASSERT(isUniform(), "uniformValue() on a paletted section");
        return m_uniform;
    }

    /// True when the whole section is air. The mesher skips these outright.
    [[nodiscard]] bool isEmpty() const noexcept
    {
        return m_bitsPerIndex == 0 && m_uniform == blocks::Air;
    }

    [[nodiscard]] std::size_t paletteSize() const noexcept
    {
        return m_bitsPerIndex == 0 ? 1u : m_palette.size();
    }
    [[nodiscard]] std::uint8_t bitsPerIndex() const noexcept { return m_bitsPerIndex; }

    /// Read-only view of the palette; empty while uniform. Used by the save
    /// path, which writes the palette verbatim.
    [[nodiscard]] const std::vector<BlockId>& palette() const noexcept { return m_palette; }
    [[nodiscard]] const std::vector<std::uint64_t>& indexWords() const noexcept { return m_indices; }

    /// Rebuilds the palette from the voxels actually present, shrinking the
    /// index width as far as it will go and collapsing to uniform when only one
    /// id remains. O(kChunkVolume); call after generation and after edit bursts,
    /// never per-voxel.
    void optimise()
    {
        if (m_bitsPerIndex == 0) {
            return;
        }

        // First-seen order keeps index 0 as the dominant block, which makes the
        // packed words compress better if the save format ever adds zlib.
        std::vector<BlockId>  used;
        std::vector<std::uint32_t> remap(m_palette.size(), kNoPaletteEntry);
        used.reserve(m_palette.size());

        for (std::size_t i = 0; i < kChunkVolume; ++i) {
            const std::uint32_t old = readIndex(i);
            if (remap[old] == kNoPaletteEntry) {
                remap[old] = static_cast<std::uint32_t>(used.size());
                used.push_back(m_palette[old]);
            }
        }

        if (used.size() == 1) {
            fill(used[0]);
            return;
        }

        const std::uint8_t newBits = widthFor(used.size());
        if (newBits == m_bitsPerIndex && used.size() == m_palette.size()) {
            return;  // already minimal
        }

        std::vector<std::uint64_t> repacked(wordCountFor(newBits), 0);
        const std::uint8_t newBitsLog2 = log2Of(newBits);
        for (std::size_t i = 0; i < kChunkVolume; ++i) {
            writeIndexInto(repacked, newBitsLog2, i, remap[readIndex(i)]);
        }

        m_palette      = std::move(used);
        m_indices      = std::move(repacked);
        m_bitsPerIndex = newBits;
        m_bitsLog2     = newBitsLog2;
        m_palette.shrink_to_fit();
    }

    // ------------------------------------------------------------- light --

    /// Packed byte: high nibble sunlight, low nibble block light.
    [[nodiscard]] std::uint8_t light(std::size_t index) const noexcept
    {
        VOXL_ASSERT(index < kChunkVolume, "voxel index out of range");
        return m_light.empty() ? m_uniformLight : m_light[index];
    }
    [[nodiscard]] std::uint8_t light(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        return light(localIndex(x, y, z));
    }

    [[nodiscard]] std::uint8_t sunlight(std::size_t index) const noexcept
    {
        return unpackSunlight(light(index));
    }
    [[nodiscard]] std::uint8_t blockLight(std::size_t index) const noexcept
    {
        return unpackBlockLight(light(index));
    }

    void setLight(std::size_t index, std::uint8_t packed)
    {
        VOXL_ASSERT(index < kChunkVolume, "voxel index out of range");
        if (m_light.empty()) {
            if (packed == m_uniformLight) {
                return;
            }
            materialiseLight();
        }
        m_light[index] = packed;
    }

    void setSunlight(std::size_t index, std::uint8_t level)
    {
        setLight(index, packLight(level, blockLight(index)));
    }
    void setBlockLight(std::size_t index, std::uint8_t level)
    {
        setLight(index, packLight(sunlight(index), level));
    }

    /// O(1) uniform light, mirroring the uniform voxel case. An all-air section
    /// above the terrain is sunlight 15 everywhere and costs one byte.
    void fillLight(std::uint8_t sunlightLevel, std::uint8_t blockLightLevel)
    {
        m_uniformLight = packLight(sunlightLevel, blockLightLevel);
        std::vector<std::uint8_t>{}.swap(m_light);
    }

    /// True once per-voxel light bytes have been allocated.
    [[nodiscard]] bool hasLightData() const noexcept { return !m_light.empty(); }
    [[nodiscard]] const std::vector<std::uint8_t>& lightData() const noexcept { return m_light; }
    [[nodiscard]] std::uint8_t uniformLight() const noexcept { return m_uniformLight; }

    // ------------------------------------------------------------ memory --

    /// Heap bytes owned by this section, excluding the object itself.
    [[nodiscard]] std::size_t heapBytes() const noexcept
    {
        return m_palette.capacity() * sizeof(BlockId) +
               m_indices.capacity() * sizeof(std::uint64_t) +
               m_light.capacity() * sizeof(std::uint8_t);
    }

    /// Total footprint reported by the debug overlay.
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept
    {
        return sizeof(ChunkStorage) + heapBytes();
    }

private:
    static constexpr std::uint32_t kNoPaletteEntry = 0xFFFFFFFFu;

    [[nodiscard]] static constexpr std::uint8_t log2Of(std::uint8_t bits) noexcept
    {
        return bits == 1    ? std::uint8_t{0}
               : bits == 2  ? std::uint8_t{1}
               : bits == 4  ? std::uint8_t{2}
               : bits == 8  ? std::uint8_t{3}
                            : std::uint8_t{4};
    }

    /// Narrowest legal width that can address `entries` palette slots.
    [[nodiscard]] static constexpr std::uint8_t widthFor(std::size_t entries) noexcept
    {
        return entries <= 2    ? std::uint8_t{1}
               : entries <= 4  ? std::uint8_t{2}
               : entries <= 16 ? std::uint8_t{4}
               : entries <= 256 ? std::uint8_t{8}
                                : std::uint8_t{16};
    }

    [[nodiscard]] std::uint32_t readIndex(std::size_t voxel) const noexcept
    {
        // Widths divide 64, so slotsPerWord is a power of two and both the word
        // selection and the shift are pure bit arithmetic.
        const std::uint32_t slotsLog2 = 6u - m_bitsLog2;
        const std::size_t   word      = voxel >> slotsLog2;
        const std::uint32_t slot      = static_cast<std::uint32_t>(voxel) & ((1u << slotsLog2) - 1u);
        const std::uint32_t shift     = slot << m_bitsLog2;
        const std::uint64_t mask      = (std::uint64_t{1} << m_bitsPerIndex) - 1u;
        return static_cast<std::uint32_t>((m_indices[word] >> shift) & mask);
    }

    static void writeIndexInto(std::vector<std::uint64_t>& words, std::uint8_t bitsLog2,
                               std::size_t voxel, std::uint32_t value) noexcept
    {
        const std::uint32_t slotsLog2 = 6u - bitsLog2;
        const std::size_t   word      = voxel >> slotsLog2;
        const std::uint32_t slot      = static_cast<std::uint32_t>(voxel) & ((1u << slotsLog2) - 1u);
        const std::uint32_t shift     = slot << bitsLog2;
        const std::uint64_t mask      = (std::uint64_t{1} << (1u << bitsLog2)) - 1u;
        words[word] = (words[word] & ~(mask << shift)) | ((static_cast<std::uint64_t>(value) & mask) << shift);
    }

    void writeIndex(std::size_t voxel, std::uint32_t value) noexcept
    {
        writeIndexInto(m_indices, m_bitsLog2, voxel, value);
    }

    [[nodiscard]] std::uint32_t findPaletteIndex(BlockId id) const noexcept
    {
        // Linear scan: real sections hold a handful of distinct blocks, and a
        // hash map's per-section allocation would cost more than it saves.
        for (std::size_t i = 0; i < m_palette.size(); ++i) {
            if (m_palette[i] == id) {
                return static_cast<std::uint32_t>(i);
            }
        }
        return kNoPaletteEntry;
    }

    /// Uniform -> 1 bit per index, every voxel pointing at palette slot 0.
    void expandFromUniform()
    {
        m_palette.assign(1, m_uniform);
        m_palette.reserve(2);
        m_bitsPerIndex = 1;
        m_bitsLog2     = 0;
        m_indices.assign(wordCountFor(1), 0);
    }

    std::uint32_t appendPaletteEntry(BlockId id)
    {
        const std::size_t capacity = std::size_t{1} << m_bitsPerIndex;
        if (m_palette.size() >= capacity) {
            growWidth();
        }
        m_palette.push_back(id);
        return static_cast<std::uint32_t>(m_palette.size() - 1);
    }

    void growWidth()
    {
        VOXL_CHECK(m_bitsPerIndex < 16, "palette overflow: more than 65536 block ids in one section");
        const std::uint8_t newBits     = static_cast<std::uint8_t>(m_bitsPerIndex * 2);
        const std::uint8_t newBitsLog2 = static_cast<std::uint8_t>(m_bitsLog2 + 1);

        std::vector<std::uint64_t> repacked(wordCountFor(newBits), 0);
        for (std::size_t i = 0; i < kChunkVolume; ++i) {
            writeIndexInto(repacked, newBitsLog2, i, readIndex(i));
        }

        m_indices      = std::move(repacked);
        m_bitsPerIndex = newBits;
        m_bitsLog2     = newBitsLog2;

        // Reserve the new width's capacity, but never speculatively allocate
        // the 16-bit worst case: no real section holds hundreds of block types.
        const std::size_t widthCapacity = std::size_t{1} << newBits;
        m_palette.reserve(widthCapacity > 256u ? 256u : widthCapacity);
    }

    void materialiseLight() { m_light.assign(kChunkVolume, m_uniformLight); }

    /// Valid only while `m_bitsPerIndex == 0`.
    BlockId      m_uniform      = blocks::Air;
    std::uint8_t m_bitsPerIndex = 0;
    std::uint8_t m_bitsLog2     = 0;
    /// Uniform light value used while `m_light` is unallocated.
    std::uint8_t m_uniformLight = 0;

    std::vector<BlockId>       m_palette;
    std::vector<std::uint64_t> m_indices;
    std::vector<std::uint8_t>  m_light;
};

}  // namespace voxl
