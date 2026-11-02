// Mediathek - The console's hardware picture decoder (libSceVideodec2).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// There is no public documentation of this library. The structures, the order of the
// calls and the values come from what others proved on the hardware: EVO Player
// (https://github.com/sainsaji/EVO-PLAYER-PS5) and ps5-hardware-video-decoding-research
// (https://github.com/JMUtechnologies/ps5-hardware-video-decoding-research), both
// GPL-3.0-or-later. Field names they could not explain stay "reserved".

#include "platform/ps5/hw_decoder.hpp"

#include "platform/ps5/system.hpp"

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

struct ComputeConfig
{
    std::uint64_t size;
    std::uint16_t pipe_id;
    std::uint16_t queue_id;
    std::uint8_t check_memory_type;
    std::uint8_t reserved0;
    std::uint16_t reserved1;
};

struct ComputeMemory
{
    std::uint64_t size;
    std::uint64_t cpu_gpu_size;
    void *cpu_gpu;
};

struct InputData
{
    std::uint64_t size;
    void *unit;
    std::uint64_t unit_size;
    std::uint64_t pts;
    std::uint64_t dts;
    std::uint64_t attached;
};

struct FrameBuffer
{
    std::uint64_t size;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t accepted;
    std::uint32_t reserved;
};

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
    int sceKernelReleaseFlexibleMemory(void *address, std::size_t size);

    int sceVideodec2QueryComputeMemoryInfo(ComputeMemory *memory);
    int sceVideodec2AllocateComputeQueue(const ComputeConfig *config, const ComputeMemory *memory,
                                         void **queue);
    int sceVideodec2ReleaseComputeQueue(void *queue);
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
constexpr std::size_t kInputSlots = 8;                    // more than the pipeline holds
// A slot must outlive the pipeline's depth, the player's wait for the converter and
// whatever the decoder keeps back in display order, and direct memory is plentiful.
constexpr std::size_t kFrameSlots = 16;
constexpr std::uint32_t kDepthWanted = 4; // the decoder works on this many at once
constexpr std::uint32_t kDepthLeast = 1;
constexpr int kReportedFailures = 4; // of each kind, so the log stays readable

std::size_t aligned(std::uint64_t size)
{
    return (static_cast<std::size_t>(size) + kAlignment - 1) & ~(kAlignment - 1);
}

struct DirectBlock
{
    void *address = nullptr;
    std::int64_t start = -1;
    std::size_t size = 0;

    int allocate(std::size_t bytes, std::int64_t limit)
    {
        int code = sceKernelAllocateDirectMemory(0, limit, bytes, kAlignment, kDirectType, &start);
        if (code != 0)
        {
            start = -1;
            return code;
        }
        size = bytes;
        return sceKernelMapDirectMemory(&address, bytes, kCpuAndGraphics, 0, start, kAlignment);
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

// Half of these calls are the system's code, and a bad value can take the process with
// it rather than return. Each step is named before it is entered: the last line in the
// console's log says what killed the app.
void step(const char *what)
{
    sys::log("[HUI] hw decoder: %s", what);
}

} // namespace

struct HwDecoder::Parts
{
    DirectBlock compute;
    void *queue = nullptr;
    void *decoder = nullptr;
    void *cpu = nullptr;
    std::size_t cpu_size = 0;
    DirectBlock gpu;
    DirectBlock cpu_gpu;
    DirectBlock input;
    DirectBlock frames;
    std::size_t frame_size = 0;
    ConfigInfo config{};
    MemoryInfo memory{};
    std::size_t input_at = 0;
    std::size_t frame_at = 0;
    int reported = 0;
    bool first_picture = true;

    ~Parts()
    {
        if (decoder != nullptr)
            sceVideodec2DeleteDecoder(decoder);
        frames.release();
        input.release();
        cpu_gpu.release();
        gpu.release();
        if (cpu != nullptr)
        {
            sceKernelReleaseFlexibleMemory(cpu, cpu_size);
            sceKernelMunmap(cpu, cpu_size);
        }
        if (queue != nullptr)
            sceVideodec2ReleaseComputeQueue(queue);
        compute.release();
    }

