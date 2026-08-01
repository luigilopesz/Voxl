#pragma once

#include <core.inl>

// The "simple" allocator declared here (as well as implemented both here and further
// for the GLSL side in allocator.glsl) is just a simple free-list linear allocator.

// HardMaxElements_ is a *provable* upper bound on how many elements the GPU side can ever ask
// for, given the world's dimensions. It is not a VRAM budget -- that is computed at runtime in
// AllocatorBufferState::create() -- it is the point past which growing is simply pointless
// because nothing could consume the extra capacity. The allocator caps at min(the two).
#define DECL_SIMPLE_ALLOCATOR(AllocatorType_, ElementType_, ElementMultiplier_, IndexType_, MaxAllocPerFrame_, HardMaxElements_) \
    struct AllocatorType_ {                                                                                                     \
        daxa_RWBufferPtr(ElementType_) heap;                                                                                    \
        daxa_RWBufferPtr(IndexType_) available_element_stack;                                                                    \
        daxa_RWBufferPtr(IndexType_) released_element_stack;                                                                     \
        daxa_i32 element_count;                                                                                                 \
        daxa_i32 available_element_stack_size;                                                                                   \
        daxa_i32 released_element_stack_size;                                                                                    \
    };                                                                                                                          \
    struct AllocatorType_##GpuOutput {                                                                                          \
        daxa_u32 current_element_count;                                                                                         \
    };                                                                                                                          \
    DAXA_DECL_BUFFER_PTR(AllocatorType_)                                                                                        \
    CPU_ONLY(DECL_SIMPLE_ALLOCATOR_CONSTANTS(AllocatorType_, ElementType_, ElementMultiplier_, IndexType_, MaxAllocPerFrame_, HardMaxElements_))

#define DECL_SIMPLE_ALLOCATOR_CONSTANTS(AllocatorType_, ElementType_, ElementMultiplier_, IndexType_, MaxAllocPerFrame_, HardMaxElements_) \
    template <>                                                                                                                     \
    struct AllocatorConstants<AllocatorType_> {                                                                                     \
        using AllocatorType = AllocatorType_;                                                                                       \
        using ElementType = ElementType_;                                                                                           \
        using IndexType = IndexType_;                                                                                               \
        static constexpr size_t ELEMENT_MULTIPLIER = ElementMultiplier_;                                                            \
        static constexpr daxa_u32 MAX_ELEMENT_ALLOCATIONS_PER_FRAME = MaxAllocPerFrame_;                                            \
        static constexpr daxa_u64 HARD_MAX_ELEMENTS = HardMaxElements_;                                                             \
        static constexpr char const *const task_allocator_buffer_name = "task_" #AllocatorType_ "_allocator_buffer";                \
        static constexpr char const *const task_element_buffer_name = "task_" #AllocatorType_ "_element_buffer";                    \
        static constexpr char const *const task_old_element_buffer_name = "task" #AllocatorType_ "_old_element_buffer";             \
        static constexpr char const *const allocator_buffer_name = #AllocatorType_ "_allocator_buffer";                             \
        static constexpr char const *const element_buffer_name = #AllocatorType_ "_element_buffer";                                 \
        static constexpr char const *const available_element_stack_buffer_name = #AllocatorType_ "_available_element_stack_buffer"; \
        static constexpr char const *const released_element_stack_buffer_name = #AllocatorType_ "_released_element_stack_buffer";   \
    };

#define DECL_SIMPLE_STATIC_ALLOCATOR(AllocatorType_, ElementType_, ElementCount_, IndexType_) \
    struct AllocatorType_ {                                                                   \
        daxa_RWBufferPtr(ElementType_) heap;                                                  \
        daxa_RWBufferPtr(IndexType_) available_element_stack;                                 \
        daxa_RWBufferPtr(IndexType_) released_element_stack;                                  \
        daxa_i32 element_count;                                                               \
        daxa_i32 available_element_stack_size;                                                \
        daxa_i32 released_element_stack_size;                                                 \
    };                                                                                        \
    DAXA_DECL_BUFFER_PTR(AllocatorType_)                                                      \
    CPU_ONLY(DECL_SIMPLE_STATIC_ALLOCATOR_CONSTANTS(AllocatorType_, ElementType_, ElementCount_, IndexType_))

#define DECL_SIMPLE_STATIC_ALLOCATOR_CONSTANTS(AllocatorType_, ElementType_, ElementCount_, IndexType_)                             \
    template <>                                                                                                                     \
    struct StaticAllocatorConstants<AllocatorType_> {                                                                               \
        using AllocatorType = AllocatorType_;                                                                                       \
        using ElementType = ElementType_;                                                                                           \
        using IndexType = IndexType_;                                                                                               \
        static constexpr daxa_u32 MAX_ELEMENTS = ElementCount_;                                                                     \
        static constexpr char const *const allocator_buffer_name = #AllocatorType_ "_allocator_buffer";                             \
        static constexpr char const *const element_buffer_name = #AllocatorType_ "_element_buffer";                                 \
        static constexpr char const *const available_element_stack_buffer_name = #AllocatorType_ "_available_element_stack_buffer"; \
        static constexpr char const *const released_element_stack_buffer_name = #AllocatorType_ "_released_element_stack_buffer";   \
    };

