// Mediathek - A player that plays nothing: for the PC preview and the tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/player.hpp"

#include <algorithm>
#include <cmath>

namespace hui::media
{

namespace
{

constexpr int kWidth = 160;
constexpr int kHeight = 90;

class SimulatedPlayer final : public Player
{
  public:
    SimulatedPlayer(const VideoSurface &surface, double duration)
        : surface_(surface), duration_(duration)
    {
        tracks_ = {{0, "English · AC3 5.1"}, {1, "Deutsch · AAC 2.0"}};
    }
    ~SimulatedPlayer() override
    {
        close();
    }

    void open(const std::string &, double start_seconds) override
    {
        close();
        position_ = std::clamp(start_seconds, 0.0, duration_);
        state_ = PlayerState::opening;
        opening_ = 0.4f;
        summary_ = "H.264 1080p · AC3 5.1";
    }
    void close() override
    {
        if (texture_ != 0 && surface_.release)
            surface_.release(texture_);
        texture_ = 0;
        state_ = PlayerState::idle;
    }
    void update(float dt) override
    {
        clock_ += dt;
        if (state_ == PlayerState::opening)
        {
            opening_ -= dt;
            if (opening_ <= 0.0f)
                state_ = PlayerState::playing;
        }
        if (state_ == PlayerState::playing)
        {
            position_ += static_cast<double>(dt);
            if (position_ >= duration_)
            {
                position_ = duration_;
                state_ = PlayerState::ended;
            }
        }
        if (state_ == PlayerState::playing || state_ == PlayerState::paused)
            draw_frame();
    }
    void pause(bool paused) override
    {
        if (paused && state_ == PlayerState::playing)
            state_ = PlayerState::paused;
        else if (!paused && state_ == PlayerState::paused)
            state_ = PlayerState::playing;
    }
    void seek_by(double seconds) override
    {
        seek_to(position_ + seconds);
    }
    void seek_to(double seconds) override
    {
        position_ = std::clamp(seconds, 0.0, duration_);
        if (state_ == PlayerState::ended)
            state_ = PlayerState::playing;
    }

    PlayerState state() const override
    {
        return state_;
    }
    double position() const override
    {
        return position_;
    }
    double duration() const override
    {
        return duration_;
    }
    std::uint32_t texture() const override
    {
        return texture_;
    }
    int video_width() const override
    {
        return kWidth;
    }
    int video_height() const override
    {
        return kHeight;
    }
    const std::string &error() const override
    {
        return error_;
    }
    const std::string &summary() const override
    {
        return summary_;
    }
    const std::vector<Track> &audio_tracks() const override
    {
        return tracks_;
    }
    int audio_track() const override
    {
        return track_;
    }
    void select_audio(int index) override
    {
        if (index >= 0 && index < static_cast<int>(tracks_.size()))
            track_ = index;
    }

  private:
    // A slow colour field with a moving bar, so a recording of the preview
    // shows that frames arrive and the scrubber agrees with them.
    void draw_frame()
    {
        if (!surface_.create || !surface_.update)
            return;
        const float t = static_cast<float>(position_);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
            {
                std::uint8_t *p = pixels_ + (y * kWidth + x) * 4;
                const float u = static_cast<float>(x) / kWidth;
                const float v = static_cast<float>(y) / kHeight;
                p[0] = static_cast<std::uint8_t>(
                    60.0f + 90.0f * (0.5f + 0.5f * std::sin(t * 0.3f + u * 3.0f)));
                p[1] = static_cast<std::uint8_t>(
                    50.0f + 70.0f * (0.5f + 0.5f * std::sin(t * 0.2f + v * 4.0f)));
                p[2] = static_cast<std::uint8_t>(110.0f + 100.0f * v);
                p[3] = 255;
                const float bar = std::fmod(t * 0.15f, 1.0f);
                if (std::fabs(u - bar) < 0.01f)
                    p[0] = p[1] = p[2] = 240;
            }
        if (texture_ == 0)
            texture_ = surface_.create(kWidth, kHeight, pixels_);
        else
            surface_.update(texture_, kWidth, kHeight, pixels_);
    }

    VideoSurface surface_;
    double duration_;
    double position_ = 0.0;
    float clock_ = 0.0f;
    float opening_ = 0.0f;
    PlayerState state_ = PlayerState::idle;
    std::uint32_t texture_ = 0;
    std::string error_;
    std::string summary_;
    std::vector<Track> tracks_;
    int track_ = 0;
    std::uint8_t pixels_[kWidth * kHeight * 4] = {};
};

} // namespace

std::unique_ptr<Player> make_simulated_player(const VideoSurface &surface, double duration)
{
    return std::make_unique<SimulatedPlayer>(surface, duration);
}

} // namespace hui::media