    // Zero when it all worked; otherwise the system's code, and `failed` says at what.
    int build(const media::HwMode &mode, std::uint32_t depth, const char **failed)
    {
        const std::int64_t limit = sceKernelGetDirectMemorySize();
        ComputeMemory compute_memory{};
        compute_memory.size = sizeof(compute_memory);
        *failed = "QueryComputeMemoryInfo";
        step(*failed);
        int code = sceVideodec2QueryComputeMemoryInfo(&compute_memory);
        if (code != 0)
            return code;
        *failed = "compute memory";
        code = compute.allocate(aligned(compute_memory.cpu_gpu_size), limit);
        if (code != 0)
            return code;
        compute_memory.cpu_gpu = compute.address;
        compute_memory.cpu_gpu_size = compute.size;
        ComputeConfig compute_config{};
        compute_config.size = sizeof(compute_config);
        *failed = "AllocateComputeQueue";
        step(*failed);
        code = sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory, &queue);
        if (code != 0)
            return code;

        config = ConfigInfo{};
        config.size = sizeof(config);
        config.resource_type = kResourceCompute;
        config.codec_type = mode.codec == media::HwCodec::hevc ? kCodecHevc : kCodecH264;
        config.profile = mode.profile;
        config.max_level = mode.level;
        config.max_width = mode.max_width;
        config.max_height = mode.max_height;
        config.max_dpb_frames = -1; // the decoder sizes it itself
        config.pipeline_depth = depth;
        config.compute_queue = reinterpret_cast<std::uint64_t>(queue);
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
        code = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
        if (code != 0)
            return code;
        sys::log("[HUI] hw decoder: wants cpu=%llu gpu=%llu cpu_gpu=%llu frame=%llu",
                 static_cast<unsigned long long>(memory.cpu_size),
                 static_cast<unsigned long long>(memory.gpu_size),
                 static_cast<unsigned long long>(memory.cpu_gpu_size),
                 static_cast<unsigned long long>(memory.max_frame_size));

        *failed = "flexible memory";
        cpu_size = aligned(memory.cpu_size);
        code = sceKernelMapNamedFlexibleMemory(&cpu, cpu_size, kCpuOnly, 0, "MediathekVdec");
        if (code != 0)
        {
            cpu = nullptr;
            return code;
        }
        memory.cpu = cpu;
        *failed = "decoder memory";
        code = gpu.allocate(aligned(memory.gpu_size), limit);
        if (code != 0)
            return code;
        memory.gpu = gpu.address;
        memory.gpu_size = gpu.size;
        if (memory.cpu_gpu_size != 0)
        {
            code = cpu_gpu.allocate(aligned(memory.cpu_gpu_size), limit);
            if (code != 0)
                return code;
            memory.cpu_gpu = cpu_gpu.address;
            memory.cpu_gpu_size = cpu_gpu.size;
        }
        *failed = "access units";
        code = input.allocate(kInputBytes * kInputSlots, limit);
        if (code != 0)
            return code;
        *failed = "picture memory";
        frame_size = aligned(memory.max_frame_size);
        code = frames.allocate(frame_size * kFrameSlots, limit);
        if (code != 0)
            return code;

        *failed = "CreateDecoder";
        step(*failed);
        code = sceVideodec2CreateDecoder(&config, &memory, &decoder);
        if (code != 0)
        {
            decoder = nullptr;
            return code;
        }
        *failed = "Reset";
        step(*failed);
        return sceVideodec2Reset(decoder);
    }

    void next_frame(FrameBuffer *frame)
    {
        std::memset(frame, 0, sizeof(*frame));
        frame->size = sizeof(*frame);
        frame->buffer = static_cast<std::uint8_t *>(frames.address) + frame_at * frame_size;
        frame->buffer_size = frame_size;
        frame_at = (frame_at + 1) % kFrameSlots;
    }

    // Everything below rests on this picture: a pointer or a size off is a crash in the
    // converter, far from here. The rows must fit in the memory the decoder was given.
    bool sane(const OutputInfo &output) const
    {
        const auto *base = static_cast<const std::uint8_t *>(frames.address);
        const auto *start = static_cast<const std::uint8_t *>(output.buffer);
        if (start < base || start >= base + frames.size)
            return false;
        const std::size_t pitch = output.pitch_bytes != 0 ? output.pitch_bytes : output.pitch;
        const std::size_t bytes = pitch * output.height * 3 / 2;
        return output.width > 0 && output.height > 0 && pitch >= output.width &&
               bytes <= frame_size && start + bytes <= base + frames.size;
    }
};

