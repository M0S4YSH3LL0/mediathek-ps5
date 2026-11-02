// Mediathek - What the console's hardware picture decoder takes and how it is fed.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rules for an HEVC access unit come from what EVO Player found on the hardware
// (GPL-3.0-or-later, https://github.com/sainsaji/EVO-PLAYER-PS5); this code is modelled on theirs.

#include "media/hw_stream.hpp"

#include <algorithm>
#include <cstring>

namespace hui::media
{

namespace
{

constexpr int kMaxWidth = 3840;
constexpr int kMaxHeight = 2176;

// The level says how big a picture the decoder must expect: a 4K surface at a lower
// one is refused as an invalid configuration. HEVC counts in thirtieths: 156 is 5.2,
// which VLC's port needed for 4K films at 60-80 Mbit/s, and 153 (5.1) is the one known
// to work everywhere, tried when 5.2 is refused.
constexpr unsigned kH264Level = 52;
constexpr unsigned kHevcLevel = 156;
constexpr unsigned kHevcFallbackLevel = 153;

constexpr int kH264ConstrainedBaseline = 578; // FFmpeg's number: 66 with a flag
constexpr int kNalPicturesEnd = 23;           // beyond it: parameter sets, SEI, ...
constexpr int kNalTrailingEnd = 5;
constexpr int kNalRaslN = 8;
constexpr int kNalRaslR = 9;
constexpr int kNalKeyFirst = 16;
constexpr int kNalKeyLast = 23;
constexpr int kNalVps = 32;
constexpr int kNalPps = 34;
constexpr int kNalIgnoredFirst = 48;
constexpr std::size_t kMaxNals = 512;
constexpr int kH264Slice = 1;
constexpr int kH264Idr = 5;
constexpr unsigned kH264SliceI = 2;    // slice_type modulo 5
constexpr std::size_t kMaxStamps = 64; // far beyond any reorder window

int padded(int size, int ceiling)
{
    // An encoder may pad to its largest block (up to 64); a decoder sized for less
    // refuses every picture of such a film.
    const int blocks = (size + 15) & ~15;
    const int wide = (blocks + 63) & ~63;
    return wide <= ceiling ? wide : blocks;
}

// Reads the start of a parameter set: its bits, without the bytes that keep a start
// code from appearing in the data. Past the end it reads zeros.
class Bits
{
  public:
    Bits(const std::uint8_t *data, std::size_t size) : data_(data), size_(size)
    {
    }

    unsigned read(int count)
    {
        unsigned value = 0;
        for (int i = 0; i < count; ++i)
            value = (value << 1) | next();
        return value;
    }

    unsigned read_ue()
    {
        int zeros = 0;
        while (zeros < 31 && next() == 0)
            ++zeros;
        return ((1u << zeros) - 1u) + read(zeros);
    }

