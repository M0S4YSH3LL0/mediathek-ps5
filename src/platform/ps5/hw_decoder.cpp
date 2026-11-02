// Mediathek - The console's hardware picture decoder (libSceVideodec2).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// There is no public documentation of this library. The structures, the order of the
// calls and the values come from what others proved on the hardware: EVO Player
// (https://github.com/sainsaji/EVO-PLAYER-PS5), ps5-hardware-video-decoding-research
// (https://github.com/JMUtechnologies/ps5-hardware-video-decoding-research), both
// GPL-3.0-or-later, and the ps5vdec module of VLC's PS5 port
// (https://github.com/ZiZc3/VLC, GPL-2.0-or-later), whose console runs found the rules
// for its memory, its picture buffers and its levels noted below. Field names nobody
// could explain stay "reserved".

#include "platform/ps5/hw_decoder.hpp"

#include "platform/ps5/system.hpp"

#include <pthread.h>

#include <atomic>
#include <cstring>

namespace hui::ps5
{

namespace
{

// Every structure starts with its own size, and a wrong one is refused.
struct ConfigInfo
{
    std::uint64_t size;
    std::uint32_t resource_type;
    std::uint32_t codec_type;
    std::uint32_t profile;
    std::uint32_t max_level;
    std::int32_t max_width;
    std::int32_t max_height;
    std::int32_t max_dpb_frames;
    std::uint32_t pipeline_depth;
    std::uint64_t compute_queue;
    std::uint64_t cpu_affinity;
    std::int32_t cpu_priority;
    std::uint32_t optimize_progressive;
    std::uint32_t check_memory_type;
    std::uint32_t reserved;
};
static_assert(sizeof(ConfigInfo) == 0x48);

struct MemoryInfo
{
    std::uint64_t size;
    std::uint64_t cpu_size;
    void *cpu;
    std::uint64_t gpu_size;
    void *gpu;
    std::uint64_t cpu_gpu_size;
    void *cpu_gpu;
    std::uint64_t max_frame_size;
    std::uint32_t frame_alignment;
    std::uint32_t reserved;
};
static_assert(sizeof(MemoryInfo) == 0x48);

struct ComputeConfig
{
    std::uint64_t size;
    std::uint16_t pipe_id;
    std::uint16_t queue_id;
    std::uint8_t check_memory_type;
    std::uint8_t reserved0;
    std::uint16_t reserved1;
};
static_assert(sizeof(ComputeConfig) == 0x10);

struct ComputeMemory
{
    std::uint64_t size;
    std::uint64_t cpu_gpu_size;
    void *cpu_gpu;
};
static_assert(sizeof(ComputeMemory) == 0x18);

struct InputData
{
    std::uint64_t size;
    void *unit;
    std::uint64_t unit_size;
    std::uint64_t pts;
    std::uint64_t dts;
    std::uint64_t attached;
};
static_assert(sizeof(InputData) == 0x30);

struct FrameBuffer
{
    std::uint64_t size;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t accepted;
    std::uint32_t reserved;
};
static_assert(sizeof(FrameBuffer) == 0x20);

struct OutputInfo
{
    std::uint64_t size;
    std::uint8_t valid;
    std::uint8_t error;
    std::uint8_t picture_count;
    std::uint8_t padding;
    std::uint32_t codec;
    std::uint32_t width;
    std::uint32_t pitch;
    std::uint32_t height;
    std::uint32_t reserved;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t frame_format;
    std::uint32_t pitch_bytes;
};
static_assert(sizeof(OutputInfo) == 0x38);

} // namespace

extern "C"
{
    int sceSysmoduleLoadModule(std::uint16_t id);
    std::int64_t sceKernelGetDirectMemorySize(void);
    int sceKernelAllocateDirectMemory(std::int64_t from, std::int64_t to, std::size_t size,
                                      std::size_t alignment, int type, std::int64_t *start);
    int sceKernelMapDirectMemory(void **address, std::size_t size, int protection, int flags,
                                 std::int64_t start, std::size_t alignment);
    int sceKernelReleaseDirectMemory(std::int64_t start, std::size_t size);
    int sceKernelMunmap(void *address, std::size_t size);
    int sceKernelMapNamedFlexibleMemory(void **address, std::size_t size, int protection, int flags,
                                        const char *name);

    int sceVideodec2QueryComputeMemoryInfo(ComputeMemory *memory);
    int sceVideodec2AllocateComputeQueue(const ComputeConfig *config, const ComputeMemory *memory,
                                         void **queue);
    int sceVideodec2QueryDecoderMemoryInfo(const ConfigInfo *config, MemoryInfo *memory);
    int sceVideodec2CreateDecoder(const ConfigInfo *config, const MemoryInfo *memory,
                                  void **decoder);
    int sceVideodec2DeleteDecoder(void *decoder);
    int sceVideodec2Reset(void *decoder);
    int sceVideodec2Decode(void *decoder, InputData *input, FrameBuffer *frame, OutputInfo *output);
    int sceVideodec2Flush(void *decoder, FrameBuffer *frame, OutputInfo *output);
}

namespace
{

constexpr std::uint16_t kSysmoduleVideodec2 = 207;
constexpr std::uint32_t kCodecH264 = 1;
constexpr std::uint32_t kCodecHevc = 974921;
constexpr std::uint32_t kResourceCompute = 1;
constexpr int kDirectType = 12;
constexpr std::size_t kAlignment = 0x4000;
constexpr int kCpuAndGraphics = 0x33;
constexpr int kCpuOnly = 0x03;
constexpr std::size_t kInputBytes = std::size_t{8} << 20; // far above any access unit seen
// The decoder may still be reading a unit after Decode returns (a big picture): three
// slots in turn broke a 4K film at its first big key frame in VLC's port, eight did not.
constexpr std::size_t kInputSlots = 8;
// The decoder keeps the buffer it was handed until that picture's turn to be shown, and
// the player keeps it until it has copied it out. Each buffer is tracked (Slot): handed
// out in turn regardless, VLC's port saw them written over while waiting (green blocks).
constexpr int kFrameSlots = 16;
constexpr std::uint32_t kDepthWanted = 4; // the decoder works on this many at once
constexpr std::uint32_t kDepthLeast = 1;
constexpr int kReportedFailures = 4; // of each kind, so the log stays readable
// The decoder's own working memory comes from flexible memory, which given back does
// not come back (VLC's port lost 600 MiB over eight 4K films and then had none): one
// block is kept for the app's life and handed to one decoder after another. 96 MiB
// covers 4K HEVC's 76.
constexpr std::size_t kCpuBlockLeast = std::size_t{96} << 20;
// Stream errors: a new or invalid sequence, a bad access unit.
constexpr unsigned kStreamErrorFirst = 0x811D0300u;
constexpr unsigned kStreamErrorLast = 0x811D0304u;
// CreateDecoder failing with these is the chip itself (its resources, its state), not
// the configuration: another configuration does no better.
constexpr unsigned kChipErrorMask = 0xFFFFFF00u;
constexpr unsigned kChipError = 0x811D0100u;

enum Slot : std::uint8_t
{
    kFree,
    kWithDecoder,
    kWithPlayer
};

std::size_t aligned(std::uint64_t size)
{
    return (static_cast<std::size_t>(size) + kAlignment - 1) & ~(kAlignment - 1);
}

// Half of these calls are the system's code, and a bad value can take the process with
// it rather than return. Each step is named before it is entered, with the time the
// step before took: the last line in the console's log says what killed the app, and
// the times say what kept a film's first picture waiting.
std::int64_t g_step_began = 0;

void step(const char *what)
{
    const std::int64_t now = sys::monotonic_us();
    sys::log("[HUI] hw decoder: %s (previous step %lld ms)", what,
             static_cast<long long>(g_step_began != 0 ? (now - g_step_began) / 1000 : 0));
    g_step_began = now;
}

struct DirectBlock
{
    void *address = nullptr;
    std::int64_t start = -1;
    std::size_t size = 0;