std::unique_ptr<HwDecoder> HwDecoder::open(const media::HwMode &mode)
{
    static const int module = sceSysmoduleLoadModule(kSysmoduleVideodec2);
    sys::log("[HUI] hw decoder: module load 0x%08x", static_cast<unsigned>(module));
    // The deeper pipeline takes more of the flexible memory, which is the scarce one.
    for (const std::uint32_t depth : {kDepthWanted, kDepthLeast})
    {
        auto parts = std::make_unique<Parts>();
        const char *failed = "?";
        const int code = parts->build(mode, depth, &failed);
        if (code == 0)
        {
            sys::log("[HUI] hw decoder: ready");
            return std::unique_ptr<HwDecoder>(new HwDecoder(std::move(parts)));
        }
        sys::log("[HUI] hw decoder: depth %u failed at %s: 0x%08x", depth, failed,
                 static_cast<unsigned>(code));
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

HwDecoder::Result HwDecoder::finish(int code, const void *frame, const void *output_info,
                                    HwPicture *picture)
{
    const auto &given = *static_cast<const FrameBuffer *>(frame);
    const auto &output = *static_cast<const OutputInfo *>(output_info);
    if (code != 0 || output.error != 0)
    {
        if (parts_->reported < kReportedFailures)
        {
            ++parts_->reported;
            sys::log("[HUI] hw decoder: code 0x%08x error=%u valid=%u accepted=%u %ux%u",
                     static_cast<unsigned>(code), output.error, output.valid, given.accepted,
                     output.width, output.height);
        }
    }
    if (code != 0)
        return Result::refused;
    if (output.valid == 0 || output.picture_count == 0 || output.buffer == nullptr)
        return Result::nothing;
    if (!parts_->sane(output))
    {
        sys::log("[HUI] hw decoder: picture out of bounds %ux%u pitch=%u/%u", output.width,
                 output.height, output.pitch, output.pitch_bytes);
        return Result::refused;
    }
    const std::size_t pitch = output.pitch_bytes != 0 ? output.pitch_bytes : output.pitch;
    picture->luma = static_cast<const std::uint8_t *>(output.buffer);
    picture->chroma = picture->luma + pitch * output.height;
    picture->pitch = pitch;
    picture->width = static_cast<int>(output.width);
    picture->height = static_cast<int>(output.height);
    picture->wide = output.pitch_bytes != 0 && output.pitch_bytes >= output.pitch * 2;
    if (parts_->first_picture)
    {
        parts_->first_picture = false;
        sys::log("[HUI] hw decoder: first picture %ux%u pitch=%u/%u format=%u count=%u",
                 output.width, output.height, output.pitch, output.pitch_bytes, output.frame_format,
                 output.picture_count);
    }
    return Result::picture;
}

HwDecoder::Result HwDecoder::decode(std::size_t size, HwPicture *picture)
{
    Parts &parts = *parts_;
    InputData input{};
    input.size = sizeof(input);
    input.unit = next_input();
    input.unit_size = size;
    input.pts = parts.input_at; // the output carries no time; PendingStamps pairs them up
    input.dts = UINT64_MAX;
    parts.input_at = (parts.input_at + 1) % kInputSlots;
    FrameBuffer frame;
    parts.next_frame(&frame);
    OutputInfo output{};
    output.size = sizeof(output);
    const int code = sceVideodec2Decode(parts.decoder, &input, &frame, &output);
    return finish(code, &frame, &output, picture);
}

HwDecoder::Result HwDecoder::drain(HwPicture *picture)
{
    FrameBuffer frame;
    parts_->next_frame(&frame);
    OutputInfo output{};
    output.size = sizeof(output);
    const int code = sceVideodec2Flush(parts_->decoder, &frame, &output);
    return finish(code, &frame, &output, picture);
}

bool HwDecoder::renew()
{
    Parts &parts = *parts_;
    if (parts.decoder != nullptr)
        sceVideodec2DeleteDecoder(parts.decoder);
    parts.decoder = nullptr;
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
