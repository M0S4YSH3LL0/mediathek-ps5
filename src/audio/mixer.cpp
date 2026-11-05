// ps5-homebrew-ui - Real-time stereo mixer (48 kHz, S16 output).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/mixer.hpp"

#include "audio/surround.hpp"

#include <algorithm>
#include <cmath>

namespace hui::audio
{

namespace
{

constexpr double kTwoPi = 6.283185307179586;
// Per-sample smoothing toward target gains (about 5 ms), avoiding zipper noise.
constexpr float kGainSmoothing = 0.004f;

void pan_gains(float pan, float *left, float *right)
{
    const float clamped = std::clamp(pan, -1.0f, 1.0f);
    const float angle = (clamped + 1.0f) * 0.25f * 3.14159265f;
    *left = std::cos(angle) * 1.41421356f;
    *right = std::sin(angle) * 1.41421356f;
}

} // namespace

Mixer::Mixer() : stream_buffer_(kStreams * kMaxGrain * kSurroundChannels, 0.0f)
{
    bus_gain_.fill(1.0f);
    bus_target_.fill(1.0f);
}

bool Mixer::post(const Command &command)
{
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) % kQueue;
    if (next == tail_.load(std::memory_order_acquire))
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    queue_[head] = command;
    head_.store(next, std::memory_order_release);
    return true;
}

bool Mixer::play_clip(const Clip *clip, const PlayParams &params)
{
    if (clip == nullptr || clip->samples == nullptr || clip->frames == 0)
        return false;
    Command command;
    command.type = CommandType::clip;
    command.clip = clip;
    command.params = params;
    return post(command);
}

bool Mixer::play_tone(const Tone &tone, const PlayParams &params)
{
    Command command;
    command.type = CommandType::tone;
    command.tone = tone;
    command.params = params;
    return post(command);
}

bool Mixer::set_bus_gain(Bus bus, float gain)
{
    Command command;
    command.type = CommandType::bus_gain;
    command.params.bus = bus;
    command.value = gain;
    return post(command);
}

bool Mixer::set_master_gain(float gain)
{
    Command command;
    command.type = CommandType::master_gain;
    command.value = gain;
    return post(command);
}

bool Mixer::set_stream_gain(std::size_t slot, float gain, float seconds)
{
    if (slot >= kStreams)
        return false;
    Command command;
    command.type = CommandType::stream_gain;
    command.slot = slot;
    command.value = gain;
    command.seconds = seconds;
    return post(command);
}

bool Mixer::swap_stream(std::size_t slot, StreamRing *ring)
{
    if (slot >= kStreams)
        return false;
    Command command;
    command.type = CommandType::stream_swap;
    command.slot = slot;
    command.ring = ring;
    return post(command);
}

bool Mixer::stop_all()
{
    Command command;
    command.type = CommandType::stop_all;
    return post(command);
}

Mixer::Voice &Mixer::allocate_voice()
{
    Voice *oldest = &voices_[0];
    for (Voice &voice : voices_)
    {
        if (!voice.active)
            return voice;
        if (voice.age < oldest->age)
            oldest = &voice;
    }
    return *oldest; // steal the oldest voice
}

void Mixer::apply(const Command &command)
{
    switch (command.type)
    {
    case CommandType::clip:
    case CommandType::tone:
    {
        Voice &voice = allocate_voice();
        voice = Voice{};
        voice.active = true;
        voice.is_tone = command.type == CommandType::tone;
        voice.clip = command.clip;
        voice.tone = command.tone;
        voice.params = command.params;
        voice.age = ++voice_clock_;
        pan_gains(command.params.pan, &voice.left, &voice.right);
        break;
    }
    case CommandType::bus_gain:
        bus_target_[static_cast<std::size_t>(command.params.bus)] =
            std::clamp(command.value, 0.0f, 1.0f);
        break;
    case CommandType::master_gain:
        master_target_ = std::clamp(command.value, 0.0f, 1.0f);
        break;
    case CommandType::stop_all:
        for (Voice &voice : voices_)
            voice.active = false;
        break;
    case CommandType::stream_swap:
        streams_[command.slot] = command.ring;
        break;
    case CommandType::stream_gain:
    {
        const float target = std::clamp(command.value, 0.0f, 1.0f);
        stream_target_[command.slot] = target;
        const float distance = std::fabs(target - stream_gain_[command.slot]);
        stream_step_[command.slot] =
            command.seconds > 0.0f ? distance / (command.seconds * kSampleRate) : distance;
        break;
    }
    }
}

float Mixer::tone_sample(Voice &voice)
{
    const Tone &tone = voice.tone;
    const double t = voice.position;
    if (t >= tone.seconds)
    {
        voice.active = false;
        return 0.0f;
    }
    const double progress = t / std::max(1e-6f, tone.seconds);
    const double hz =
        (tone.start_hz + (tone.end_hz - tone.start_hz) * progress) * voice.params.pitch;
    voice.phase += hz / kSampleRate;
    voice.phase -= std::floor(voice.phase);
    const double p = voice.phase;
    double value = 0.0;
    switch (tone.wave)
    {
    case Wave::sine:
        value = std::sin(kTwoPi * p);
        break;
    case Wave::triangle:
        value = 4.0 * std::fabs(p - 0.5) - 1.0;
        break;
    case Wave::soft_square:
        value = std::tanh(3.0 * std::sin(kTwoPi * p)) * 0.8;
        break;
    }
    double envelope = 1.0;
    if (tone.attack > 0.0f && t < tone.attack)
        envelope = t / tone.attack;
    const double remaining = tone.seconds - t;
    if (tone.release > 0.0f && remaining < tone.release)
        envelope = std::min(envelope, remaining / tone.release);
    voice.position += 1.0 / kSampleRate;
    return static_cast<float>(value * envelope);
}