    int allocate(std::size_t bytes)
    {
        const std::int64_t limit = sceKernelGetDirectMemorySize();
        int code = sceKernelAllocateDirectMemory(0, limit, bytes, kAlignment, kDirectType, &start);
        if (code != 0)
        {
            start = -1;
            return code;
        }
        size = bytes;
        code = sceKernelMapDirectMemory(&address, bytes, kCpuAndGraphics, 0, start, kAlignment);
        if (code != 0)
        {
            address = nullptr;
            release();
        }
        return code;
    }

    void release()
    {
        if (address != nullptr)
            sceKernelMunmap(address, size);
        if (start >= 0)
            sceKernelReleaseDirectMemory(start, size);
        address = nullptr;
        start = -1;
        size = 0;
    }
};

// What every decoder shares, for the app's life.
struct Shared
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    bool prepared = false;
    void *queue = nullptr;
    DirectBlock queue_memory;
    void *cpu_block = nullptr;
    std::size_t cpu_block_size = 0;
    bool cpu_block_busy = false;
};

Shared g_shared;

class SharedLock
{
  public:
    SharedLock()
    {
        pthread_mutex_lock(&g_shared.mutex);
    }
    ~SharedLock()
    {
        pthread_mutex_unlock(&g_shared.mutex);
    }
    SharedLock(const SharedLock &) = delete;
    SharedLock &operator=(const SharedLock &) = delete;
};

// The decoder's working memory: the shared block when it is free and big enough (a
// bigger one replaces it, the old one stays lost, once), its own while a second
// decoder still runs. `shared` says which, for give_cpu_memory().
void *take_cpu_memory(std::size_t size, bool *shared)
{
    {
        SharedLock lock;
        if (!g_shared.cpu_block_busy)
        {
            if (size > g_shared.cpu_block_size)
            {
                const std::size_t want = size > kCpuBlockLeast ? size : kCpuBlockLeast;
                void *address = nullptr;
                if (sceKernelMapNamedFlexibleMemory(&address, want, kCpuOnly, 0, "MediathekVdec") ==
                    0)
                {
                    if (g_shared.cpu_block != nullptr)
                        sys::log("[HUI] hw decoder: working memory grown %zu -> %zu MiB",
                                 g_shared.cpu_block_size >> 20, want >> 20);
                    g_shared.cpu_block = address;
                    g_shared.cpu_block_size = want;
                }
            }
            if (g_shared.cpu_block != nullptr && size <= g_shared.cpu_block_size)
            {
                g_shared.cpu_block_busy = true;
                *shared = true;
                return g_shared.cpu_block;
            }
        }
    }
    *shared = false;
    void *address = nullptr;
    if (sceKernelMapNamedFlexibleMemory(&address, size, kCpuOnly, 0, "MediathekVdec2") != 0)
        return nullptr;
    return address;
}

void give_cpu_memory(void *address, std::size_t size, bool shared)
{
    if (address == nullptr)
        return;
    if (shared)
    {
        SharedLock lock;
        g_shared.cpu_block_busy = false;
        return;
    }
    sceKernelMunmap(address, size);
}

} // namespace