  private:
    unsigned next()
    {
        if (at_ >= size_)
            return 0;
        if (bit_ == 0 && zeros_ >= 2 && data_[at_] == 3)
        {
            ++at_;
            zeros_ = 0;
            if (at_ >= size_)
                return 0;
        }
        const unsigned value = (data_[at_] >> (7 - bit_)) & 1u;
        if (++bit_ == 8)
        {
            zeros_ = data_[at_] == 0 ? zeros_ + 1 : 0;
            bit_ = 0;
            ++at_;
        }
        return value;
    }

    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t at_ = 0;
    int bit_ = 0;
    int zeros_ = 0;
};

// The id a parameter set defines; `data` starts after the NAL header.
unsigned parameter_set_id(int type, const std::uint8_t *data, std::size_t size)
{
    Bits bits(data, size);
    if (type == kNalVps)
        return bits.read(4);
    if (type == kNalPps)
        return bits.read_ue();
    // SPS: the VPS id, the sub-layer count, the nesting flag, the profile, then the id.
    bits.read(4);
    const unsigned layers = bits.read(3);
    bits.read(1);
    bits.read(32);
    bits.read(32);
    bits.read(32);
    bool profile[8] = {};
    bool level[8] = {};
    for (unsigned i = 0; i < layers; ++i)
    {
        profile[i] = bits.read(1) != 0;
        level[i] = bits.read(1) != 0;
    }
    if (layers > 0)
        for (unsigned i = layers; i < 8; ++i)
            bits.read(2);
    for (unsigned i = 0; i < layers; ++i)
    {
        if (profile[i])
        {
            bits.read(32);
            bits.read(32);
            bits.read(24);
        }
        if (level[i])
            bits.read(8);
    }
    return bits.read_ue();
}

// Where a start code at or after `from` begins, or `size` if there is none.
std::size_t find_start(const std::uint8_t *unit, std::size_t size, std::size_t from)
{
    for (std::size_t i = from; i + 3 <= size; ++i)
        if (unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1)
            return i;
    return size;
}

struct Nal
{
    std::size_t begin = 0;   // the start code
    std::size_t payload = 0; // the NAL header
    std::size_t end = 0;
    std::uint32_t key = 0; // what identifies a parameter set; 0 for everything else
};

} // namespace

bool wants_hw_decoder(int width, int height)
{
    return width > 1920 || height > 1088;
}

std::optional<HwMode> hw_mode(HwCodec codec, int profile, int depth, bool yuv420, int width,
                              int height)
{
    if (!yuv420 || width <= 0 || height <= 0 || width > kMaxWidth || height > kMaxHeight)
        return std::nullopt;
    HwMode mode;
    mode.codec = codec;
    mode.max_width = padded(width, kMaxWidth);
    mode.max_height = padded(height, kMaxHeight);
    if (codec == HwCodec::h264)
    {
        const bool known = profile == 66 || profile == kH264ConstrainedBaseline || profile == 77 ||
                           profile == 100 || profile < 0;
        if (depth != 8 || !known)
            return std::nullopt;
        mode.profile = 100;
        mode.level = kH264Level;
        return mode;
    }
    if (depth > 10 || (profile >= 0 && profile != 1 && profile != 2) || (profile == 1 && depth > 8))
        return std::nullopt;
    mode.profile = depth > 8 || profile == 2 ? 2 : 1;
    mode.level = kHevcLevel;
    mode.fallback_level = kHevcFallbackLevel;
    return mode;
}

int hevc_picture_type(const std::uint8_t *unit, std::size_t size)
{
    std::size_t at = find_start(unit, size, 0);
    while (at + 3 < size)
    {
        const int type = (unit[at + 3] >> 1) & 0x3f;
        if (type <= kNalPicturesEnd)
            return type;
        at = find_start(unit, size, at + 3);
    }
    return -1;
}

std::size_t clean_hevc_unit(const std::uint8_t *unit, std::size_t size, std::uint8_t *out)
{
    std::vector<Nal> nals;
    nals.reserve(16);
    std::size_t at = find_start(unit, size, 0);
    if (at == size)
    {
        // Not in start-code form: nothing here can be read, so nothing is taken out.
        if (size > 0)
            std::memcpy(out, unit, size);
        return size;
    }
    while (at < size)
    {
        if (nals.size() == kMaxNals)
        {
            std::memcpy(out, unit, size); // not a real picture's worth; leave it alone
            return size;
        }
        const std::size_t next = find_start(unit, size, at + 3);
        Nal nal;
        // A four-byte start code's leading zero belongs to it, not to the NAL before.
        nal.begin = at > 0 && unit[at - 1] == 0 ? at - 1 : at;
        nal.payload = at + 3;
        nal.end = next;
        if (next < size && unit[next - 1] == 0)
            nal.end = next - 1;
        if (nal.payload + 2 < nal.end)
        {
            const int type = (unit[nal.payload] >> 1) & 0x3f;
            if (type >= kNalVps && type <= kNalPps)
            {
                const unsigned layer =
                    ((unit[nal.payload] & 1u) << 5) | (unit[nal.payload + 1] >> 3);
                const unsigned id =
                    parameter_set_id(type, unit + nal.payload + 2, nal.end - nal.payload - 2);
                nal.key = 0x80000000u | (static_cast<std::uint32_t>(type) << 24) | (layer << 16) |
                          (id & 0xffffu);
            }
        }
        nals.push_back(nal);
        at = next;
    }

    std::size_t written = 0;
    for (std::size_t i = 0; i < nals.size(); ++i)
    {
        const Nal &nal = nals[i];
        if (nal.payload < size && ((unit[nal.payload] >> 1) & 0x3f) >= kNalIgnoredFirst)
            continue;
        if (nal.payload + 1 < size &&
            (((unit[nal.payload] & 1u) << 5) | (unit[nal.payload + 1] >> 3)) != 0)
            continue; // not the base layer
        if (nal.key != 0 &&
            std::any_of(nals.begin() + static_cast<std::ptrdiff_t>(i) + 1, nals.end(),
                        [&](const Nal &later) { return later.key == nal.key; }))
            continue;
        std::memcpy(out + written, unit + nal.begin, nal.end - nal.begin);
        written += nal.end - nal.begin;
    }
    return written;
}

bool h264_is_key(const std::uint8_t *unit, std::size_t size)
{
    std::size_t at = find_start(unit, size, 0);
    while (at + 3 < size)
    {
        const int type = unit[at + 3] & 0x1f;
        if (type == kH264Idr)
            return true;
        if (type == kH264Slice)
        {
            Bits bits(unit + at + 4, size - at - 4);
            bits.read_ue(); // first_mb_in_slice
            return bits.read_ue() % 5 == kH264SliceI;
        }
        at = find_start(unit, size, at + 3);
    }
    return false;
}

void HevcEntry::restart()
{
    leading_ = true;
    need_key_ = false;
}

bool HevcEntry::admit(int type)
{
    if (type < 0)
        return true;
    if (need_key_)
    {
        if (type < kNalKeyFirst || type > kNalKeyLast)
            return false;
        need_key_ = false;
    }
    if (leading_)
    {
        if (type == kNalRaslN || type == kNalRaslR)
            return false;
        if (type <= kNalTrailingEnd)
            leading_ = false; // past these no RASL can follow until the next key frame
    }
    return true;
}

void HevcEntry::refused()
{
    need_key_ = true;
}

void PendingStamps::push(double seconds)
{
    // A picture the decoder lost leaves its time behind; dropping the oldest keeps one
    // lost picture from holding every later one a frame early for the rest of the film.
    if (stamps_.size() >= kMaxStamps)
        stamps_.erase(std::min_element(stamps_.begin(), stamps_.end()));
    stamps_.push_back(seconds);
}

bool PendingStamps::take(double *seconds)
{
    if (stamps_.empty())
        return false;
    const auto earliest = std::min_element(stamps_.begin(), stamps_.end());
    *seconds = *earliest;
    stamps_.erase(earliest);
    return true;
}

void PendingStamps::reset()
{
    stamps_.clear();
}

} // namespace hui::media
