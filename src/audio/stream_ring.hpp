// ps5-homebrew-ui - Lock-free ring of float frames (one producer, one consumer).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace hui::audio
{

// The game thread writes decoded music; the audio thread reads it. Neither
// side blocks: a short read means the producer fell behind (silence fills in).
// A frame is `channels` interleaved samples: 2 (left, right) for music, 8 for a
// film's sound, in the console's order (see audio/surround.hpp).
class StreamRing
{
  public:
    explicit StreamRing(std::size_t frames = 1u << 16, std::size_t channels = 2)
        : samples_(frames * channels), capacity_(frames), channels_(channels)
    {
    }

    std::size_t capacity() const
    {
        return capacity_;
    }
    std::size_t channels() const
    {
        return channels_;
    }
    // Whether the frames being written carry more than left and right: the output then
    // asks for the console's 8-channel port. Set by the producer, read by the consumer.
    bool surround() const
    {
        return surround_.load(std::memory_order_relaxed);
    }
    void set_surround(bool surround)
    {
        surround_.store(surround, std::memory_order_relaxed);
    }
    // Frames buffered and not yet read.
    std::size_t available() const
    {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    std::size_t space() const
    {
        return capacity_ - available();
    }

    // Producer: copies up to frames frames of channels() samples; returns how many fit.
    std::size_t write(const float *samples, std::size_t frames)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t count = std::min(frames, capacity_ - (head - tail));
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t at = ((head + i) % capacity_) * channels_;
            std::copy_n(samples + i * channels_, channels_,
                        samples_.begin() + static_cast<std::ptrdiff_t>(at));
        }
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    // Consumer: copies up to frames frames of channels() samples; returns how many were there.
    std::size_t read(float *samples, std::size_t frames)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t count = std::min(frames, head - tail);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t at = ((tail + i) % capacity_) * channels_;
            std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(at), channels_,
                        samples + i * channels_);
        }
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // Consumer side only: drops everything buffered.
    void drain()
    {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

  private:
    std::vector<float> samples_;
    std::size_t capacity_;
    std::size_t channels_;
    std::atomic<bool> surround_{false};
    std::atomic<std::size_t> head_{0}; // frames written, ever
    std::atomic<std::size_t> tail_{0}; // frames read, ever
};

} // namespace hui::audio