struct HwDecoder::Parts
{
    void *decoder = nullptr;
    void *cpu = nullptr;
    std::size_t cpu_size = 0;
    bool cpu_shared = false;
    DirectBlock gpu;
    DirectBlock cpu_gpu;
    DirectBlock input;
    DirectBlock frames;
    std::size_t frame_size = 0;
    std::atomic<std::uint8_t> slots[kFrameSlots] = {};
    ConfigInfo config{};
    MemoryInfo memory{};
    std::size_t input_at = 0;
    int reported = 0;
    bool first_picture = true;
    bool chip_refused = false;

    ~Parts()
    {
        if (decoder != nullptr)
            sceVideodec2DeleteDecoder(decoder);
        frames.release();
        input.release();
        cpu_gpu.release();
        gpu.release();
        give_cpu_memory(cpu, cpu_size, cpu_shared);
    }

    // Zero when it all worked; otherwise the system's code, and `failed` says at what.
    int build(const media::HwMode &mode, unsigned level, std::uint32_t depth, const char **failed)
    {
        config = ConfigInfo{};
        config.size = sizeof(config);
        config.resource_type = kResourceCompute;
        config.codec_type = mode.codec == media::HwCodec::hevc ? kCodecHevc : kCodecH264;
        config.profile = mode.profile;
        config.max_level = level;
        config.max_width = mode.max_width;
        config.max_height = mode.max_height;
        config.max_dpb_frames = -1; // the decoder sizes it itself
        config.pipeline_depth = depth;
        config.compute_queue = reinterpret_cast<std::uint64_t>(g_shared.queue);
        config.cpu_affinity = 0x3f;
        config.cpu_priority = 700;
        config.optimize_progressive = 1;
        memory = MemoryInfo{};
        memory.size = sizeof(memory);
        sys::log("[HUI] hw decoder: config codec=%u profile=%u level=%u max=%dx%d depth=%u",
                 config.codec_type, config.profile, config.max_level, config.max_width,
                 config.max_height, config.pipeline_depth);
        *failed = "QueryDecoderMemoryInfo";
        step(*failed);
        int code = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
        if (code != 0)
            return code;
        sys::log("[HUI] hw decoder: wants cpu=%llu gpu=%llu cpu_gpu=%llu frame=%llu",
                 static_cast<unsigned long long>(memory.cpu_size),
                 static_cast<unsigned long long>(memory.gpu_size),
                 static_cast<unsigned long long>(memory.cpu_gpu_size),
                 static_cast<unsigned long long>(memory.max_frame_size));

        *failed = "working memory";
        step(*failed);
        cpu_size = aligned(memory.cpu_size);
        cpu = take_cpu_memory(cpu_size, &cpu_shared);
        if (cpu == nullptr)
            return -1;
        memory.cpu = cpu;
        *failed = "decoder memory";
        step(*failed);
        code = gpu.allocate(aligned(memory.gpu_size));
        if (code != 0)
            return code;
        memory.gpu = gpu.address;
        memory.gpu_size = gpu.size;
        if (memory.cpu_gpu_size != 0)
        {
            code = cpu_gpu.allocate(aligned(memory.cpu_gpu_size));
            if (code != 0)
                return code;
            memory.cpu_gpu = cpu_gpu.address;
            memory.cpu_gpu_size = cpu_gpu.size;
        }
        *failed = "access units";
        code = input.allocate(kInputBytes * kInputSlots);
        if (code != 0)
            return code;
        *failed = "picture memory";
        const std::size_t frame_alignment =
            memory.frame_alignment > kAlignment ? memory.frame_alignment : kAlignment;
        frame_size = (static_cast<std::size_t>(memory.max_frame_size) + frame_alignment - 1) &
                     ~(frame_alignment - 1);
        code = frames.allocate(frame_size * kFrameSlots);
        if (code != 0)
            return code;
        clear(mode);

        *failed = "CreateDecoder";
        step(*failed);
        code = sceVideodec2CreateDecoder(&config, &memory, &decoder);
        if (code != 0)
        {
            decoder = nullptr;
            chip_refused = (static_cast<unsigned>(code) & kChipErrorMask) == kChipError;
            return code;
        }
        *failed = "Reset";
        step(*failed);
        return sceVideodec2Reset(decoder);
    }