#define SIMPLE_STATIC_ALLOCATOR_BUFFER_USE_N 4
#define SIMPLE_STATIC_ALLOCATOR_USE_BUFFERS(HeapUsage, AllocatorType_)                                                            \
    DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE_CONCURRENT, daxa_RWBufferPtr(AllocatorType_), AllocatorType_##_allocator_buffer) \
    DAXA_TH_BUFFER(HeapUsage, AllocatorType_##_heap)                                                                              \
    DAXA_TH_BUFFER(COMPUTE_SHADER_READ_WRITE_CONCURRENT, AllocatorType_##_available_elements)                                     \
    DAXA_TH_BUFFER(COMPUTE_SHADER_READ_WRITE_CONCURRENT, AllocatorType_##_released_elements)

#define SIMPLE_STATIC_ALLOCATOR_BUFFERS_PUSH_USES(AllocatorType_, var_name) \
    daxa_RWBufferPtr(AllocatorType_) var_name = push.uses.AllocatorType_##_allocator_buffer;

#define SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(TaskHeadName, AllocatorType_, allocator)                                                          \
    daxa::TaskViewVariant{std::pair{TaskHeadName::AT.AllocatorType_##_allocator_buffer, allocator.allocator_buffer.task_resource}},                     \
        daxa::TaskViewVariant{std::pair{TaskHeadName::AT.AllocatorType_##_heap, allocator.element_buffer.task_resource}},                               \
        daxa::TaskViewVariant{std::pair{TaskHeadName::AT.AllocatorType_##_available_elements, allocator.available_element_stack_buffer.task_resource}}, \
        daxa::TaskViewVariant {                                                                                                                      \
        std::pair { TaskHeadName::AT.AllocatorType_##_released_elements, allocator.released_element_stack_buffer.task_resource }                        \
    }

#if defined(__cplusplus)

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fmt/format.h>

// daxa_dvc_get_vk_physical_device() lives in the C API, and daxa/c/types.h is what pulls in
// <vulkan/vulkan.h>. Both are already on this target's include path (vcpkg's vulkan-headers
// port) and vulkan-1.lib is already on its link line, so this costs no build-system change.
#include <daxa/c/device.h>

// ---------------------------------------------------------------------------------------------
// VRAM budget for the growable heap allocators.
// ---------------------------------------------------------------------------------------------
// WHY THIS EXISTS. check_for_realloc() below grows the heap geometrically while the OLD and the
// NEW buffers are both alive -- realloc() copies from one to the other, so it cannot be
// otherwise. Upstream did that with no cap, no out-of-memory check and no shrink path, so the
// transient peak was unbounded. Measured on this project's 6 GB target card, the demo world
// settled at 1376256 pages = 2782 MiB of heap; its next growth step would have wanted 4791 MiB
// more while still holding the old buffer. That is not a stutter.
//
// It is not theoretical either. On 2026-07-31, with a second engine instance already holding
// 4.3 GB of the 6144 MiB card, a run of tools/run.ps1 died at t=17 s with an EMPTY stderr, an
// empty log, and no diagnostic of any kind. Silent death is the failure mode being fixed here.
//
// THE POLICY: the heap's own buffers may occupy at most `budget` bytes at ANY instant, including
// mid-realloc. When the geometric 1.5x step does not fit, we grow instead to the largest size
// that does -- in one step, not by crawling. When not even the minimum safe size fits we REFUSE
// the growth, log it, and surface it in the debug overlay. Refusing is a deliberate, visible
// degradation; calling create_buffer() and faulting is not.
//
// Measured effect of the cap alone, world unchanged (CHUNKS_PER_AXIS 32, 30 s moving soak):
//   heap capacity 1376256 -> 1168766 pages, peak VRAM 4592 -> 4119 MiB, frame time 21.11 -> 21.46
//   ms. The trimmed capacity was pure slack: heap *usage* was 1629 MB before and 1647 MB after.
namespace gpu_heap_budget {
    inline constexpr daxa_u64 MiB = 1024ull * 1024ull;

    // Reserved for everything on the card that is NOT this heap: render targets, the G-buffer
    // stack, the irradiance cache, the chunk table, the grass/flower/particle buffers, FSR2's
    // internals, and Daxa's own allocations.
    //
    // MEASURED, not guessed, at the largest this figure has ever been here -- CHUNKS_PER_AXIS 32
    // with MAX_GRASS_BLADES 1<<22 (docs/BASELINE.md: peak 4592 MiB, heap at 1376256 pages):
    //   1376256 elements x 2120 B/element  = 2782 MiB of heap
    //   idle desktop before launch          =  280 MiB
    //   4592 - 2782 - 280                   = 1530 MiB of engine, everything else
    // Re-measured on the shipped world (CHUNKS_PER_AXIS 16, MAX_GRASS_BLADES 1<<20, stationary at
    // spawn, 1280x720): 2127 MiB total - 254 MiB desktop - 795 MiB heap = 1078 MiB. So 1536 MiB
    // leaves 458 MiB of margin for a larger window: the render targets are the part of this that
    // scales with resolution, and 1920x1080 is 2.25x the pixels.
    inline constexpr daxa_u64 ENGINE_RESERVE_BYTES = 1536 * MiB;

    // Reserved for the desktop compositor and whatever else the user has open. The idle
    // baseline measured across four runs was 273-288 MiB; doubled, because a browser or a
    // second window is normal and the heap must not be the thing that pushes the card over.
    inline constexpr daxa_u64 SYSTEM_RESERVE_BYTES = 512 * MiB;

    // Used only if the device query fails outright. Deliberately small: an unknown card is not
    // an excuse to allocate optimistically.
    inline constexpr daxa_u64 FALLBACK_BUDGET_BYTES = 1024 * MiB;

    /// Size of the largest DEVICE_LOCAL memory heap, in bytes. 0 if it cannot be determined.
    /// Largest rather than the sum of all DEVICE_LOCAL heaps: on a discrete part there is
    /// exactly one real VRAM heap, but ReBAR/host-visible-device-local memory *types* alias
    /// into it on several drivers, and summing heaps would then double count.
    ///
    /// Ask the driver rather than trusting the box: on the RTX 3050 "6 GB" this returns
    /// 6 293 MB (6001 MiB), not the 6144 MiB nvidia-smi reports as board memory. The ~143 MiB
    /// difference is driver-reserved and was never ours to budget.
    inline auto device_local_bytes(daxa::Device &device) -> daxa_u64 {
        if (!device.is_valid()) {
            return 0;
        }
        VkPhysicalDevice const physical_device = daxa_dvc_get_vk_physical_device(device.get());
        if (physical_device == nullptr) {
            return 0;
        }
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &props);
        daxa_u64 largest = 0;
        for (uint32_t heap_i = 0; heap_i < props.memoryHeapCount; ++heap_i) {
            if ((props.memoryHeaps[heap_i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                largest = std::max<daxa_u64>(largest, props.memoryHeaps[heap_i].size);
            }
        }
        return largest;
    }

    /// Bytes the growable heap is allowed to occupy at any instant, mid-realloc included.
    inline auto compute(daxa::Device &device) -> daxa_u64 {
        // TEST OVERRIDE -- VOXL_HEAP_BUDGET_MB.
        //
        // WHY IT EXISTS. Everything below is a degradation policy that, on the shipped world,
        // CANNOT BE REACHED, at any CHUNKS_PER_AXIS. Measured across 16/32/48/64: the heap
        // settles at 393216 pages (830 MB) against a 1955198-page (4145 MB) cap, with zero
        // refusals, because the island's content does not grow when the world box does. Worse,
        // the heap's INITIAL capacity is (FRAMES_IN_FLIGHT+1) * PALETTES_PER_CHUNK *
        // MAX_CHUNK_UPDATES_PER_FRAME = 131072 pages = 278 MB, and the island only ever consumes
        // about 6100 of those, so the very first allocation is already 21x what the world needs.
        //
        // A degradation policy nobody can provoke is a policy nobody has checked, and this one
        // was added in response to a crash and had never once run in anger. This makes it
        // reachable on demand: docs/SCALE_LIMITS.md sec 6 is what it is for, and it distinguishes
        // "GROWTH REFUSED" (the safety margin can no longer be guaranteed) from actual heap
        // exhaustion, which are not the same event and have very different consequences.
        //
        // Read once at startup. Unset, the behaviour is byte-for-byte what it was before.
        if (auto const *over = std::getenv("VOXL_HEAP_BUDGET_MB"); over != nullptr && *over != '\0') {
            auto const mb = std::strtoull(over, nullptr, 10);
            if (mb > 0) {
                return mb * MiB;
            }
        }
        auto const total = device_local_bytes(device);
        if (total == 0) {
            return FALLBACK_BUDGET_BYTES;
        }
        auto const reserved = ENGINE_RESERVE_BYTES + SYSTEM_RESERVE_BYTES;
        if (total <= reserved) {
            // A card smaller than the reserves. Give the heap a quarter of it and let the cap
            // do its job loudly rather than refusing to start.
            return total / 4;
        }
        return total - reserved;
    }
} // namespace gpu_heap_budget

template <typename T>
struct AllocatorConstants {
    using AllocatorType = T;
    using ElementType = daxa_u32;
    using IndexType = daxa_u32;
    static constexpr size_t ELEMENT_MULTIPLIER = 1;
    static constexpr daxa_u32 MAX_ELEMENT_ALLOCATIONS_PER_FRAME = 1;
    static constexpr daxa_u64 HARD_MAX_ELEMENTS = ~0ull;
    static constexpr char const *const task_allocator_buffer_name = "task_allocator_buffer";
    static constexpr char const *const task_element_buffer_name = "task_element_buffer";
    static constexpr char const *const task_old_element_buffer_name = "task_old_element_buffer";
    static constexpr char const *const allocator_buffer_name = "allocator_buffer";
    static constexpr char const *const element_buffer_name = "element_buffer";
    static constexpr char const *const available_element_stack_buffer_name = "available_element_stack_buffer";
    static constexpr char const *const released_element_stack_buffer_name = "released_element_stack_buffer";
};
template <typename T>
struct StaticAllocatorConstants {
    using AllocatorType = T;
    using ElementType = daxa_u32;
    using IndexType = daxa_u32;
    static constexpr size_t MAX_ELEMENTS = 1;
    static constexpr char const *const allocator_buffer_name = "allocator_buffer";
    static constexpr char const *const element_buffer_name = "element_buffer";
    static constexpr char const *const available_element_stack_buffer_name = "available_element_stack_buffer";
    static constexpr char const *const released_element_stack_buffer_name = "released_element_stack_buffer";
};

template <typename T>
struct AllocatorBufferState {
    daxa::Device device;
    daxa::BufferId allocator_buffer;
    daxa::BufferId element_buffer;
    daxa::BufferId available_element_stack_buffer;
    daxa::BufferId released_element_stack_buffer;
    daxa::TaskBuffer task_allocator_buffer{{.name = AllocatorConstants<T>::task_allocator_buffer_name}};
    daxa::TaskBuffer task_element_buffer{{.name = AllocatorConstants<T>::task_element_buffer_name}};
    daxa::TaskBuffer task_old_element_buffer{{.name = AllocatorConstants<T>::task_old_element_buffer_name}};
    daxa_u32 current_element_count = 0;
    daxa_u32 next_element_count = 0;
    daxa_u32 prev_element_count = 0;

    // --- cap state, all readable by the debug overlay ---------------------------------------
    // Bytes this heap may occupy at any instant, mid-realloc included. Computed once in
    // create() from the device's own VRAM heap; see namespace gpu_heap_budget above.
    daxa_u64 budget_bytes = 0;
    // Largest capacity the cap will ever allow, in elements. min(budget, HARD_MAX_ELEMENTS).
    daxa_u32 max_element_count = 0;
    // Number of growth requests refused since startup. Non-zero means the world is bigger than
    // the card and the engine is knowingly running degraded -- see check_for_realloc().
    daxa_u32 growth_refusals = 0;
    // Set once the geometric 1.5x step stopped fitting and we started crawling linearly.
    bool growth_throttled = false;
    bool budget_logged = false;

    // Bytes of VRAM one element of capacity actually costs. The element array is the obvious
    // part, but available_element_stack and released_element_stack are resized in lock-step
    // with it, so each element also costs two index slots. For the voxel page allocator that is
    // 2112 + 8 = 2120 B/page, and ignoring the 8 understates the heap by 0.4%.
    static constexpr size_t ELEMENT_BYTES =
        sizeof(typename AllocatorConstants<T>::ElementType) * AllocatorConstants<T>::ELEMENT_MULTIPLIER;
    static constexpr size_t TOTAL_BYTES_PER_ELEMENT =
        ELEMENT_BYTES + 2 * sizeof(typename AllocatorConstants<T>::IndexType);
    // Slack the capacity must always carry beyond what the CPU has seen the GPU consume. See the
    // invariant note on check_for_realloc(). Hoisted here so growth_headroom_steps() below can
    // model the growth with the *same* arithmetic the allocator actually uses -- an "N steps
    // left" readout computed a different way is exactly the sort of number nobody should trust.
    static constexpr daxa_u64 PER_FRAME_HEADROOM =
        static_cast<daxa_u64>(AllocatorConstants<T>::MAX_ELEMENT_ALLOCATIONS_PER_FRAME) * (FRAMES_IN_FLIGHT + 1);

    /// VRAM this heap currently occupies, in bytes.
    [[nodiscard]] auto capacity_bytes() const -> daxa_u64 {
        return static_cast<daxa_u64>(current_element_count) * TOTAL_BYTES_PER_ELEMENT;
    }
    /// Bytes the cap allows. Equivalent to max_element_count expressed in VRAM.
    [[nodiscard]] auto capacity_limit_bytes() const -> daxa_u64 {
        return static_cast<daxa_u64>(max_element_count) * TOTAL_BYTES_PER_ELEMENT;
    }
    /// How many further 1.5x growth steps would still fit under the cap. 0 means the next
    /// growth is already going to be throttled or refused. This is the "headroom" figure --
    /// the whole reason the uncapped allocator was dangerous was that nobody could see it.
    [[nodiscard]] auto growth_headroom_steps() const -> daxa_u32 {
        auto steps = daxa_u32{0};
        auto candidate = static_cast<daxa_u64>(current_element_count);
        // steps < 64 is belt-and-braces; candidate == 0 would otherwise never grow and would
        // report a meaningless 64. It cannot happen after create(), but the overlay reads this.
        while (steps < 64 && candidate != 0) {
            auto const grown = (candidate + PER_FRAME_HEADROOM) * 3 / 2;
            // Same test check_for_realloc() applies: old and new must be resident together.
            if (grown > max_element_count || (candidate + grown) * TOTAL_BYTES_PER_ELEMENT > budget_bytes) {
                break;
            }
            candidate = grown;
            ++steps;
        }
        return steps;
    }

    void create(GpuContext &gpu_context) {
        device = gpu_context.device;
        constexpr auto MAX_ELEMENT_ALLOCATIONS_PER_FRAME = AllocatorConstants<T>::MAX_ELEMENT_ALLOCATIONS_PER_FRAME;
        daxa_u32 element_count = (FRAMES_IN_FLIGHT + 1) * MAX_ELEMENT_ALLOCATIONS_PER_FRAME;
        current_element_count = element_count;

        budget_bytes = gpu_heap_budget::compute(device);
        auto const budget_elements = budget_bytes / TOTAL_BYTES_PER_ELEMENT;
        max_element_count = static_cast<daxa_u32>(
            std::min<daxa_u64>({budget_elements, AllocatorConstants<T>::HARD_MAX_ELEMENTS, ~0u}));
        // The initial allocation is the one size we cannot refuse -- without it the GPU has
        // nowhere to put a single page. If it does not fit the cap the cap is misconfigured, so
        // say so at startup rather than corrupting memory quietly at frame 300.
        assert(max_element_count >= current_element_count &&
               "VRAM budget cannot even cover the allocator's initial capacity");
        allocator_buffer = device.create_buffer({
            .size = sizeof(typename AllocatorConstants<T>::AllocatorType),
            .name = AllocatorConstants<T>::allocator_buffer_name,
        });
        element_buffer = device.create_buffer({
            .size = sizeof(typename AllocatorConstants<T>::ElementType) * AllocatorConstants<T>::ELEMENT_MULTIPLIER * current_element_count,
            .name = AllocatorConstants<T>::element_buffer_name,
        });
        available_element_stack_buffer = device.create_buffer({
            .size = sizeof(typename AllocatorConstants<T>::IndexType) * current_element_count,
            .name = AllocatorConstants<T>::available_element_stack_buffer_name,
        });
        released_element_stack_buffer = device.create_buffer({
            .size = sizeof(typename AllocatorConstants<T>::IndexType) * current_element_count,
            .name = AllocatorConstants<T>::released_element_stack_buffer_name,
        });
        task_allocator_buffer.set_buffers({.buffers = std::array{allocator_buffer}});
        task_element_buffer.set_buffers({
            .buffers = std::array{
                element_buffer,
                available_element_stack_buffer,
                released_element_stack_buffer,
            },
        });
        task_old_element_buffer.set_buffers({
            .buffers = std::array{
                element_buffer,
                available_element_stack_buffer,
                released_element_stack_buffer,
            },
        });
    }
    ~AllocatorBufferState() {
        if (!element_buffer.is_empty()) {
            device.destroy_buffer(element_buffer);
        }
        if (!available_element_stack_buffer.is_empty()) {
            device.destroy_buffer(available_element_stack_buffer);
        }
        if (!released_element_stack_buffer.is_empty()) {
            device.destroy_buffer(released_element_stack_buffer);
        }
        device.destroy_buffer(allocator_buffer);
    }
    void init(daxa::Device &device, daxa::CommandRecorder &recorder) {
        auto staging_buffer = device.create_buffer({
            .size = sizeof(typename AllocatorConstants<T>::AllocatorType),
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "staging_buffer",
        });
        recorder.destroy_buffer_deferred(staging_buffer);
        auto *buffer_ptr = device.get_host_address_as<typename AllocatorConstants<T>::AllocatorType>(staging_buffer).value();
        *buffer_ptr = typename AllocatorConstants<T>::AllocatorType{
            .heap = device.get_device_address(element_buffer).value(),
            .available_element_stack = device.get_device_address(available_element_stack_buffer).value(),
            .released_element_stack = device.get_device_address(released_element_stack_buffer).value(),
            .element_count = 0,
            .available_element_stack_size = 0,
            .released_element_stack_size = 0,
        };
        recorder.copy_buffer_to_buffer({
            .src_buffer = staging_buffer,
            .dst_buffer = task_allocator_buffer.get_state().buffers[0],
            .size = sizeof(typename AllocatorConstants<T>::AllocatorType),
        });
    }
    void clear_buffers(daxa::CommandRecorder &recorder) {
        recorder.clear_buffer({
            .buffer = task_element_buffer.get_state().buffers[0],
            .offset = 0,
            .size = sizeof(typename AllocatorConstants<T>::ElementType) * AllocatorConstants<T>::ELEMENT_MULTIPLIER * current_element_count,
            .clear_value = 0,
        });
        recorder.clear_buffer({
            .buffer = task_element_buffer.get_state().buffers[1],
            .offset = 0,
            .size = sizeof(typename AllocatorConstants<T>::IndexType) * current_element_count,
            .clear_value = 0,
        });
        recorder.clear_buffer({
            .buffer = task_element_buffer.get_state().buffers[2],
            .offset = 0,
            .size = sizeof(typename AllocatorConstants<T>::IndexType) * current_element_count,
            .clear_value = 0,
        });
    }
    void realloc(daxa::Device &device, daxa::CommandRecorder &recorder) {
        recorder.copy_buffer_to_buffer({
            .src_buffer = task_old_element_buffer.get_state().buffers[0],
            .dst_buffer = task_element_buffer.get_state().buffers[0],
            .src_offset = 0,
            .dst_offset = 0,
            .size = sizeof(typename AllocatorConstants<T>::ElementType) * AllocatorConstants<T>::ELEMENT_MULTIPLIER * prev_element_count,
        });
        recorder.copy_buffer_to_buffer({
            .src_buffer = task_old_element_buffer.get_state().buffers[1],
            .dst_buffer = task_element_buffer.get_state().buffers[1],
            .src_offset = 0,
            .dst_offset = 0,
            .size = sizeof(typename AllocatorConstants<T>::IndexType) * prev_element_count,
        });
        recorder.copy_buffer_to_buffer({
            .src_buffer = task_old_element_buffer.get_state().buffers[2],
            .dst_buffer = task_element_buffer.get_state().buffers[2],
            .src_offset = 0,
            .dst_offset = 0,
            .size = sizeof(typename AllocatorConstants<T>::IndexType) * prev_element_count,
        });
        recorder.destroy_buffer_deferred(task_old_element_buffer.get_state().buffers[0]);
        recorder.destroy_buffer_deferred(task_old_element_buffer.get_state().buffers[1]);
        recorder.destroy_buffer_deferred(task_old_element_buffer.get_state().buffers[2]);
        task_old_element_buffer.set_buffers({});
        auto staging_buffer = device.create_buffer({
            .size = sizeof(typename AllocatorConstants<T>::AllocatorType),
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "staging_buffer",
        });
        recorder.destroy_buffer_deferred(staging_buffer);
        auto *buffer_ptr = device.get_host_address_as<typename AllocatorConstants<T>::AllocatorType>(staging_buffer).value();
        *buffer_ptr = typename AllocatorConstants<T>::AllocatorType{
            .heap = device.get_device_address(element_buffer).value(),
            .available_element_stack = device.get_device_address(available_element_stack_buffer).value(),
            .released_element_stack = device.get_device_address(released_element_stack_buffer).value(),
        };
        recorder.copy_buffer_to_buffer({
            .src_buffer = staging_buffer,
            .dst_buffer = allocator_buffer,
            .size = offsetof(typename AllocatorConstants<T>::AllocatorType, element_count),
        });
    }
    void for_each_task_buffer(auto const &functor) {
        functor(task_allocator_buffer);
        functor(task_element_buffer);
        functor(task_old_element_buffer);
    }
    /// Grow the heap if the GPU is about to outrun it -- but never past the VRAM budget.
    ///
    /// THE INVARIANT THIS FUNCTION EXISTS TO MAINTAIN, because everything else follows from it:
    /// the GPU-side allocator (utilities/allocator.glsl) bumps `element_count` with an
    /// unchecked atomicAdd and writes into `heap[index]` immediately. Nothing on the GPU knows
    /// the buffer's size. The only thing keeping those writes in bounds is that capacity is
    /// always at least
    ///     (element count the CPU last saw) + MAX_ELEMENT_ALLOCATIONS_PER_FRAME * (FIF + 1)
    /// i.e. more than the GPU could possibly consume in the frames the CPU has not seen yet.
    /// `max_count_after_cpu_catch_up` below is exactly that quantity. Growing to at least it is
    /// a correctness requirement; the 1.5x on top is only an amortisation trick.
    void check_for_realloc(daxa::Device &device, size_t current_known_element_count) {
        if (!budget_logged) {
            budget_logged = true;
            debug_utils::Console::add_log(fmt::format(
                "[heap] {}: VRAM {:.0f} MB, budget {:.0f} MB, cap {} elements ({:.0f} MB at {} B/element)\n",
                AllocatorConstants<T>::element_buffer_name,
                static_cast<double>(gpu_heap_budget::device_local_bytes(device)) / 1'000'000.0,
                static_cast<double>(budget_bytes) / 1'000'000.0,
                max_element_count,
                static_cast<double>(capacity_limit_bytes()) / 1'000'000.0,
                TOTAL_BYTES_PER_ELEMENT));
        }

        // 64-bit throughout. The original computed `ELEM_SIZE_BYTES * current_element_count` in
        // daxa_u32 and handed the result straight to create_buffer(.size), which wraps above
        // 2 033 601 pages -- i.e. it would have silently created a *tiny* buffer at exactly the
        // page count the debug overlay also started lying at. Same root cause, worse symptom.
        auto const max_count_after_cpu_catch_up =
            static_cast<daxa_u64>(current_known_element_count) + PER_FRAME_HEADROOM;

        next_element_count = 0;
        if (max_count_after_cpu_catch_up <= static_cast<daxa_u64>(current_element_count)) {
            return; // capacity still covers the worst case; nothing to do
        }

        // Three quantities decide the new capacity.
        //  - geometric:  the original 1.5x step, which keeps reallocation O(log n).
        //  - minimum:    the smallest capacity that still satisfies the invariant above.
        //                Anything below this is a correctness failure, not a slow path.
        //  - affordable: the largest capacity whose buffers can be resident AT THE SAME TIME as
        //                the ones being replaced. realloc() copies between them, so both are
        //                live for a frame or two. Skipping this joint test is precisely the
        //                original bug: 2782 MiB of old heap + 4791 MiB of new on a 6144 MiB card.
        auto const geometric = std::max<daxa_u64>(
            (static_cast<daxa_u64>(current_element_count) + PER_FRAME_HEADROOM) * 3 / 2,
            max_count_after_cpu_catch_up);
        auto const minimum = max_count_after_cpu_catch_up;
        auto const budget_elements = budget_bytes / TOTAL_BYTES_PER_ELEMENT;
        auto const affordable = budget_elements > static_cast<daxa_u64>(current_element_count)
                                    ? budget_elements - static_cast<daxa_u64>(current_element_count)
                                    : daxa_u64{0};

        // Take as much as is allowed, in ONE step. Crawling to `minimum` repeatedly was measured
        // to be much worse than it sounds: every crawl still pays the full old+new transient, so
        // a 2 GB heap growing 277 MB at a time spends most of its frames at 4 GB resident.
        auto const chosen = std::min<daxa_u64>({geometric, affordable, static_cast<daxa_u64>(max_element_count)});

        if (chosen < geometric && chosen >= minimum && !growth_throttled) {
            growth_throttled = true;
            debug_utils::Console::add_log(fmt::format(
                "[heap] {}: 1.5x growth would not fit the {:.0f} MB budget; capped this step at {} elements ({:.0f} MB)\n",
                AllocatorConstants<T>::element_buffer_name,
                static_cast<double>(budget_bytes) / 1'000'000.0,
                chosen,
                static_cast<double>(chosen * TOTAL_BYTES_PER_ELEMENT) / 1'000'000.0));
        }

        if (chosen < minimum) {
            // DEGRADATION POLICY -- refuse the growth.
            //
            // Why refuse rather than allocate and hope: create_buffer() on a device with no room
            // either throws (Daxa turns VK_ERROR_OUT_OF_DEVICE_MEMORY into an exception) or, on
            // WDDM, succeeds by paging the heap to system RAM, at which point every voxel trace
            // walks over PCIe and the frame time collapses to single digits. Both outcomes are
            // worse than stopping, and neither is visible to the user. This is.
            //
            // What refusing costs, stated honestly: the invariant at the top of this function is
            // now broken, so a GPU that keeps allocating can write past the end of the heap. The
            // engine cannot currently be told to stop generating chunks from the CPU side -- the
            // chunk-edit dispatch is computed on the GPU in voxels/impl/perframe.comp.glsl. The
            // proper companion fix is a bounds check in utilities/allocator.glsl's malloc(); that
            // file has an owner and this is recorded as an integration note for them.
            //
            // What makes this survivable in the meantime is that the cap is set far above what
            // the shipped world can reach. Measured at CHUNKS_PER_AXIS 16: the heap settles at
            // 393216 pages against a cap of 1955198, with two further 1.5x growth steps still
            // affordable and no refusals in either a stationary or a 30 s moving run. A refusal
            // therefore means something has genuinely gone wrong -- and it is now loud instead
            // of silent. (Forced at CHUNKS_PER_AXIS 32 on an earlier build, 108 refusals in 30 s
            // still exited cleanly with code 0 and no visible corruption, but that is the
            // driver being forgiving, not this code being correct.)
            ++growth_refusals;
            if (growth_refusals == 1) {
                debug_utils::Console::add_log(fmt::format(
                    "[heap] {}: GROWTH REFUSED. {} elements ({:.0f} MB) is the cap; {} more would need "
                    "{:.0f} MB resident at once against a {:.0f} MB budget. The world is larger than this card.\n",
                    AllocatorConstants<T>::element_buffer_name,
                    current_element_count,
                    static_cast<double>(capacity_bytes()) / 1'000'000.0,
                    minimum - current_element_count,
                    static_cast<double>((static_cast<daxa_u64>(current_element_count) + minimum) * TOTAL_BYTES_PER_ELEMENT) / 1'000'000.0,
                    static_cast<double>(budget_bytes) / 1'000'000.0));
            }
            return; // next_element_count stays 0, so needs_realloc() is false
        }

        prev_element_count = current_element_count;
        current_element_count = static_cast<daxa_u32>(chosen);
        // next_element_count is only ever read through needs_realloc(); its value is unused.
        next_element_count = current_element_count;
        assert(current_element_count > prev_element_count);

        auto new_element_buffer = device.create_buffer({
            .size = static_cast<daxa_u64>(current_element_count) * ELEMENT_BYTES,
            .name = AllocatorConstants<T>::element_buffer_name,
        });
        auto new_available_element_stack_buffer = device.create_buffer({
            .size = static_cast<daxa_u64>(current_element_count) * sizeof(typename AllocatorConstants<T>::IndexType),
            .name = AllocatorConstants<T>::available_element_stack_buffer_name,
        });
        auto new_released_element_stack_buffer = device.create_buffer({
            .size = static_cast<daxa_u64>(current_element_count) * sizeof(typename AllocatorConstants<T>::IndexType),
            .name = AllocatorConstants<T>::released_element_stack_buffer_name,
        });
        task_old_element_buffer.swap_buffers(task_element_buffer);
        element_buffer = new_element_buffer;
        available_element_stack_buffer = new_available_element_stack_buffer;
        released_element_stack_buffer = new_released_element_stack_buffer;
        task_element_buffer.set_buffers({
            .buffers = std::array{
                element_buffer,
                available_element_stack_buffer,
                released_element_stack_buffer,
            },
        });
    }
    auto needs_realloc() -> bool {
        return next_element_count != 0;
    }
};

template <typename T>
struct StaticAllocatorBufferState {
    TemporalBuffer allocator_buffer;
    TemporalBuffer element_buffer;
    TemporalBuffer available_element_stack_buffer;
    TemporalBuffer released_element_stack_buffer;

    bool initialized = false;

    void init(GpuContext &gpu_context) {
        allocator_buffer = gpu_context.find_or_add_temporal_buffer({
            .size = sizeof(typename StaticAllocatorConstants<T>::AllocatorType),
            .name = StaticAllocatorConstants<T>::allocator_buffer_name,
        });
        element_buffer = gpu_context.find_or_add_temporal_buffer({
            .size = sizeof(typename StaticAllocatorConstants<T>::ElementType) * StaticAllocatorConstants<T>::MAX_ELEMENTS,
            .name = StaticAllocatorConstants<T>::element_buffer_name,
        });
        available_element_stack_buffer = gpu_context.find_or_add_temporal_buffer({
            .size = sizeof(typename StaticAllocatorConstants<T>::IndexType) * StaticAllocatorConstants<T>::MAX_ELEMENTS,
            .name = StaticAllocatorConstants<T>::available_element_stack_buffer_name,
        });
        released_element_stack_buffer = gpu_context.find_or_add_temporal_buffer({
            .size = sizeof(typename StaticAllocatorConstants<T>::IndexType) * StaticAllocatorConstants<T>::MAX_ELEMENTS,
            .name = StaticAllocatorConstants<T>::released_element_stack_buffer_name,
        });

        gpu_context.frame_task_graph.use_persistent_buffer(allocator_buffer.task_resource);
        gpu_context.frame_task_graph.use_persistent_buffer(element_buffer.task_resource);
        gpu_context.frame_task_graph.use_persistent_buffer(available_element_stack_buffer.task_resource);
        gpu_context.frame_task_graph.use_persistent_buffer(released_element_stack_buffer.task_resource);

        gpu_context.startup_task_graph.use_persistent_buffer(allocator_buffer.task_resource);
        gpu_context.startup_task_graph.use_persistent_buffer(element_buffer.task_resource);

        gpu_context.startup_task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, allocator_buffer.task_resource),
                daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, element_buffer.task_resource),
            },
            .task = [this](daxa::TaskInterface const &ti) {
                auto staging_buffer = ti.device.create_buffer({
                    .size = sizeof(typename StaticAllocatorConstants<T>::AllocatorType),
                    .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                    .name = "allocator_staging_buffer",
                });
                ti.recorder.destroy_buffer_deferred(staging_buffer);
                auto *buffer_ptr = ti.device.get_host_address_as<typename StaticAllocatorConstants<T>::AllocatorType>(staging_buffer).value();
                *buffer_ptr = typename StaticAllocatorConstants<T>::AllocatorType{
                    .heap = ti.device.get_device_address(element_buffer.resource_id).value(),
                    .available_element_stack = ti.device.get_device_address(available_element_stack_buffer.resource_id).value(),
                    .released_element_stack = ti.device.get_device_address(released_element_stack_buffer.resource_id).value(),
                    .element_count = 0,
                    .available_element_stack_size = 0,
                    .released_element_stack_size = 0,
                };
                ti.recorder.copy_buffer_to_buffer({
                    .src_buffer = staging_buffer,
                    .dst_buffer = allocator_buffer.resource_id,
                    .size = sizeof(typename StaticAllocatorConstants<T>::AllocatorType),
                });
                ti.recorder.clear_buffer({
                    .buffer = element_buffer.resource_id,
                    .size = sizeof(typename StaticAllocatorConstants<T>::ElementType) * StaticAllocatorConstants<T>::MAX_ELEMENTS,
                });
            },
            .name = "Allocator State Init",
        });
    }
};
#endif
