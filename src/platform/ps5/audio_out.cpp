// ps5-homebrew-ui - Audio output thread through sceAudioOut.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/audio_out.hpp"

#include "audio/surround.hpp"
#include "platform/ps5/system.hpp"

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
}

namespace hui::ps5
{

namespace
{

constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);
constexpr int kSystemUser = 0xff;
constexpr int kPortMain = 0;
constexpr unsigned int kFormatS16Stereo = 1;
constexpr unsigned int kFormatS16Surround = 2; // L R C LFE Ls Rs Lb Rb

} // namespace

AudioOut::~AudioOut()
{
    stop();
}

bool AudioOut::start(audio::Mixer &mixer)
{
    const int init = sceAudioOutInit();
    if (init != 0 && init != kAlreadyInitialized)
    {
        sys::log("[HUI] audio sceAudioOutInit=0x%08x", static_cast<unsigned>(init));
        return false;
    }
    handle_ = sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrainFrames, audio::kSampleRate,
                              kFormatS16Stereo);
    if (handle_ < 0)
    {
        sys::log("[HUI] audio sceAudioOutOpen=0x%08x", static_cast<unsigned>(handle_));
        handle_ = -1;
        return false;
    }
    mixer_ = &mixer;
    stop_.store(false);
    // Never name threads: pthread_setname_np hangs on this platform.
    if (pthread_create(&thread_, nullptr, &AudioOut::thread_main, this) != 0)
    {
        sys::log("[HUI] audio thread creation failed");
        sceAudioOutClose(handle_);
        handle_ = -1;
        return false;
    }
    running_ = true;
    sys::log("[HUI] audio open handle=%d grain=%d rate=%d", handle_, kGrainFrames,
             audio::kSampleRate);
    return true;
}

void AudioOut::open_surround()
{
    if (!surround_tried_)
    {
        surround_tried_ = true;
        surround_handle_ = sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrainFrames,
                                           audio::kSampleRate, kFormatS16Surround);
        sys::log("[HUI] audio surround port=0x%08x", static_cast<unsigned>(surround_handle_));
        if (surround_handle_ < 0)
            surround_handle_ = -1; // the mixer mixes surround films down to stereo
    }
}

void *AudioOut::thread_main(void *self)
{
    static_cast<AudioOut *>(self)->run();
    return nullptr;
}

void AudioOut::run()
{
    alignas(64) std::int16_t grain[kGrainFrames * 2];
    alignas(64) std::int16_t surround[kGrainFrames * audio::kSurroundChannels];
    while (!stop_.load(std::memory_order_relaxed))
    {
        if (!surround_tried_ && mixer_->wants_surround())
            open_surround();
        // Once open, the 8-channel port gets every grain (the film, or silence) beside the
        // stereo one. Moving the sound from one port to the other at play and pause, with a
        // menu sound playing, crackled: a port stopped in the middle of a sound.
        const bool both = surround_handle_ >= 0;
        mixer_->render(grain, kGrainFrames, both ? surround : nullptr);
        if (sceAudioOutOutput(handle_, grain) < 0)
        {
            errors_.fetch_add(1, std::memory_order_relaxed);
            sys::sleep_us(5000);
            continue;
        }
        if (both && sceAudioOutOutput(surround_handle_, surround) < 0)
            errors_.fetch_add(1, std::memory_order_relaxed);
        grains_.fetch_add(1, std::memory_order_relaxed);
    }
    sceAudioOutOutput(handle_, nullptr); // drain the queued grain
    if (surround_handle_ >= 0)
        sceAudioOutOutput(surround_handle_, nullptr);
}

void AudioOut::stop()
{
    if (running_)
    {
        stop_.store(true);
        pthread_join(thread_, nullptr);
        running_ = false;
    }
    if (handle_ >= 0)
        sceAudioOutClose(handle_);
    if (surround_handle_ >= 0)
        sceAudioOutClose(surround_handle_);
    handle_ = -1;
    surround_handle_ = -1;
    surround_tried_ = false;
}

} // namespace hui::ps5