    // The memory as new: direct memory and the reused working block come back as they
    // were left, and a picture the chip did not fully decode showed a moment of the film
    // before (VLC's port, only after a different film). Pictures mid-grey, not green.
    void clear(const media::HwMode &mode)
    {
        step("clearing memory");
        std::memset(cpu, 0, cpu_size);
        std::memset(gpu.address, 0, gpu.size);
        if (cpu_gpu.address != nullptr)
            std::memset(cpu_gpu.address, 0, cpu_gpu.size);
        if (mode.codec == media::HwCodec::hevc && mode.profile == 2)
        {
            auto *words = static_cast<std::uint64_t *>(frames.address);
            const std::size_t count = frames.size / sizeof(std::uint64_t);
            for (std::size_t i = 0; i < count; ++i)
                words[i] = 0x0200020002000200ull; // 512 of 1023, low-aligned
        }
        else
        {
            std::memset(frames.address, 0x80, frames.size);
        }
    }

    // A buffer the decoder may write: -1 when every one is held.
    int free_slot() const
    {
        for (int i = 0; i < kFrameSlots; ++i)
            if (slots[i].load(std::memory_order_acquire) == kFree)
                return i;
        return -1;
    }

    void describe(int slot, FrameBuffer *frame) const
    {
        std::memset(frame, 0, sizeof(*frame));
        frame->size = sizeof(*frame);
        frame->buffer = static_cast<std::uint8_t *>(frames.address) +
                        static_cast<std::size_t>(slot) * frame_size;
        frame->buffer_size = frame_size;
    }

    // The buffer a picture came back in, or -1 if it is none of ours.
    int slot_of(const void *buffer) const
    {
        const auto *base = static_cast<const std::uint8_t *>(frames.address);
        const auto *start = static_cast<const std::uint8_t *>(buffer);
        if (start == nullptr || start < base || start >= base + frames.size || frame_size == 0)
            return -1;
        return static_cast<int>(static_cast<std::size_t>(start - base) / frame_size);
    }

    // Everything below rests on this picture: a pointer or a size off is a crash in the
    // copy, far from here. The rows must fit in the buffer the picture is in.
    bool sane(const OutputInfo &output, int slot) const
    {
        const std::size_t pitch = output.pitch_bytes != 0 ? output.pitch_bytes : output.pitch;
        const std::size_t bytes = pitch * output.height * 3 / 2;
        const auto *buffer = static_cast<const std::uint8_t *>(output.buffer);
        const auto *slot_start = static_cast<const std::uint8_t *>(frames.address) +
                                 static_cast<std::size_t>(slot) * frame_size;
        return output.width > 0 && output.height > 0 && pitch >= output.width &&
               buffer + bytes <= slot_start + frame_size;
    }
};

