// Mediathek - A player that plays nothing: for the PC preview and the tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/player.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hui::media
{

namespace
{

constexpr int kWidth = 160;
constexpr int kHeight = 90;
// The HDR card is twice the size it is shown at, so its luma is halved as 4K's is.
constexpr int kCardWidth = 640;
constexpr int kCardHeight = 360;
constexpr float kPaperWhite = 203.0f;

// One colour in nits, BT.709 primaries, as 10-bit limited-range BT.2020 Y'CbCr with PQ:
// what an HDR10 film holds.
std::array<int, 3> hdr10(std::array<float, 3> nits)
{
    const float r = 0.6274f * nits[0] + 0.3293f * nits[1] + 0.0433f * nits[2];
    const float g = 0.0691f * nits[0] + 0.9195f * nits[1] + 0.0114f * nits[2];
    const float b = 0.0164f * nits[0] + 0.0880f * nits[1] + 0.8956f * nits[2];
    const auto pq = [](float value)
    {
        const float y = std::pow(std::max(value, 0.0f) / 10000.0f, 0.1593017578125f);
        return std::pow((0.8359375f + 18.8515625f * y) / (1.0f + 18.6875f * y), 78.84375f);
    };
    const float rp = pq(r);
    const float gp = pq(g);
    const float bp = pq(b);
    const float luma = 0.2627f * rp + 0.6780f * gp + 0.0593f * bp;
    const float cb = (bp - luma) / 1.8814f;
    const float cr = (rp - luma) / 1.4746f;
    return {static_cast<int>(std::lround(64.0f + 876.0f * luma)),
            static_cast<int>(std::lround(512.0f + 896.0f * cb)),
            static_cast<int>(std::lround(512.0f + 896.0f * cr))};
}

// Six colours and white, as the bars of a test card.
std::array<float, 3> bar(int index, float nits)
{
    static constexpr float kBars[7][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0},
                                          {1, 0, 1}, {1, 0, 0}, {0, 0, 1}};
    return {kBars[index][0] * nits, kBars[index][1] * nits, kBars[index][2] * nits};
}

class SimulatedPlayer final : public Player
{
  public:
    SimulatedPlayer(const VideoSurface &surface, double duration, bool hdr)
        : surface_(surface), duration_(duration), hdr_(hdr && surface.update_yuv)
    {
        tracks_ = {{0, "English · AC3 5.1"}, {1, "Deutsch · AAC 2.0"}};
        if (hdr_)
            make_card();
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
        summary_ = hdr_ ? "HEVC 4K HDR10 · AC3 5.1" : "H.264 1080p · AC3 5.1";
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
        if ((state_ == PlayerState::playing || state_ == PlayerState::paused) &&
            !(hdr_ && draw_card()))
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

    // Paper white's bars on top, a grey ramp from 0.1 to 4000 nits in the middle and the
    // bars again at 1000 nits below: the tone curve has to keep the top as it is, roll
    // the ramp off smoothly and keep the bright bars' colours.
    void make_card()
    {
        card_luma_.assign(static_cast<std::size_t>(kCardWidth * kCardHeight), 0);
        card_chroma_.assign(static_cast<std::size_t>(kCardWidth * kCardHeight / 2), 0);
        for (int y = 0; y < kCardHeight; ++y)
            for (int x = 0; x < kCardWidth; ++x)
            {
                const int band = y * 3 / kCardHeight;
                const float u = static_cast<float>(x) / kCardWidth;
                std::array<float, 3> nits{};
                if (band == 1)
                {
                    const float grey = 0.1f * std::pow(40000.0f, u);
                    nits = {grey, grey, grey};
                }
                else
                {
                    nits = bar(x * 7 / kCardWidth, band == 0 ? kPaperWhite : 1000.0f);
                }
                const std::array<int, 3> code = hdr10(nits);
                card_luma_[static_cast<std::size_t>(y * kCardWidth + x)] =
                    static_cast<std::uint16_t>(code[0]);
                if (x % 2 == 0 && y % 2 == 0)
                {
                    const std::size_t at = static_cast<std::size_t>((y / 2) * kCardWidth + x);
                    card_chroma_[at] = static_cast<std::uint16_t>(code[1]);
                    card_chroma_[at + 1] = static_cast<std::uint16_t>(code[2]);
                }
            }
    }

    // The card with a 4000-nit bar moving across it, sent as planes. False when the
    // surface would not take them.
    bool draw_card()
    {
        std::vector<std::uint16_t> luma = card_luma_;
        const float t = static_cast<float>(position_);
        const int column = static_cast<int>(std::fmod(t * 0.15f, 1.0f) * kCardWidth) & ~1;
        const int peak = hdr10({4000.0f, 4000.0f, 4000.0f})[0];
        for (int y = 0; y < kCardHeight; ++y)
            for (int x = column; x < std::min(column + 6, kCardWidth); ++x)
                luma[static_cast<std::size_t>(y * kCardWidth + x)] =
                    static_cast<std::uint16_t>(peak);
        YuvPicture card;
        card.luma = reinterpret_cast<const std::uint8_t *>(luma.data());
        card.chroma = reinterpret_cast<const std::uint8_t *>(card_chroma_.data());
        card.pitch = kCardWidth * sizeof(std::uint16_t);
        card.width = kCardWidth;
        card.height = kCardHeight;
        card.wide = true;
        YuvFrame frame;
        frame.layout = yuv_layout(card, kWidth * 2, kHeight * 2);
        planes_.resize(yuv_bytes(frame.layout));
        copy_planes(card, frame.layout, planes_.data());
        frame.planes = planes_.data();
        frame.colour.matrix = YuvMatrix::bt2020;
        frame.colour.transfer = Transfer::pq;
        frame.colour.wide_gamut = true;
        frame.colour.peak_nits = hdr_peak_nits(4000.0f, 0.0f);
        if (texture_ == 0)
            texture_ = surface_.create(kWidth * 2, kHeight * 2, nullptr);
        if (surface_.update_yuv(texture_, kWidth * 2, kHeight * 2, frame))
            return true;
        hdr_ = false; // as the real player does: the CPU from now on
        surface_.release(texture_);
        texture_ = 0;
        return false;
    }

    VideoSurface surface_;
    double duration_;
    bool hdr_;
    std::vector<std::uint16_t> card_luma_;
    std::vector<std::uint16_t> card_chroma_;
    std::vector<std::uint8_t> planes_;
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

std::unique_ptr<Player> make_simulated_player(const VideoSurface &surface, double duration,
                                              bool hdr)
{
    return std::make_unique<SimulatedPlayer>(surface, duration, hdr);
}

} // namespace hui::media
