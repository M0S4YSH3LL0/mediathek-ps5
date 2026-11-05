// Mediathek - The video player interface the screens talk to.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/yuv_picture.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hui::media
{

enum class PlayerState : std::uint8_t
{
    idle,      // nothing open
    opening,   // reading the file's headers
    buffering, // playback stalled: the decoder or the drive is behind
    playing,
    paused,
    ended,
    failed,
};

struct Track
{
    int index = 0;
    std::string label; // "English · AC3 5.1"
};

// A subtitle track inside the film.
struct SubtitleTrack
{
    std::string language = "und"; // canonical ISO 639-2 code
    bool hearing_impaired = false;
    bool picture = false; // PGS, VobSub, DVB: drawn as a picture rather than as text
};

// A picture subtitle on screen: its texture, and where it goes as fractions of the frame it was
// made for (x, y from its top left corner). That frame is the film's picture when `aspect` is 0
// (a DVD's subtitles, made at the film's size); otherwise it is a frame of that width / height,
// fitted to the screen as a film would be: a Blu-ray's are made on 1920 x 1080 whether or not
// the film was cropped to 1920 x 800, and belong where they are on the full screen.
struct SubtitlePicture
{
    std::uint32_t texture = 0; // 0: none showing
    float aspect = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// How a player puts pictures on screen: it owns one texture and rewrites it
// as frames arrive. The app wires these to the GL batch.
struct VideoSurface
{
    std::function<std::uint32_t(int, int, const std::uint8_t *)> create;
    std::function<void(std::uint32_t, int, int, const std::uint8_t *)> update;
    std::function<void(std::uint32_t)> release;
    // Optional: draws a picture's YUV planes into the texture (made by `create`, with no
    // pixels) on the graphics chip, tone mapping HDR on the way. False when it cannot;
    // the player then converts on the CPU instead.
    std::function<bool(std::uint32_t, int, int, const YuvFrame &)> update_yuv;
};

// One movie at a time. All calls come from the frame thread; decoding runs on
// the player's own threads and never blocks it.
class Player
{
  public:
    virtual ~Player() = default;

    // Starts opening a file, at start_seconds in. Returns at once.
    virtual void open(const std::string &path, double start_seconds) = 0;
    // Stops and lets go of the file and the texture.
    virtual void close() = 0;
    // Once per frame: updates the state and puts the newest frame on screen.
    virtual void update(float dt) = 0;

    virtual void pause(bool paused) = 0;
    virtual void seek_by(double seconds) = 0;
    virtual void seek_to(double seconds) = 0;

    virtual PlayerState state() const = 0;
    virtual double position() const = 0;
    virtual double duration() const = 0; // 0 while unknown
    // 0 until the first frame is up.
    virtual std::uint32_t texture() const = 0;
    virtual int video_width() const = 0;
    virtual int video_height() const = 0;
    // Why state() is failed, in words a person can act on.
    virtual const std::string &error() const = 0;
    // "H.264 1080p · AAC 5.1": what is being played, for the info panel.
    virtual const std::string &summary() const = 0;

    virtual const std::vector<Track> &audio_tracks() const = 0;
    virtual int audio_track() const = 0;
    virtual void select_audio(int index) = 0;
    // Whether a 5.1 or 7.1 track may play on as many channels (Settings *Audio output*);
    // otherwise it is mixed down to stereo. Applies from the next film. A player that
    // knows only stereo ignores it.
    virtual void set_surround(bool allowed)
    {
        static_cast<void>(allowed);
    }
    // The language a film starts in (Settings *Audio language*): a canonical ISO 639-2 code
    // such as "ger", "" for Automatic (media::preferred_audio_track). Applies from
    // the next film.
    virtual void set_audio_language(std::string language)
    {
        static_cast<void>(language);
    }

    // The film's own subtitle tracks: empty until it is open (state() past opening).
    virtual const std::vector<SubtitleTrack> &subtitle_tracks() const = 0;
    virtual int subtitle_track() const = 0; // -1: none
    // -1 for none. May be called while the film opens; it applies once it is open.
    virtual void select_subtitle(int index) = 0;
    // What the chosen track shows now, kept up by update(): a text track's lines joined
    // with '\n' ("" for none), and a picture track's picture.
    virtual const std::string &subtitle_text() const = 0;
    virtual SubtitlePicture subtitle_picture() const = 0;
};

// A player that plays nothing: it counts time forward and shows a moving
// test picture. It drives the screens on a PC and in the tests.
// `hdr`: the picture is a 10-bit PQ test card sent as YUV planes through update_yuv,
// which shows the GPU's conversion and tone mapping in the PC preview.
// `embedded_subtitles`: the film has subtitle tracks of its own, an English and a German text
// track and a German picture track for the hard of hearing, each showing a line now and then.
std::unique_ptr<Player> make_simulated_player(const VideoSurface &surface, double duration,
                                              bool hdr = false, bool embedded_subtitles = false);

} // namespace hui::media