bool HwDecoder::prepare()
{
    SharedLock lock;
    if (g_shared.prepared)
        return g_shared.queue != nullptr;
    g_shared.prepared = true;
    const std::int64_t began = sys::monotonic_us();
    g_step_began = began;
    const int module = sceSysmoduleLoadModule(kSysmoduleVideodec2);
    sys::log("[HUI] hw decoder: module load 0x%08x", static_cast<unsigned>(module));
    ComputeMemory compute_memory{};
    compute_memory.size = sizeof(compute_memory);
    step("QueryComputeMemoryInfo");
    int code = sceVideodec2QueryComputeMemoryInfo(&compute_memory);
    if (code == 0)
        code = g_shared.queue_memory.allocate(aligned(compute_memory.cpu_gpu_size));
    if (code == 0)
    {
        compute_memory.cpu_gpu = g_shared.queue_memory.address;
        compute_memory.cpu_gpu_size = g_shared.queue_memory.size;
        ComputeConfig compute_config{};
        compute_config.size = sizeof(compute_config);
        step("AllocateComputeQueue");
        code = sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory, &g_shared.queue);
        if (code != 0)
        {
            g_shared.queue = nullptr;
            g_shared.queue_memory.release();
        }
    }
    sys::log("[HUI] hw decoder: compute queue %s (0x%08x, %zu KiB) in %lld ms",
             g_shared.queue != nullptr ? "ready" : "FAILED", static_cast<unsigned>(code),
             g_shared.queue_memory.size >> 10,
             static_cast<long long>((sys::monotonic_us() - began) / 1000));
    return g_shared.queue != nullptr;
}

std::unique_ptr<HwDecoder> HwDecoder::open(const media::HwMode &mode)
{
    if (!prepare())
        return nullptr;
    const std::int64_t began = sys::monotonic_us();
    g_step_began = began;
    // The deeper pipeline keeps the decoder busy between calls (one at a time decoded a
    // 4K high-tier film at 20 pictures a second in VLC's port) but costs memory; the
    // higher level is for the fastest films. The lighter settings follow in that order.
    const unsigned levels[] = {mode.level, mode.fallback_level};
    for (const unsigned level : levels)
    {
        if (level == 0)
            continue;
        for (const std::uint32_t depth : {kDepthWanted, kDepthLeast})
        {
            auto parts = std::make_unique<Parts>();
            const char *failed = "?";
            const int code = parts->build(mode, level, depth, &failed);
            if (code == 0)
            {
                sys::log("[HUI] hw decoder: ready at level %u depth %u in %lld ms", level, depth,
                         static_cast<long long>((sys::monotonic_us() - began) / 1000));
                return std::unique_ptr<HwDecoder>(new HwDecoder(std::move(parts)));
            }
            sys::log("[HUI] hw decoder: level %u depth %u failed at %s: 0x%08x", level, depth,
                     failed, static_cast<unsigned>(code));
            if (parts->chip_refused)
                return nullptr;
        }
    }
    return nullptr;
}

HwDecoder::HwDecoder(std::unique_ptr<Parts> parts) : parts_(std::move(parts))
{
}

HwDecoder::~HwDecoder() = default;

std::uint8_t *HwDecoder::next_input()
{
    return static_cast<std::uint8_t *>(parts_->input.address) + parts_->input_at * kInputBytes;
}

std::size_t HwDecoder::input_capacity()
{
    return kInputBytes;
}