float Mixer::limit(float sample)
{
    const float magnitude = std::fabs(sample);
    if (magnitude <= kKnee)
        return sample;
    const float range = kCeiling - kKnee;
    const float shaped = kKnee + range * std::tanh((magnitude - kKnee) / range);
    return sample < 0.0f ? -shaped : shaped;
}

bool Mixer::wants_surround() const
{
    const StreamRing *ring = streams_[kMovieStream];
    return ring != nullptr && ring->channels() == kSurroundChannels && ring->surround();
}

void Mixer::render(std::int16_t *out, int frames, std::int16_t *surround)
{
    // Drain commands posted since the last grain.
    std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    while (tail != head)
    {
        apply(queue_[tail]);
        tail = (tail + 1) % kQueue;
    }
    tail_.store(tail, std::memory_order_release);

    // Long requests (host tests) are mixed in stream-buffer-sized chunks.
    while (frames > static_cast<int>(kMaxGrain))
    {
        render_chunk(out, static_cast<int>(kMaxGrain), surround);
        out += kMaxGrain * 2;
        if (surround != nullptr)
            surround += kMaxGrain * kSurroundChannels;
        frames -= static_cast<int>(kMaxGrain);
    }
    render_chunk(out, frames, surround);

    int active = 0;
    for (const Voice &voice : voices_)
        active += voice.active ? 1 : 0;
    active_.store(active, std::memory_order_relaxed);
}

void Mixer::render_chunk(std::int16_t *out, int frames, std::int16_t *surround)
{
    // Pull this grain from each stream; a short read plays silence.
    for (std::size_t s = 0; s < kStreams; ++s)
    {
        float *buffer = stream_buffer_.data() + s * kMaxGrain * kSurroundChannels;
        const std::size_t width = streams_[s] != nullptr ? streams_[s]->channels() : 2;
        std::size_t got = 0;
        if (streams_[s] != nullptr)
            got = streams_[s]->read(buffer, static_cast<std::size_t>(frames));
        if (got < static_cast<std::size_t>(frames))
        {
            std::fill(buffer + got * width, buffer + static_cast<std::size_t>(frames) * width,
                      0.0f);
            if (streams_[s] != nullptr && stream_gain_[s] > 0.0f)
                underruns_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    for (int frame = 0; frame < frames; ++frame)
    {
        for (std::size_t bus = 0; bus < bus_gain_.size(); ++bus)
            bus_gain_[bus] += (bus_target_[bus] - bus_gain_[bus]) * kGainSmoothing;
        master_gain_ += (master_target_ - master_gain_) * kGainSmoothing;

        float mix[2] = {};
        float film[kSurroundChannels] = {}; // for the surround port
        for (Voice &voice : voices_)
        {
            if (!voice.active)
                continue;
            float l = 0.0f;
            float r = 0.0f;
            if (voice.is_tone)
            {
                const float s = tone_sample(voice);
                l = s;
                r = s;
            }
            else
            {
                const std::size_t index = static_cast<std::size_t>(voice.position);
                if (index + 1 >= voice.clip->frames)
                {
                    voice.active = false;
                    continue;
                }
                const float fraction =
                    static_cast<float>(voice.position - static_cast<double>(index));
                const float *a = voice.clip->samples + index * 2;
                const float *b = a + 2;
                l = a[0] + (b[0] - a[0]) * fraction;
                r = a[1] + (b[1] - a[1]) * fraction;
                voice.position += voice.params.pitch;
            }
            const float gain =
                voice.params.gain * bus_gain_[static_cast<std::size_t>(voice.params.bus)];
            mix[kFrontLeft] += l * gain * voice.left;
            mix[kFrontRight] += r * gain * voice.right;
        }
        const float music = bus_gain_[static_cast<std::size_t>(Bus::music)];
        for (std::size_t s = 0; s < kStreams; ++s)
        {
            float &gain = stream_gain_[s];
            const float target = stream_target_[s];
            if (gain != target)
                gain = gain < target ? std::min(target, gain + stream_step_[s])
                                     : std::max(target, gain - stream_step_[s]);
            if (gain <= 0.0f || streams_[s] == nullptr)
                continue;
            const float level = gain * (s == kMovieStream ? 1.0f : music);
            const std::size_t width = streams_[s]->channels();
            const float *in = stream_buffer_.data() + s * kMaxGrain * kSurroundChannels +
                              static_cast<std::size_t>(frame) * width;
            if (width != kSurroundChannels)
            {
                mix[kFrontLeft] += in[0] * level;
                mix[kFrontRight] += in[1] * level;
            }
            else if (surround != nullptr && streams_[s]->surround())
            {
                for (std::size_t c = 0; c < kSurroundChannels; ++c)
                    film[c] += in[c] * level;
            }
            else
            {
                float l = 0.0f;
                float r = 0.0f;
                downmix_frame(in, &l, &r);
                mix[kFrontLeft] += l * level;
                mix[kFrontRight] += r * level;
            }
        }
        std::int16_t *to = out + static_cast<std::size_t>(frame) * 2;
        for (std::size_t c = 0; c < 2; ++c)
            to[c] = static_cast<std::int16_t>(std::lrint(limit(mix[c] * master_gain_) * 32767.0f));
        if (surround != nullptr)
        {
            std::int16_t *at = surround + static_cast<std::size_t>(frame) * kSurroundChannels;
            for (std::size_t c = 0; c < kSurroundChannels; ++c)
                at[c] =
                    static_cast<std::int16_t>(std::lrint(limit(film[c] * master_gain_) * 32767.0f));
        }
    }
}

} // namespace hui::audio