HwDecoder::Result HwDecoder::finish(int code, int slot, const void *frame, const void *output_info,
                                    HwPicture *picture)
{
    Parts &parts = *parts_;
    const auto &given = *static_cast<const FrameBuffer *>(frame);
    const auto &output = *static_cast<const OutputInfo *>(output_info);
    if (given.accepted != 0)
        parts.slots[slot].store(kWithDecoder, std::memory_order_release);
    if ((code != 0 || output.error != 0) && parts.reported < kReportedFailures)
    {
        ++parts.reported;
        sys::log("[HUI] hw decoder: code 0x%08x error=%u valid=%u accepted=%u %ux%u",
                 static_cast<unsigned>(code), output.error, output.valid, given.accepted,
                 output.width, output.height);
    }
    if (code != 0)
    {
        const auto value = static_cast<unsigned>(code);
        return value >= kStreamErrorFirst && value <= kStreamErrorLast ? Result::broken
                                                                       : Result::refused;
    }
    if (output.valid == 0 || output.picture_count == 0 || output.buffer == nullptr)
        return Result::nothing;
    const int out = parts.slot_of(output.buffer);
    if (out < 0 || !parts.sane(output, out))
    {
        sys::log("[HUI] hw decoder: picture out of bounds %ux%u pitch=%u/%u", output.width,
                 output.height, output.pitch, output.pitch_bytes);
        if (out >= 0)
            parts.slots[out].store(kFree, std::memory_order_release);
        return Result::refused;
    }
    if (output.error != 0)
    {
        // Its buffer is the decoder's no more, and there is nothing in it to show.
        parts.slots[out].store(kFree, std::memory_order_release);
        return Result::nothing;
    }
    parts.slots[out].store(kWithPlayer, std::memory_order_release);
    const std::size_t pitch = output.pitch_bytes != 0 ? output.pitch_bytes : output.pitch;
    picture->luma = static_cast<const std::uint8_t *>(output.buffer);
    picture->chroma = picture->luma + pitch * output.height;
    picture->pitch = pitch;
    picture->width = static_cast<int>(output.width);
    picture->height = static_cast<int>(output.height);
    picture->wide = output.pitch_bytes != 0 && output.pitch_bytes >= output.pitch * 2;
    picture->slot = out;
    if (parts.first_picture)
    {
        parts.first_picture = false;
        sys::log("[HUI] hw decoder: first picture %ux%u pitch=%u/%u format=%u count=%u",
                 output.width, output.height, output.pitch, output.pitch_bytes, output.frame_format,
                 output.picture_count);
    }
    return Result::picture;
}

HwDecoder::Result HwDecoder::decode(std::size_t size, HwPicture *picture)
{
    Parts &parts = *parts_;
    const int slot = parts.free_slot();
    if (slot < 0)
        return Result::no_room;
    InputData input{};
    input.size = sizeof(input);
    input.unit = next_input();
    input.unit_size = size;
    input.pts = parts.input_at; // the output carries no time; PendingStamps pairs them up
    input.dts = UINT64_MAX;
    parts.input_at = (parts.input_at + 1) % kInputSlots;
    FrameBuffer frame;
    parts.describe(slot, &frame);
    OutputInfo output{};
    output.size = sizeof(output);
    const int code = sceVideodec2Decode(parts.decoder, &input, &frame, &output);
    return finish(code, slot, &frame, &output, picture);
}

HwDecoder::Result HwDecoder::drain(HwPicture *picture)
{
    const int slot = parts_->free_slot();
    if (slot < 0)
        return Result::no_room;
    FrameBuffer frame;
    parts_->describe(slot, &frame);
    OutputInfo output{};
    output.size = sizeof(output);
    const int code = sceVideodec2Flush(parts_->decoder, &frame, &output);
    return finish(code, slot, &frame, &output, picture);
}

void HwDecoder::release(const HwPicture &picture)
{
    if (picture.slot >= 0 && picture.slot < kFrameSlots)
        parts_->slots[picture.slot].store(kFree, std::memory_order_release);
}

bool HwDecoder::has_room() const
{
    return parts_->free_slot() >= 0;
}

bool HwDecoder::can_free_room() const
{
    for (const auto &slot : parts_->slots)
        if (slot.load(std::memory_order_acquire) != kWithDecoder)
            return true;
    return false;
}

bool HwDecoder::renew()
{
    Parts &parts = *parts_;
    if (parts.decoder != nullptr)
        sceVideodec2DeleteDecoder(parts.decoder);
    parts.decoder = nullptr;
    for (auto &slot : parts.slots)
    {
        std::uint8_t held = kWithDecoder;
        slot.compare_exchange_strong(held, kFree, std::memory_order_acq_rel);
    }
    MemoryInfo memory = parts.memory;
    int code = sceVideodec2CreateDecoder(&parts.config, &memory, &parts.decoder);
    if (code == 0)
        code = sceVideodec2Reset(parts.decoder);
    if (code != 0)
    {
        sys::log("[HUI] hw decoder: renew failed 0x%08x", static_cast<unsigned>(code));
        if (parts.decoder != nullptr)
            sceVideodec2DeleteDecoder(parts.decoder);
        parts.decoder = nullptr;
        return false;
    }
    return true;
}

} // namespace hui::ps5
