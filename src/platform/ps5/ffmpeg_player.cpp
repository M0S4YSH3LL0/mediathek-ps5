// Mediathek - Movie playback on the console: FFmpeg decodes, the app shows and plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/ffmpeg_player.hpp"

#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "media/decode_budget.hpp"
#include "media/decode_pace.hpp"
#include "media/hw_stream.hpp"
#include "media/yuv_picture.hpp"
#include "platform/ps5/av_names.hpp"
#include "platform/ps5/hw_decoder.hpp"
#include "platform/ps5/system.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

extern "C" std::size_t hui_heap_capacity(void);
extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);

namespace hui::ps5
{

namespace
{

using media::PlayerState;

// What the three threads may hold ahead of the screen. Pictures are big
// (8 MB at 1080p), packets are not; the limits keep a 4K remux from eating
// the heap while still riding out a slow drive for a second or two.
constexpr std::size_t kMaxPictures = 4;
// The byte limits follow the heap the app was given (see app_heap.c): with the
// pictures (about 50 MB), the decoder's frames (about 60 MB) and the rest of
// the app (30 MB) the old 128 MiB had nothing to spare for a busy film.
constexpr std::size_t kMiB = 1u << 20;
constexpr std::size_t kMaxPackets = 600;
// FFmpeg's own file reads are 32 KB. On a USB drive behind the sandbox that is
// a system call per 32 KB and a drive that never gets to stream; a few MB at
// a time keeps it busy. The packet queues above are not raised with it: with
// the pictures they already use most of the fixed 128 MiB heap.
constexpr int kReadBuffer = 4 << 20;
constexpr int kMaxWidth = 1920; // pictures are scaled down to fit, never up
constexpr int kMaxHeight = 1080;
// Beyond 1080p the decoder's frames take the room the queues had: a 4K file's
// seconds of packets are cheap to refill, the frames are not. The console gave
// the app 256 MiB, 34 of them in use at start, and one decoder thread of a 4K
// 10-bit film already needs about 190.
constexpr std::size_t kBigVideoPackets = 24 * kMiB;
constexpr std::size_t kBigAudioPackets = 4 * kMiB;
constexpr std::size_t kBigPictures = 3;
constexpr std::size_t kSlack = 16 * kMiB;
constexpr std::size_t kBigSlack = 8 * kMiB;
constexpr int kDecoderThreads = 4;
// The console's default thread stack is too small for its decoder library: an ordinary
// H.264 film overflowed it inside sceVideodec2Decode (EVO Player's finding).
constexpr std::size_t kVideoStack = kMiB;
// Pictures the hardware decoder has finished and the converter has not reached.
constexpr std::size_t kMaxHwFrames = 3;
// A film the hardware cannot read shows it at once: errors or silence, not a bad picture.
// The first few are normal after a random access into the middle of something.
constexpr int kMaxHwErrors = 32;
constexpr int kMaxHwSilence = 128;
// A stream error leaves the decoder refusing everything after it, so it starts again from
// the next key frame (VLC's port). A film that keeps doing it goes to FFmpeg; the count
// starts over once this many pictures came out fine since the last one.
constexpr int kMaxHwResyncs = 5;
constexpr int kHwResyncForgiven = 300;
// How long the sound waits for the first picture. Long, because sound that starts alone
// runs ahead of a picture that then has to catch it up: with 2 s, a 4K film's decoder was
// 4.24 s behind on the console at its start. Only a film with no working picture waits it out.
constexpr int kFirstPictureWaitMs = 15000;
constexpr int kFirstPicturePollMs = 5;
constexpr std::int64_t kSlowStepUs = 200000; // a step this long is worth a line in the log
constexpr int kOutputRate = audio::kSampleRate;

std::size_t max_video_bytes()
{
    const std::size_t heap = hui_heap_capacity();
    return heap >= 256 * kMiB ? 64 * kMiB : heap >= 192 * kMiB ? 24 * kMiB : 12 * kMiB;
}

std::size_t max_audio_bytes()
{
    return hui_heap_capacity() >= 192 * kMiB ? 8 * kMiB : 3 * kMiB;
}

struct Item
{
    AVPacket *packet = nullptr; // null: the file has ended
    int serial = 0;
    int stream = 0;
};

struct Picture
{
    // RGBA, or with `yuv` the hardware decoder's planes as media::copy_planes() writes
    // them, which the graphics chip makes into RGBA. The same buffer either way, so the
    // spare ones serve both.
    std::vector<std::uint8_t> rgba;
    int width = 0; // what is shown
    int height = 0;
    double pts = 0.0;
    int serial = 0;
    bool yuv = false;
    media::YuvLayout layout;
    media::YuvColour colour;
};

// A picture the hardware decoder finished, still in its memory.
struct HwFrame
{
    HwPicture picture;
    double pts = 0.0;
    int serial = 0;
};

using av::audio_name;
using av::layout_name;
using av::video_name;

// The film file behind FFmpeg's reads, with a count of what the drive gave.
struct FileSource
{
    int fd = -1;
    std::atomic<std::int64_t> bytes{0};
    std::atomic<std::int64_t> micros{0};
};

int read_file(void *opaque, std::uint8_t *buffer, int size)
{
    auto *source = static_cast<FileSource *>(opaque);
    const std::int64_t began = sys::monotonic_us();
    const ssize_t got = ::read(source->fd, buffer, static_cast<std::size_t>(size));
    source->micros.fetch_add(sys::monotonic_us() - began, std::memory_order_relaxed);
    if (got < 0)
        return AVERROR(errno);
    if (got == 0)
        return AVERROR_EOF;
    source->bytes.fetch_add(got, std::memory_order_relaxed);
    return static_cast<int>(got);
}

std::int64_t seek_file(void *opaque, std::int64_t offset, int whence)
{
    auto *source = static_cast<FileSource *>(opaque);
    if ((whence & AVSEEK_SIZE) != 0)
    {
        struct stat info = {};
        return ::fstat(source->fd, &info) == 0 ? static_cast<std::int64_t>(info.st_size) : -1;
    }
    const off_t at = ::lseek(source->fd, static_cast<off_t>(offset), whence & ~AVSEEK_FORCE);
    return at < 0 ? -1 : static_cast<std::int64_t>(at);
}

class Lock
{
  public:
    explicit Lock(pthread_mutex_t &mutex) : mutex_(mutex)
    {
        pthread_mutex_lock(&mutex_);
    }
    ~Lock()
    {
        pthread_mutex_unlock(&mutex_);
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

  private:
    pthread_mutex_t &mutex_;
};

// One opened file and the threads working on it. The player makes a new one
// per movie, so nothing has to be reset between movies.
//
// Packets and pictures carry a serial. A seek bumps it, and everything that
// carries an older one is thrown away wherever it is found: that is what lets
// the threads run without stopping each other.
class Session
{
  public:
    Session(std::string path, double start, audio::StreamRing *ring, bool hardware,
            bool gpu_pictures)
        : path_(std::move(path)), ring_(ring), hardware_(hardware), gpu_pictures_(gpu_pictures),
          discard_before_(start)
    {
        pthread_mutex_init(&mutex_, nullptr);
        pthread_cond_init(&wake_, nullptr);
        start_ = start;
        pthread_create(&demux_thread_, nullptr, &Session::demux_entry, this);
        start_with_big_stack(&video_thread_, &Session::video_entry);
        pthread_create(&audio_thread_, nullptr, &Session::audio_entry, this);
    }

    ~Session()
    {
        {
            Lock lock(mutex_);
            stop_ = true;
            pthread_cond_broadcast(&wake_);
        }
        pthread_join(demux_thread_, nullptr);
        pthread_join(video_thread_, nullptr);
        pthread_join(audio_thread_, nullptr);
        clear_queues();
        sws_freeContext(sws_);
        if (video_context_ != nullptr)
            avcodec_free_context(&video_context_);
        if (format_ != nullptr)
            avformat_close_input(&format_);
        if (io_ != nullptr)
        {
            av_freep(&io_->buffer); // FFmpeg may have swapped it for a bigger one
            avio_context_free(&io_);
        }
        if (source_.fd >= 0)
            ::close(source_.fd);
        pthread_cond_destroy(&wake_);
        pthread_mutex_destroy(&mutex_);
    }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // ---- read by the frame thread once opened() is 1 ----
    int opened() const
    {
        return opened_.load(std::memory_order_acquire);
    }
    const std::string &error() const
    {
        return error_;
    }
    const std::string &summary() const
    {
        return summary_;
    }
    double duration() const
    {
        return duration_;
    }
    const std::vector<media::Track> &tracks() const
    {
        return tracks_;
    }
    bool has_video() const
    {
        return video_stream_ >= 0;
    }
    bool has_audio() const
    {
        return !audio_streams_.empty();
    }
    int audio_choice() const
    {
        return audio_choice_.load();
    }

    // ---- frame thread calls ----
    void seek(double seconds)
    {
        Lock lock(mutex_);
        ++serial_;
        seek_target_ = seconds + start_time_;
        seek_pending_ = true;
        discard_before_.store(seconds);
        eof_ = false;
        video_eof_ = false;
        audio_eof_ = false;
        video_ready_.store(false);
        audio_has_.store(false);
        clear_queues_locked();
        pthread_cond_broadcast(&wake_);
    }
    void choose_audio(int index, double position)
    {
        if (index < 0 || index >= static_cast<int>(audio_streams_.size()))
            return;
        audio_choice_.store(index);
        seek(position); // the packets already queued belong to the old track
    }
    void set_paused(bool paused)
    {
        Lock lock(mutex_);
        paused_ = paused;
        pthread_cond_broadcast(&wake_);
    }
    void set_clock(double seconds)
    {
        clock_.store(seconds, std::memory_order_relaxed);
    }

    struct Status
    {
        std::size_t pictures = 0;
        double first_pts = 0.0;
        double second_pts = 0.0;
        bool video_done = false;
        bool audio_done = false;
        bool eof = false;
    };
    Status status()
    {
        Lock lock(mutex_);
        drop_stale_pictures_locked();
        Status s;
        s.pictures = pictures_.size();
        if (!pictures_.empty())
            s.first_pts = pictures_[0].pts;
        if (pictures_.size() > 1)
            s.second_pts = pictures_[1].pts;
        s.video_done = video_eof_;
        s.audio_done = audio_eof_;
        s.eof = eof_;
        return s;
    }
    // Moves the oldest picture out (into the session's upload slot); null if none.
    const Picture *take_picture()
    {
        Lock lock(mutex_);
        drop_stale_pictures_locked();
        if (pictures_.empty())
            return nullptr;
        std::swap(upload_, pictures_.front());
        recycle_locked(std::move(pictures_.front()));
        pictures_.pop_front();
        pthread_cond_broadcast(&wake_);
        return &upload_;
    }
    // Where the sound the speakers are playing right now stands in the film.
    bool audio_clock(double *seconds) const
    {
        if (!audio_has_.load(std::memory_order_acquire) || ring_->available() == 0)
            return false;
        *seconds = audio_end_.load(std::memory_order_relaxed) -
                   static_cast<double>(ring_->available()) / kOutputRate;
        return true;
    }

    // One line for the log: when the picture runs dry, and every few seconds
    // of playing. It tells a drive that
    // cannot keep up (low read rate, empty queues) from a decoder that cannot
    // (full queues, many milliseconds per frame), which want different fixes.
    void log_stats(const char *what, double position, int shown, int skipped)
    {
        std::size_t video_packets = 0, audio_packets = 0, video_bytes = 0, pictures = 0;
        {
            Lock lock(mutex_);
            video_packets = video_queue_.size();
            audio_packets = audio_queue_.size();
            video_bytes = video_bytes_;
            pictures = pictures_.size();
        }
        const std::int64_t read_bytes = source_.bytes.load();
        const std::int64_t read_us = std::max<std::int64_t>(source_.micros.load(), 1);
        const std::int64_t frames = std::max<std::int64_t>(decoded_.load(), 1);
        sys::log(
            "[HUI] %s at %.1fs (%s): %d shown, %d skipped on screen; video queue %zu packets %zu "
            "KB, audio %zu packets, "
            "%zu pictures; drive %.1f MB/s while reading (%lld MB total); "
            "decode+convert %.1f ms/frame over %lld frames, %lld late; opened %.2fs, first "
            "picture %.2fs; video thread: %.1fs waiting for packets, %.1fs for room, %.1fs "
            "converting %lld pictures (%.0f ms each)",
            what, position, on_hardware_.load(std::memory_order_relaxed) ? "hardware" : "software",
            shown, skipped, video_packets, video_bytes >> 10, audio_packets, pictures,
            static_cast<double>(read_bytes) / static_cast<double>(read_us),
            static_cast<long long>(read_bytes >> 20),
            static_cast<double>(decode_us_.load()) / 1000.0 / static_cast<double>(frames),
            static_cast<long long>(decoded_.load()), static_cast<long long>(late_.load()),
            static_cast<double>(opened_us_.load()) / 1e6,
            static_cast<double>(first_picture_us_.load()) / 1e6,
            static_cast<double>(packet_wait_us_.load()) / 1e6,
            static_cast<double>(room_wait_us_.load()) / 1e6,
            static_cast<double>(convert_us_.load()) / 1e6,
            static_cast<long long>(converted_.load()),
            static_cast<double>(convert_us_.load()) / 1000.0 /
                static_cast<double>(std::max<std::int64_t>(converted_.load(), 1)));
    }

  private:
    // ------------------------------------------------------------ open

    bool fail(const char *what, int code)
    {
        char reason[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(code, reason, sizeof(reason));
        error_ = std::string(what) + ": " + reason;
        return false;
    }

    // What the heap has left for the decoder once the player has its share:
    // the packet queues, the pictures for the screen and some slack.
    std::size_t decoder_room() const
    {
        const std::size_t capacity = hui_heap_capacity();
        if (capacity == 0)
            return SIZE_MAX; // the system's malloc, not the fixed heap
        std::size_t live = 0;
        hui_heap_stats(&live, nullptr, nullptr, nullptr);
        const std::size_t player =
            video_limit_ + audio_limit_ + picture_limit_ * kMaxWidth * kMaxHeight * 4 + slack_;
        return capacity > live + player ? capacity - live - player : 0;
    }

    // The console's picture decoder, for a film it can take and FFmpeg cannot play in
    // time. False leaves the film to FFmpeg, which also gets it if this fails later.
    bool open_hardware(const AVCodecParameters *parameters)
    {
        if (!hardware_ || !media::wants_hw_decoder(parameters->width, parameters->height))
            return false;
        media::HwCodec codec = media::HwCodec::h264;
        if (parameters->codec_id == AV_CODEC_ID_HEVC)
            codec = media::HwCodec::hevc;
        else if (parameters->codec_id != AV_CODEC_ID_H264)
            return false;
        const AVPixFmtDescriptor *format =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
        const int depth =
            format != nullptr
                ? format->comp[0].depth
                : (parameters->profile == 2 && codec == media::HwCodec::hevc ? 10 : 8);
        const bool yuv420 =
            format == nullptr || (format->log2_chroma_w == 1 && format->log2_chroma_h == 1);
        const auto mode = media::hw_mode(codec, parameters->profile, depth, yuv420,
                                         parameters->width, parameters->height);
        if (!mode.has_value())
        {
            sys::log("[HUI] hw decoder: not for this film (profile %d, %d-bit, %dx%d)",
                     parameters->profile, depth, parameters->width, parameters->height);
            return false;
        }
        hw_ = HwDecoder::open(*mode);
        if (hw_ == nullptr)
            return false;
        on_hardware_.store(true, std::memory_order_relaxed);
        hevc_ = codec == media::HwCodec::hevc;
        hw_visible_width_ = parameters->width;
        hw_visible_height_ = parameters->height;
        AVStream *stream = format_->streams[video_stream_];
        fit_to_screen(av_guess_sample_aspect_ratio(format_, stream, nullptr), parameters->width,
                      parameters->height, &hw_width_, &hw_height_);
        const AVRational rate = av_guess_frame_rate(format_, stream, nullptr);
        hw_frame_time_ = rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 0.04;
        return true;
    }

    // What the shader needs to show the film's colours and light on an ordinary screen.
    static media::YuvColour colour_of(const AVCodecParameters *parameters)
    {
        media::YuvColour colour;
        colour.full_range = parameters->color_range == AVCOL_RANGE_JPEG;
        if (parameters->color_space == AVCOL_SPC_BT2020_NCL ||
            parameters->color_space == AVCOL_SPC_BT2020_CL)
            colour.matrix = media::YuvMatrix::bt2020;
        else if (parameters->color_space == AVCOL_SPC_BT470BG ||
                 parameters->color_space == AVCOL_SPC_SMPTE170M)
            colour.matrix = media::YuvMatrix::bt601;
        colour.wide_gamut = parameters->color_primaries == AVCOL_PRI_BT2020;
        if (parameters->color_trc == AVCOL_TRC_SMPTE2084)
            colour.transfer = media::Transfer::pq;
        else if (parameters->color_trc == AVCOL_TRC_ARIB_STD_B67)
            colour.transfer = media::Transfer::hlg;
        float content = 0.0f;
        float mastering = 0.0f;
        if (const AVPacketSideData *light =
                av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data,
                                        AV_PKT_DATA_CONTENT_LIGHT_LEVEL))
            content = static_cast<float>(
                reinterpret_cast<const AVContentLightMetadata *>(light->data)->MaxCLL);
        if (const AVPacketSideData *display =
                av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data,
                                        AV_PKT_DATA_MASTERING_DISPLAY_METADATA))
        {
            const auto *metadata =
                reinterpret_cast<const AVMasteringDisplayMetadata *>(display->data);
            if (metadata->has_luminance != 0)
                mastering = static_cast<float>(av_q2d(metadata->max_luminance));
        }
        // HLG has no peak of its own: it is shown as on the 1000-nit screen it assumes.
        colour.peak_nits = colour.transfer == media::Transfer::hlg
                               ? 1000.0f
                               : media::hdr_peak_nits(content, mastering);
        if (colour.transfer != media::Transfer::sdr)
            sys::log("[HUI] hdr %s, %s, peak %.0f nits (content %.0f, mastering %.0f)",
                     colour.transfer == media::Transfer::pq ? "PQ" : "HLG",
                     colour.wide_gamut ? "BT.2020" : "BT.709", colour.peak_nits, content,
                     mastering);
        return colour;
    }

    // The player's own limits stay as they are here: the frames do not touch the heap.
    bool open_software(const AVCodecParameters *parameters)
    {
        const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
        if (codec == nullptr)
            return fail(i18n::tr(i18n::Text::err_no_picture_decoder), AVERROR_DECODER_NOT_FOUND);
        video_context_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(video_context_, parameters);
        // Every frame thread holds frames of its own; at 4K that is more than
        // the fixed heap has left, and running it out ends the app.
        if (parameters->width > kMaxWidth)
        {
            Lock lock(mutex_); // the hardware's failure brings this here mid-film
            video_limit_ = std::min(video_limit_, kBigVideoPackets);
            audio_limit_ = std::min(audio_limit_, kBigAudioPackets);
            picture_limit_ = kBigPictures;
            slack_ = kBigSlack;
        }
        const AVPixFmtDescriptor *format =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
        const int depth = format != nullptr ? format->comp[0].depth : 16;
        const int bytes_per_sample = depth > 8 ? 2 : 1;
        const int threads =
            media::decoder_threads(parameters->width, parameters->height, bytes_per_sample,
                                   kDecoderThreads, decoder_room());
        const bool slices = threads == 1 && kDecoderThreads > 1;
        sys::log("[HUI] decoder %dx%d %d-bit threads=%d (of %d)%s need=%zu room=%zu",
                 parameters->width, parameters->height, depth, threads, kDecoderThreads,
                 slices ? " +slice threads" : "",
                 media::decoder_bytes(parameters->width, parameters->height, bytes_per_sample,
                                      std::max(threads, 1)),
                 decoder_room());
        if (threads == 0)
        {
            error_ = i18n::tr(i18n::Text::err_too_big); // no system reason to add
            return false;
        }
        // One frame thread is all a 4K film may have. Slice threads hold no
        // frames of their own (measured: the same memory, 94 instead of 32 fps)
        // and help every stream coded for them, which is x265's default (WPP);
        // a stream without them decodes on one thread either way.
        pace_.allow_thinning(threads == 1);
        video_context_->thread_count = slices ? kDecoderThreads : threads;
        video_context_->thread_type = slices ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE;
        // Not AV_CODEC_FLAG2_FAST: its non-conforming shortcuts leave
        // blocks and smears in busy, high-bitrate H.264 for a few percent.
        // Beyond 1080p the CPU is the limit: skip the in-loop filter on
        // frames nothing else is predicted from.
        if (parameters->width > kMaxWidth)
            video_context_->skip_loop_filter = AVDISCARD_NONREF;
        const int code = avcodec_open2(video_context_, codec, nullptr);
        if (code < 0)
            return fail(i18n::tr(i18n::Text::err_start_decoder), code);
        return true;
    }

    bool open_input()
    {
        AVDictionary *options = nullptr;
        // A drive can take a while to spin up; the default probe reads little.
        av_dict_set(&options, "analyzeduration", "6000000", 0);
        av_dict_set(&options, "probesize", "20000000", 0);
        source_.fd = ::open(path_.c_str(), O_RDONLY);
        if (source_.fd < 0)
        {
            av_dict_free(&options);
            return fail(i18n::tr(i18n::Text::err_open_file), AVERROR(errno));
        }
        auto *buffer = static_cast<std::uint8_t *>(av_malloc(kReadBuffer));
        io_ = buffer != nullptr ? avio_alloc_context(buffer, kReadBuffer, 0, &source_, &read_file,
                                                     nullptr, &seek_file)
                                : nullptr;
        format_ = io_ != nullptr ? avformat_alloc_context() : nullptr;
        if (format_ == nullptr)
        {
            if (io_ == nullptr)
                av_free(buffer); // otherwise the context owns it
            av_dict_free(&options);
            return fail(i18n::tr(i18n::Text::err_open_file), AVERROR(ENOMEM));
        }
        format_->pb = io_;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;
        int code = avformat_open_input(&format_, path_.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (code < 0)
            return fail(i18n::tr(i18n::Text::err_open_file), code);
        code = avformat_find_stream_info(format_, nullptr);
        if (code < 0)
            return fail(i18n::tr(i18n::Text::err_read_file), code);

        start_time_ = format_->start_time != AV_NOPTS_VALUE
                          ? static_cast<double>(format_->start_time) / AV_TIME_BASE
                          : 0.0;
        duration_ = format_->duration != AV_NOPTS_VALUE
                        ? static_cast<double>(format_->duration) / AV_TIME_BASE
                        : 0.0;

        video_stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        const int best_audio = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        int first = 0;
        for (unsigned i = 0; i < format_->nb_streams; ++i)
        {
            const AVStream *stream = format_->streams[i];
            if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;
            if (static_cast<int>(i) == best_audio)
                first = static_cast<int>(audio_streams_.size());
            audio_streams_.push_back(static_cast<int>(i));
            media::Track track;
            track.index = static_cast<int>(audio_streams_.size()) - 1;
            const AVDictionaryEntry *language =
                av_dict_get(stream->metadata, "language", nullptr, 0);
            const int channels = stream->codecpar->ch_layout.nb_channels;
            char label[96];
            std::snprintf(label, sizeof(label), "%s%s%s %s",
                          language != nullptr ? language->value : "und", " \xC2\xB7 ",
                          audio_name(stream->codecpar->codec_id).c_str(), layout_name(channels));
            track.label = label;
            tracks_.push_back(track);
        }
        audio_choice_.store(first);
        if (video_stream_ < 0 && audio_streams_.empty())
            return fail(i18n::tr(i18n::Text::err_nothing_to_play), AVERROR_STREAM_NOT_FOUND);

        if (video_stream_ >= 0)
        {
            const AVCodecParameters *parameters = format_->streams[video_stream_]->codecpar;
            colour_ = colour_of(parameters);
            if (!open_hardware(parameters) && !open_software(parameters))
                return false;
            char text[96];
            std::snprintf(text, sizeof(text), "%s %dx%d", video_name(parameters->codec_id),
                          parameters->width, parameters->height);
            summary_ = text;
        }
        if (!audio_streams_.empty())
        {
            const AVCodecParameters *parameters =
                format_->streams[audio_streams_[static_cast<std::size_t>(first)]]->codecpar;
            if (!summary_.empty())
                summary_ += " \xC2\xB7 ";
            summary_ += audio_name(parameters->codec_id) + " " +
                        layout_name(parameters->ch_layout.nb_channels);
        }
        return true;
    }

    // --------------------------------------------------------- demuxing

    bool queues_full_locked() const
    {
        return video_bytes_ > video_limit_ || audio_bytes_ > audio_limit_ ||
               video_queue_.size() > kMaxPackets || audio_queue_.size() > kMaxPackets;
    }

    void clear_queues_locked()
    {
        for (std::deque<Item> *queue : {&video_queue_, &audio_queue_})
        {
            for (Item &item : *queue)
                av_packet_free(&item.packet);
            queue->clear();
        }
        video_bytes_ = 0;
        audio_bytes_ = 0;
        for (Picture &picture : pictures_)
            recycle_locked(std::move(picture));
        pictures_.clear();
        for (const HwFrame &frame : hw_frames_)
            if (hw_ != nullptr)
                hw_->release(frame.picture);
        hw_frames_.clear();
        pthread_cond_broadcast(&wake_); // a video thread may wait for a picture buffer
    }
    void clear_queues()
    {
        Lock lock(mutex_);
        clear_queues_locked();
    }

    void recycle_locked(Picture &&picture)
    {
        if (spare_.size() < picture_limit_ + 1 && !picture.rgba.empty())
            spare_.push_back(std::move(picture.rgba));
    }
    void drop_stale_pictures_locked()
    {
        while (!pictures_.empty() && pictures_.front().serial != serial_.load())
        {
            recycle_locked(std::move(pictures_.front()));
            pictures_.pop_front();
        }
    }

    static void *demux_entry(void *self)
    {
        static_cast<Session *>(self)->demux();
        return nullptr;
    }

    void demux()
    {
        const bool ok = open_input();
        if (!ok)
        {
            opened_.store(2, std::memory_order_release);
            Lock lock(mutex_);
            pthread_cond_broadcast(&wake_);
            return;
        }
        {
            Lock lock(mutex_);
            if (start_ > 0.5)
            {
                seek_target_ = start_ + start_time_;
                seek_pending_ = true;
            }
            opened_us_.store(sys::monotonic_us() - began_us_);
            opened_.store(1, std::memory_order_release);
            pthread_cond_broadcast(&wake_);
        }
        int serial = 0;
        for (;;)
        {
            bool do_seek = false;
            double target = 0.0;
            {
                Lock lock(mutex_);
                while (!stop_ && !seek_pending_ && (eof_ || queues_full_locked()))
                    pthread_cond_wait(&wake_, &mutex_);
                if (stop_)
                    break;
                do_seek = seek_pending_;
                target = seek_target_;
                seek_pending_ = false;
                serial = serial_.load();
            }
            if (do_seek)
            {
                const auto stamp = static_cast<std::int64_t>(target * AV_TIME_BASE);
                const std::int64_t began = sys::monotonic_us();
                const std::int64_t read_before = source_.bytes.load(std::memory_order_relaxed);
                // Backward: the nearest key frame before the target, so the
                // decoder can run forward to it without grey pictures.
                if (av_seek_frame(format_, -1, stamp, AVSEEK_FLAG_BACKWARD) < 0)
                    av_seek_frame(format_, -1, stamp, 0);
                log_slow_seek(target - start_time_, sys::monotonic_us() - began,
                              source_.bytes.load(std::memory_order_relaxed) - read_before);
            }
            AVPacket *packet = av_packet_alloc();
            const int code = av_read_frame(format_, packet);
            Lock lock(mutex_);
            if (stop_)
            {
                av_packet_free(&packet);
                break;
            }
            if (serial != serial_.load())
            {
                av_packet_free(&packet); // read before a seek; it is not wanted now
                continue;
            }
            if (code < 0)
            {
                av_packet_free(&packet);
                eof_ = true; // an unreadable file ends the movie the same way
                video_queue_.push_back({nullptr, serial, 0});
                audio_queue_.push_back({nullptr, serial, 0});
                pthread_cond_broadcast(&wake_);
                continue;
            }
            const int size = packet->size;
            if (packet->stream_index == video_stream_)
            {
                video_queue_.push_back({packet, serial, packet->stream_index});
                video_bytes_ += static_cast<std::size_t>(size);
            }
            else if (!audio_streams_.empty() &&
                     packet->stream_index ==
                         audio_streams_[static_cast<std::size_t>(audio_choice_.load())])
            {
                audio_queue_.push_back({packet, serial, packet->stream_index});
                audio_bytes_ += static_cast<std::size_t>(size);
            }
            else
            {
                av_packet_free(&packet);
                continue;
            }
            pthread_cond_broadcast(&wake_);
        }
    }

    // A seek that read a lot or took long names its container and how many seek points the
    // file gave: a file with no index (an MKV without Cues, mostly) is read from where the last
    // known point is up to the target, so the farther the jump the longer the wait, and that
    // is the log line which tells it from a slow drive.
    void log_slow_seek(double to, std::int64_t micros, std::int64_t bytes) const
    {
        constexpr std::int64_t kSlowUs = 300 * 1000;
        constexpr std::int64_t kBigRead = std::int64_t{16} << 20;
        if (micros < kSlowUs && bytes < kBigRead)
            return;
        const int stream = video_stream_ >= 0 ? video_stream_ : 0;
        sys::log("[HUI] seek to %.1fs took %.2fs and read %lld MB (%s, %d seek points)", to,
                 static_cast<double>(micros) / 1e6, static_cast<long long>(bytes >> 20),
                 format_->iformat != nullptr ? format_->iformat->name : "?",
                 format_->nb_streams > 0
                     ? avformat_index_get_entries_count(format_->streams[stream])
                     : 0);
    }

    // ----------------------------------------------------------- pictures

    static void *video_entry(void *self)
    {
        static_cast<Session *>(self)->video();
        return nullptr;
    }

    bool wait_opened()
    {
        Lock lock(mutex_);
        while (!stop_ && opened_.load() == 0)
            pthread_cond_wait(&wake_, &mutex_);
        return !stop_ && opened_.load() == 1;
    }

    // The next packet for the picture decoder; false when the session is stopping.
    bool next_video_item(Item *item)
    {
        const std::int64_t waiting = sys::monotonic_us();
        Lock lock(mutex_);
        while (!stop_ && video_queue_.empty())
            pthread_cond_wait(&wake_, &mutex_);
        packet_wait_us_.fetch_add(sys::monotonic_us() - waiting, std::memory_order_relaxed);
        if (stop_)
            return false;
        *item = video_queue_.front();
        video_queue_.pop_front();
        if (item->packet != nullptr)
            video_bytes_ -= static_cast<std::size_t>(item->packet->size);
        pthread_cond_broadcast(&wake_);
        return true;
    }

    void video()
    {
        if (!wait_opened() || video_stream_ < 0)
            return;
        if (hw_ != nullptr)
        {
            const bool stopping = hardware_video();
            on_hardware_.store(false, std::memory_order_relaxed);
            std::unique_ptr<HwDecoder> leaving;
            {
                // Other threads clear the queue under the lock and release into the
                // decoder; it leaves under the same lock, with nothing in the queue.
                Lock lock(mutex_);
                hw_frames_.clear();
                leaving = std::move(hw_);
            }
            leaving.reset();
            if (stopping)
                return;
            // The hardware gave up on this film: FFmpeg takes over from where it is.
            if (!open_software(format_->streams[video_stream_]->codecpar))
            {
                sys::log("[HUI] hw decoder: no software decoder to fall back on");
                Lock lock(mutex_);
                video_eof_ = true;
                return;
            }
            sys::log("[HUI] hw decoder: FFmpeg takes over");
            seek(clock_.load(std::memory_order_relaxed));
        }
        AVFrame *frame = av_frame_alloc();
        int decoder_serial = -1;
        for (;;)
        {
            Item item;
            if (!next_video_item(&item))
                break;
            if (item.serial != serial_.load())
            {
                av_packet_free(&item.packet);
                continue;
            }
            if (item.serial != decoder_serial)
            {
                avcodec_flush_buffers(video_context_);
                decoder_serial = item.serial;
                pace_.reset();
                apply_pace(media::Skip::none);
            }
            const bool ended = item.packet == nullptr; // av_packet_free clears the pointer
            if (!ended)
                keep_pace(item.packet);
            const std::int64_t began = sys::monotonic_us();
            avcodec_send_packet(video_context_, item.packet); // null drains at the end of the file
            av_packet_free(&item.packet);
            while (avcodec_receive_frame(video_context_, frame) == 0)
            {
                decoded_.fetch_add(1, std::memory_order_relaxed);
                show(frame, decoder_serial);
                av_frame_unref(frame);
            }
            decode_us_.fetch_add(sys::monotonic_us() - began, std::memory_order_relaxed);
            if (ended)
            {
                Lock lock(mutex_);
                if (decoder_serial == serial_.load())
                    video_eof_ = true;
                pthread_cond_broadcast(&wake_);
            }
        }
        av_frame_free(&frame);
    }

    // Leaves frames out when the decoder is behind the sound (see DecodePace). The
    // lag is that of the packet's picture; the clock is where the sound is.
    void keep_pace(const AVPacket *packet)
    {
        apply_pace(next_pace(packet));
    }

    media::Skip next_pace(const AVPacket *packet)
    {
        const std::int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (stamp == AV_NOPTS_VALUE)
            return media::Skip::none;
        const double base = av_q2d(format_->streams[video_stream_]->time_base);
        const double lag = clock_.load(std::memory_order_relaxed) -
                           (static_cast<double>(stamp) * base - start_time_);
        const media::Skip skip =
            pace_.next(lag, (packet->flags & AV_PKT_FLAG_KEY) != 0, video_ready_.load());
        // A jump leaves the picture standing while the sound runs, so the log names each one.
        if (skip == media::Skip::to_key_frame && !jump_logged_)
            sys::log("[HUI] picture %.2fs behind the sound at %.2fs: jumping to a key frame", lag,
                     clock_.load(std::memory_order_relaxed));
        jump_logged_ = skip == media::Skip::to_key_frame;
        return skip;
    }

    void apply_pace(media::Skip skip)
    {
        video_context_->skip_frame = skip == media::Skip::none            ? AVDISCARD_DEFAULT
                                     : skip == media::Skip::non_reference ? AVDISCARD_NONREF
                                                                          : AVDISCARD_NONKEY;
    }

    // Converts one decoded picture and queues it for the screen.
    void show(const AVFrame *frame, int serial)
    {
        const AVStream *stream = format_->streams[video_stream_];
        const double base = av_q2d(stream->time_base);
        double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                         ? static_cast<double>(frame->best_effort_timestamp) * base - start_time_
                         : last_video_pts_ + 0.04;
        last_video_pts_ = pts;
        // After a seek the decoder runs from the key frame; those pictures
        // are before the point the viewer asked for.
        if (pts < discard_before_.load() - 0.001)
            return;
        // Behind the sound by more than a blink: skip the conversion, which is
        // the expensive part, and let the decoder catch up.
        if (pts < clock_.load(std::memory_order_relaxed) - 0.3 && video_ready_.load())
        {
            late_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        int width = 0;
        int height = 0;
        fit_to_screen(av_guess_sample_aspect_ratio(format_, const_cast<AVStream *>(stream),
                                                   const_cast<AVFrame *>(frame)),
                      frame->width, frame->height, &width, &height);
        if (colour_.transfer != media::Transfer::sdr &&
            gpu_pictures_.load(std::memory_order_relaxed) &&
            show_planes(frame, pts, serial, width, height))
            return;

        sws_ = sws_getCachedContext(sws_, frame->width, frame->height,
                                    static_cast<AVPixelFormat>(frame->format), width, height,
                                    AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ == nullptr)
            return;
        int matrix = SWS_CS_ITU709;
        if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL)
            matrix = SWS_CS_BT2020;
        else if (frame->colorspace == AVCOL_SPC_BT470BG ||
                 frame->colorspace == AVCOL_SPC_SMPTE170M ||
                 (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height < 720))
            matrix = SWS_CS_ITU601;
        sws_setColorspaceDetails(sws_, sws_getCoefficients(matrix),
                                 frame->color_range == AVCOL_RANGE_JPEG,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

        Picture picture;
        if (!wait_for_room(serial, &picture))
            return;
        picture.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        std::uint8_t *planes[4] = {picture.rgba.data(), nullptr, nullptr, nullptr};
        const int strides[4] = {width * 4, 0, 0, 0};
        const std::int64_t converting = sys::monotonic_us();
        sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, planes, strides);
        convert_us_.fetch_add(sys::monotonic_us() - converting, std::memory_order_relaxed);
        converted_.fetch_add(1, std::memory_order_relaxed);
        picture.width = width;
        picture.height = height;
        picture.pts = pts;
        picture.serial = serial;
        queue_picture(std::move(picture));
    }

    // HDR goes to the graphics chip as planes, like the hardware decoder's pictures,
    // because swscale cannot tone-map: its RGBA of a PQ or HLG film is grey and pale.
    // False for a layout copy_planes() does not know; swscale then does what it can.
    bool show_planes(const AVFrame *frame, double pts, int serial, int width, int height)
    {
        const auto format = static_cast<AVPixelFormat>(frame->format);
        if (format != AV_PIX_FMT_YUV420P10LE && format != AV_PIX_FMT_YUV420P)
        {
            if (!planes_refused_)
                sys::log("[HUI] hdr picture in %s: converted without tone mapping",
                         av_get_pix_fmt_name(format));
            planes_refused_ = true;
            return false;
        }
        Picture picture;
        if (!wait_for_room(serial, &picture))
            return true; // not wanted any more, which is not the layout's fault
        media::YuvPicture source;
        source.luma = frame->data[0];
        source.pitch = static_cast<std::size_t>(frame->linesize[0]);
        source.chroma = frame->data[1];
        source.chroma_v = frame->data[2];
        source.chroma_pitch = static_cast<std::size_t>(frame->linesize[1]);
        source.width = frame->width;
        source.height = frame->height;
        source.wide = format == AV_PIX_FMT_YUV420P10LE; // its 10 bits at the bottom: shift 0
        const std::int64_t converting = sys::monotonic_us();
        picture.yuv = true;
        picture.layout = media::yuv_layout(source, width, height);
        picture.colour = colour_;
        picture.rgba.resize(media::yuv_bytes(picture.layout));
        media::copy_planes(source, picture.layout, picture.rgba.data());
        convert_us_.fetch_add(sys::monotonic_us() - converting, std::memory_order_relaxed);
        converted_.fetch_add(1, std::memory_order_relaxed);
        picture.width = width;
        picture.height = height;
        picture.pts = pts;
        picture.serial = serial;
        queue_picture(std::move(picture));
        return true;
    }

    // Pictures are never scaled up, and always fit the screen's canvas.
    static void fit_to_screen(AVRational aspect, int source_width, int source_height, int *width,
                              int *height)
    {
        int wide = source_width;
        if (aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den)
            wide = static_cast<int>(static_cast<double>(wide) * aspect.num / aspect.den);
        const double fit = std::min({1.0, static_cast<double>(kMaxWidth) / wide,
                                     static_cast<double>(kMaxHeight) / source_height});
        *width = std::max(2, static_cast<int>(wide * fit) & ~1);
        *height = std::max(2, static_cast<int>(source_height * fit) & ~1);
    }

    // Waits until the screen's queue has room and gives `picture` a used buffer if there
    // is one; false when the picture is not wanted any more.
    bool wait_for_room(int serial, Picture *picture)
    {
        const std::int64_t waiting = sys::monotonic_us();
        Lock lock(mutex_);
        while (!stop_ && serial == serial_.load() && pictures_.size() >= picture_limit_)
            pthread_cond_wait(&wake_, &mutex_);
        room_wait_us_.fetch_add(sys::monotonic_us() - waiting, std::memory_order_relaxed);
        if (stop_ || serial != serial_.load())
            return false;
        if (!spare_.empty())
        {
            picture->rgba = std::move(spare_.back());
            spare_.pop_back();
        }
        return true;
    }

    void queue_picture(Picture &&picture)
    {
        Lock lock(mutex_);
        if (stop_ || picture.serial != serial_.load())
            return;
        pictures_.push_back(std::move(picture));
        std::int64_t none = 0;
        if (first_picture_us_.compare_exchange_strong(none, sys::monotonic_us() - began_us_))
            sys::log("[HUI] first picture after %.2fs",
                     static_cast<double>(first_picture_us_) / 1e6);
        video_ready_.store(true);
        pthread_cond_broadcast(&wake_);
    }

    // ------------------------------------------------- the hardware's pictures
    //
    // Two threads, so that neither waits for the other: the decoder's call takes about as
    // long as a frame lasts at 4K60, and so can the conversion on a loaded console.

    void start_with_big_stack(pthread_t *thread, void *(*entry)(void *))
    {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, kVideoStack);
        pthread_create(thread, &attributes, entry, this);
        pthread_attr_destroy(&attributes);
    }

    // A new decoder and filter for a stream that starts here: after a seek or a jump.
    bool restart_hardware(bool renew)
    {
        hw_stamps_.reset();
        hw_entry_.restart();
        hw_errors_ = 0;
        hw_silence_ = 0;
        hw_h264_need_key_ = false;
        if (renew && !hw_->renew())
            return false;
        return build_annexb_filter();
    }

    // The decoder wants start codes where a file holds lengths. The filter is built anew
    // each time because only a new one puts the parameter sets in front of the first key
    // frame, and a decoder that starts without them never starts at all.
    bool build_annexb_filter()
    {
        av_bsf_free(&annexb_);
        const AVBitStreamFilter *filter =
            av_bsf_get_by_name(hevc_ ? "hevc_mp4toannexb" : "h264_mp4toannexb");
        if (filter == nullptr || av_bsf_alloc(filter, &annexb_) < 0)
            return false;
        const AVStream *stream = format_->streams[video_stream_];
        if (avcodec_parameters_copy(annexb_->par_in, stream->codecpar) < 0)
            return false;
        annexb_->time_base_in = stream->time_base;
        return av_bsf_init(annexb_) >= 0;
    }

    // Runs until the session stops (true) or the hardware gives up on the film (false).
    bool hardware_video()
    {
        pthread_t converter{};
        start_with_big_stack(&converter, &Session::convert_entry);
        AVPacket *unit = av_packet_alloc();
        pace_.allow_thinning(false); // the hardware works on several frames at once
        bool stopping = true;
        int decoder_serial = -1;
        for (;;)
        {
            Item item;
            if (!next_video_item(&item))
                break;
            if (item.serial != serial_.load())
            {
                av_packet_free(&item.packet);
                continue;
            }
            if (item.serial != decoder_serial)
            {
                if (!restart_hardware(decoder_serial >= 0))
                {
                    av_packet_free(&item.packet);
                    stopping = false;
                    break;
                }
                pace_.reset();
                hw_jumping_ = false;
                decoder_serial = item.serial;
            }
            if (item.packet == nullptr)
            {
                finish_hardware(decoder_serial);
                continue;
            }
            const bool fine = feed_hardware(item.packet, unit, decoder_serial);
            av_packet_free(&item.packet);
            if (!fine)
            {
                stopping = false;
                break;
            }
        }
        {
            Lock lock(mutex_);
            hw_finished_ = true;
            // Leaving a film also ends in `false` from below; that is no failure of the hardware.
            stopping = stopping || stop_;
            pthread_cond_broadcast(&wake_);
        }
        pthread_join(converter, nullptr); // it reads the decoder's memory
        av_packet_free(&unit);
        av_bsf_free(&annexb_);
        return stopping;
    }

    // False when the hardware cannot go on with this film.
    bool feed_hardware(AVPacket *packet, AVPacket *unit, int serial)
    {
        const media::Skip skip = next_pace(packet);
        if (skip == media::Skip::to_key_frame)
        {
            hw_jumping_ = true;
            return true;
        }
        if (hw_jumping_)
        {
            // A key frame near the sound. What came before it was never submitted, so
            // this is a random access like any seek.
            hw_jumping_ = false;
            if (!restart_hardware(true))
                return false;
        }
        if (hw_resync_)
        {
            hw_resync_ = false;
            if (++hw_resyncs_ > kMaxHwResyncs)
            {
                sys::log("[HUI] hw decoder: stream errors keep coming");
                return false;
            }
            if (!restart_hardware(true))
                return false;
            hw_entry_.refused(); // HEVC: from the next key frame
            hw_h264_need_key_ = !hevc_;
            hw_since_resync_ = 0;
            sys::log("[HUI] hw decoder: stream error, starting again at the next key frame "
                     "(%d of %d)",
                     hw_resyncs_, kMaxHwResyncs);
        }
        const std::int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        const double base = av_q2d(format_->streams[video_stream_]->time_base);
        const double pts = stamp != AV_NOPTS_VALUE ? static_cast<double>(stamp) * base - start_time_
                                                   : last_video_pts_ + hw_frame_time_;
        if (av_bsf_send_packet(annexb_, packet) < 0)
            return true; // one bad packet, not a bad film
        while (av_bsf_receive_packet(annexb_, unit) == 0)
        {
            const bool fine = submit_unit(unit, pts, serial);
            av_packet_unref(unit);
            if (!fine)
                return false;
        }
        return true;
    }

    bool submit_unit(const AVPacket *unit, double pts, int serial)
    {
        const auto size = static_cast<std::size_t>(unit->size);
        if (size == 0 || size > HwDecoder::input_capacity())
            return true;
        const int type = hevc_ ? media::hevc_picture_type(unit->data, size) : -1;
        if (hevc_ && !hw_entry_.admit(type))
            return true;
        if (hw_h264_need_key_)
        {
            if (!media::h264_is_key(unit->data, size))
                return true;
            hw_h264_need_key_ = false;
        }
        std::uint8_t *input = hw_->next_input();
        std::size_t used = size;
        if (hevc_)
            used = media::clean_hevc_unit(unit->data, size, input);
        else
            std::memcpy(input, unit->data, size);
        if (used == 0)
            return true;
        HwPicture picture;
        HwDecoder::Result result = HwDecoder::Result::no_room;
        std::int64_t decoding = 0;
        while (result == HwDecoder::Result::no_room)
        {
            const std::int64_t began = sys::monotonic_us();
            result = hw_->decode(used, &picture);
            decoding += sys::monotonic_us() - began;
            if (result == HwDecoder::Result::no_room && !wait_for_picture_buffer(serial))
                return !stop_ && hw_->can_free_room();
        }
        decode_us_.fetch_add(decoding, std::memory_order_relaxed);
        if (decoding > kSlowStepUs)
            sys::log("[HUI] hw decoder: slow decode %lld ms at %.2fs",
                     static_cast<long long>(decoding / 1000), pts);
        if (result == HwDecoder::Result::broken)
        {
            hw_resync_ = true;
            return true;
        }
        if (result == HwDecoder::Result::refused)
        {
            if (type >= 16 && type <= 23)
                hw_entry_.refused();
            return ++hw_errors_ <= kMaxHwErrors;
        }
        hw_stamps_.push(pts);
        if (result == HwDecoder::Result::picture)
        {
            hw_errors_ = 0;
            hw_silence_ = 0;
            if (++hw_since_resync_ >= kHwResyncForgiven)
                hw_resyncs_ = 0;
            return hand_over(picture, serial);
        }
        return ++hw_silence_ <= kMaxHwSilence;
    }

    // True, unless the session is stopping.
    bool hand_over(const HwPicture &picture, int serial)
    {
        double pts = 0.0;
        if (!hw_stamps_.take(&pts))
            pts = last_video_pts_ + hw_frame_time_;
        last_video_pts_ = pts;
        decoded_.fetch_add(1, std::memory_order_relaxed);
        Lock lock(mutex_);
        while (!stop_ && serial == serial_.load() && hw_frames_.size() >= kMaxHwFrames)
            pthread_cond_wait(&wake_, &mutex_);
        if (stop_ || serial != serial_.load())
        {
            hw_->release(picture);
            return !stop_;
        }
        hw_frames_.push_back({picture, pts, serial});
        pthread_cond_broadcast(&wake_);
        return true;
    }

    // Every picture buffer is held. The player's own come back once the converter has
    // copied them out; false when the session stops or seeks first, or when the decoder
    // holds them all (nothing will come back then).
    bool wait_for_picture_buffer(int serial)
    {
        Lock lock(mutex_);
        while (!stop_ && serial == serial_.load() && hw_->can_free_room())
        {
            if (hw_->has_room())
                return true;
            pthread_cond_wait(&wake_, &mutex_);
        }
        return false;
    }

    // The file has ended: what the decoder still holds, then wait for the converter.
    void finish_hardware(int serial)
    {
        for (int i = 0; i < kMaxHwSilence; ++i)
        {
            HwPicture picture;
            HwDecoder::Result result = hw_->drain(&picture);
            if (result == HwDecoder::Result::no_room && wait_for_picture_buffer(serial))
                result = hw_->drain(&picture);
            if (result != HwDecoder::Result::picture || !hand_over(picture, serial))
                break;
        }
        Lock lock(mutex_);
        while (!stop_ && serial == serial_.load() && (!hw_frames_.empty() || hw_converting_))
            pthread_cond_wait(&wake_, &mutex_);
        if (serial == serial_.load())
            video_eof_ = true;
        pthread_cond_broadcast(&wake_);
    }

    static void *convert_entry(void *self)
    {
        static_cast<Session *>(self)->convert_frames();
        return nullptr;
    }

    void convert_frames()
    {
        for (;;)
        {
            HwFrame frame;
            {
                Lock lock(mutex_);
                hw_converting_ = false;
                pthread_cond_broadcast(&wake_);
                while (!stop_ && !hw_finished_ && hw_frames_.empty())
                    pthread_cond_wait(&wake_, &mutex_);
                if (stop_ || hw_finished_)
                    break;
                frame = hw_frames_.front();
                hw_frames_.pop_front();
                hw_converting_ = true;
                pthread_cond_broadcast(&wake_);
            }
            if (frame.serial == serial_.load())
                convert_frame(frame);
            hw_->release(frame.picture); // the lock and broadcast at the top tell the video thread
        }
    }

    void convert_frame(const HwFrame &frame)
    {
        if (frame.pts < discard_before_.load() - 0.001)
            return;
        if (frame.pts < clock_.load(std::memory_order_relaxed) - 0.3 && video_ready_.load())
        {
            late_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Picture picture;
        if (!wait_for_room(frame.serial, &picture))
            return;
        if (frame.picture.wide && hw_shift_ < 0)
        {
            // One row across the middle of the first picture: it says where the bits are.
            const std::size_t row = static_cast<std::size_t>(frame.picture.height / 2);
            hw_shift_ = media::ten_bit_shift(reinterpret_cast<const std::uint16_t *>(
                                                 frame.picture.luma + row * frame.picture.pitch),
                                             static_cast<std::size_t>(frame.picture.width));
            sys::log("[HUI] hw decoder: 10-bit samples shifted by %d", hw_shift_);
        }
        media::YuvPicture source;
        source.luma = frame.picture.luma;
        source.chroma = frame.picture.chroma;
        source.pitch = frame.picture.pitch;
        source.width = std::min(hw_visible_width_, frame.picture.width);
        source.height = std::min(hw_visible_height_, frame.picture.height);
        source.wide = frame.picture.wide;
        source.shift = frame.picture.wide ? std::max(hw_shift_, 0) : 0;
        const std::int64_t converting = sys::monotonic_us();
        picture.yuv = gpu_pictures_.load(std::memory_order_relaxed);
        if (picture.yuv)
        {
            // Only a copy here: the graphics chip converts, and tone maps HDR on the way.
            picture.layout = media::yuv_layout(source, hw_width_, hw_height_);
            picture.colour = colour_;
            picture.rgba.resize(media::yuv_bytes(picture.layout));
            media::copy_planes(source, picture.layout, picture.rgba.data());
        }
        else
        {
            picture.rgba.resize(static_cast<std::size_t>(hw_width_) *
                                static_cast<std::size_t>(hw_height_) * 4);
            media::yuv_to_rgba(source, colour_.matrix, colour_.full_range, picture.rgba.data(),
                               hw_width_, hw_height_);
        }
        const std::int64_t took = sys::monotonic_us() - converting;
        convert_us_.fetch_add(took, std::memory_order_relaxed);
        if (took > kSlowStepUs)
            sys::log("[HUI] hw decoder: slow conversion %lld ms at %.2fs",
                     static_cast<long long>(took / 1000), frame.pts);
        converted_.fetch_add(1, std::memory_order_relaxed);
        picture.width = hw_width_;
        picture.height = hw_height_;
        picture.pts = frame.pts;
        picture.serial = frame.serial;
        queue_picture(std::move(picture));
    }

    // -------------------------------------------------------------- sound

    static void *audio_entry(void *self)
    {
        static_cast<Session *>(self)->audio();
        return nullptr;
    }

    // Opens the decoder and the converter to stereo float at the mixer's rate.
    bool open_audio(int stream_index, AVCodecContext **context, SwrContext **swr)
    {
        avcodec_free_context(context);
        swr_free(swr);
        const AVCodecParameters *parameters = format_->streams[stream_index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
        if (codec == nullptr)
            return false;
        *context = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(*context, parameters);
        if (avcodec_open2(*context, codec, nullptr) < 0)
        {
            avcodec_free_context(context);
            return false;
        }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(swr, &stereo, AV_SAMPLE_FMT_FLT, kOutputRate,
                                &(*context)->ch_layout, (*context)->sample_fmt,
                                (*context)->sample_rate, 0, nullptr) < 0 ||
            swr_init(*swr) < 0)
        {
            swr_free(swr);
            avcodec_free_context(context);
            return false;
        }
        return true;
    }

    void audio()
    {
        if (!wait_opened() || audio_streams_.empty())
            return;
        AVCodecContext *context = nullptr;
        SwrContext *swr = nullptr;
        AVFrame *frame = av_frame_alloc();
        std::vector<float> converted;
        int decoder_serial = -1;
        int open_stream = -1;
        double next_pts = 0.0;
        for (;;)
        {
            Item item;
            {
                Lock lock(mutex_);
                while (!stop_ && (audio_queue_.empty() || paused_))
                    pthread_cond_wait(&wake_, &mutex_);
                if (stop_)
                    break;
                item = audio_queue_.front();
                audio_queue_.pop_front();
                if (item.packet != nullptr)
                    audio_bytes_ -= static_cast<std::size_t>(item.packet->size);
                pthread_cond_broadcast(&wake_);
            }
            if (item.serial != serial_.load())
            {
                av_packet_free(&item.packet);
                continue;
            }
            if (item.packet != nullptr && item.stream != open_stream)
            {
                open_stream = open_audio(item.stream, &context, &swr) ? item.stream : -1;
                decoder_serial = -1;
            }
            const bool ended = item.packet == nullptr; // av_packet_free clears the pointer
            if (context == nullptr)
            {
                av_packet_free(&item.packet);
                if (ended)
                    mark_audio_done(item.serial);
                continue;
            }
            if (item.serial != decoder_serial)
            {
                avcodec_flush_buffers(context);
                ring_->drain();
                audio_has_.store(false, std::memory_order_release);
                next_pts = -1.0;
                decoder_serial = item.serial;
            }
            avcodec_send_packet(context, item.packet);
            av_packet_free(&item.packet);
            while (avcodec_receive_frame(context, frame) == 0)
            {
                const AVStream *stream = format_->streams[open_stream];
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    next_pts = static_cast<double>(frame->best_effort_timestamp) *
                                   av_q2d(stream->time_base) -
                               start_time_;
                else if (next_pts < 0.0)
                    next_pts = discard_before_.load();
                const double length = static_cast<double>(frame->nb_samples) / frame->sample_rate;
                if (next_pts + length < discard_before_.load())
                {
                    next_pts += length;
                    av_frame_unref(frame);
                    continue; // all of it is before the point the viewer asked for
                }
                const int capacity = swr_get_out_samples(swr, frame->nb_samples);
                converted.resize(static_cast<std::size_t>(std::max(capacity, 1)) * 2);
                float *out = converted.data();
                const int samples = swr_convert(swr, reinterpret_cast<std::uint8_t **>(&out),
                                                capacity, frame->extended_data, frame->nb_samples);
                av_frame_unref(frame);
                if (samples <= 0)
                    continue;
                if (!wait_for_first_picture(decoder_serial))
                    break;
                write_audio(converted.data(), static_cast<std::size_t>(samples), decoder_serial,
                            next_pts + static_cast<double>(samples) / kOutputRate);
                next_pts += length;
            }
            if (ended && item.serial == decoder_serial)
                mark_audio_done(item.serial);
        }
        av_frame_free(&frame);
        swr_free(&swr);
        avcodec_free_context(&context);
    }

    void mark_audio_done(int serial)
    {
        Lock lock(mutex_);
        if (serial == serial_.load())
            audio_eof_ = true;
    }

    // The sound waits for the first picture after a start or a seek, or the
    // sound would run ahead of a picture that takes a moment to decode.
    bool wait_for_first_picture(int serial)
    {
        if (video_stream_ < 0)
            return true;
        for (int waited = 0; waited < kFirstPictureWaitMs; waited += kFirstPicturePollMs)
        {
            {
                Lock lock(mutex_);
                if (stop_ || serial != serial_.load())
                    return false;
                if (video_ready_.load() || video_eof_)
                    return true;
            }
            usleep(kFirstPicturePollMs * 1000);
        }
        return true;
    }

    // `end` is the film time just after the last sample of this chunk.
    void write_audio(const float *samples, std::size_t frames, int serial, double end)
    {
        std::size_t done = 0;
        while (done < frames)
        {
            done += ring_->write(samples + done * 2, frames - done);
            if (done >= frames)
                break;
            usleep(4000); // the mixer takes 48000 frames a second; wait for room
            Lock lock(mutex_);
            if (stop_ || serial != serial_.load())
                return;
        }
        audio_end_.store(end, std::memory_order_relaxed);
        audio_has_.store(true, std::memory_order_release);
    }

    std::string path_;
    audio::StreamRing *ring_;
    const bool hardware_;
    // Pictures from the hardware decoder, and HDR pictures from FFmpeg, go to the
    // graphics chip as planes.
    std::atomic<bool> gpu_pictures_;
    // The film's colours and light, for the graphics chip; set before the threads start.
    media::YuvColour colour_;
    double start_ = 0.0;

    pthread_mutex_t mutex_;
    pthread_cond_t wake_;
    pthread_t demux_thread_{};
    pthread_t video_thread_{};
    pthread_t audio_thread_{};

    // Written by the demux thread before opened_ is set, read-only afterwards.
    FileSource source_;
    AVIOContext *io_ = nullptr;
    AVFormatContext *format_ = nullptr;
    AVCodecContext *video_context_ = nullptr;
    std::vector<int> audio_streams_;
    std::vector<media::Track> tracks_;
    int video_stream_ = -1;
    double start_time_ = 0.0;
    double duration_ = 0.0;
    std::string error_;
    std::string summary_;
    std::atomic<int> opened_{0}; // 0 opening, 1 ready, 2 failed
    // For the log: when the file was open and the first picture ready, from the start.
    const std::int64_t began_us_ = sys::monotonic_us();
    std::atomic<std::int64_t> opened_us_{0};
    std::atomic<std::int64_t> first_picture_us_{0};
    // Where the video thread spends its time, for the log.
    std::atomic<std::int64_t> packet_wait_us_{0};
    std::atomic<std::int64_t> room_wait_us_{0};
    std::atomic<std::int64_t> convert_us_{0};
    std::atomic<std::int64_t> converted_{0};
    // Used by the video thread only.
    media::DecodePace pace_;

    // Guarded by mutex_.
    std::deque<Item> video_queue_;
    std::deque<Item> audio_queue_;
    std::size_t video_bytes_ = 0;
    std::size_t audio_bytes_ = 0;
    std::size_t video_limit_ = max_video_bytes(); // lowered at open for big pictures
    std::size_t audio_limit_ = max_audio_bytes();
    std::size_t picture_limit_ = kMaxPictures;
    std::size_t slack_ = kSlack; // heap kept free for everything the player does not count
    std::deque<Picture> pictures_;
    std::vector<std::vector<std::uint8_t>> spare_;
    bool stop_ = false;
    bool paused_ = false;
    bool seek_pending_ = false;
    double seek_target_ = 0.0;
    bool eof_ = false;
    bool video_eof_ = false;
    bool audio_eof_ = false;

    std::atomic<bool> video_ready_{false};
    std::atomic<int> serial_{0};
    std::atomic<int> audio_choice_{0};
    std::atomic<double> discard_before_;
    std::atomic<double> clock_{0.0};
    std::atomic<double> audio_end_{0.0};
    std::atomic<bool> audio_has_{false};
    std::atomic<std::int64_t> decoded_{0};
    std::atomic<std::int64_t> decode_us_{0};
    std::atomic<std::int64_t> late_{0};

    // Video thread only.
    SwsContext *sws_ = nullptr;
    bool planes_refused_ = false; // video thread: logged once
    double last_video_pts_ = 0.0;
    AVBSFContext *annexb_ = nullptr;
    media::HevcEntry hw_entry_;
    media::PendingStamps hw_stamps_;
    int hw_errors_ = 0;
    int hw_silence_ = 0;
    int hw_resyncs_ = 0;
    int hw_since_resync_ = 0;
    bool hw_jumping_ = false;
    bool hw_resync_ = false;        // a stream error: start again at the next unit
    bool hw_h264_need_key_ = false; // H.264 after a stream error: nothing but a key frame
    bool jump_logged_ = false;

    // The hardware decoder: set up at open, used by the video and converter threads,
    // and gone once they have left it.
    std::unique_ptr<HwDecoder> hw_;
    std::atomic<bool> on_hardware_{false}; // for the log, which runs on the frame thread
    bool hevc_ = false;
    int hw_visible_width_ = 0;
    int hw_visible_height_ = 0;
    int hw_width_ = 0;
    int hw_height_ = 0;
    double hw_frame_time_ = 0.04;
    int hw_shift_ = -1; // converter thread: not known yet
    // Guarded by mutex_.
    std::deque<HwFrame> hw_frames_;
    bool hw_converting_ = false;
    bool hw_finished_ = false;
    // Frame thread only.
    Picture upload_;

  public:
    // The graphics chip could not take a picture: the converter does them from now on.
    void convert_on_cpu()
    {
        gpu_pictures_.store(false, std::memory_order_relaxed);
    }
};

class FfmpegPlayer final : public media::Player
{
  public:
    FfmpegPlayer(const media::VideoSurface &surface, audio::StreamRing *ring,
                 std::function<void(bool)> sound, bool hardware)
        : surface_(surface), ring_(ring), sound_(std::move(sound)), hardware_(hardware)
    {
    }
    ~FfmpegPlayer() override
    {
        close();
    }

    void open(const std::string &path, double start_seconds) override
    {
        close();
        sound(true); // the last film may have been left while paused
        error_.clear();
        summary_.clear();
        tracks_.clear();
        clock_ = std::max(0.0, start_seconds);
        duration_ = 0.0;
        starved_ = 0.0f;
        report_ = 0.0f;
        shown_ = 0;
        skipped_ = 0;
        scrub_ = false;
        session_ = std::make_unique<Session>(path, clock_, ring_, hardware_,
                                             static_cast<bool>(surface_.update_yuv) && gpu_ok_);
        state_ = PlayerState::opening;
    }

    void close() override
    {
        session_.reset(); // stops and joins the threads
        if (ring_ != nullptr)
            ring_->drain();
        release_texture();
        state_ = PlayerState::idle;
        width_ = 0;
        height_ = 0;
    }

    void update(float dt) override
    {
        if (!session_)
            return;
        if (state_ == PlayerState::opening)
        {
            const int opened = session_->opened();
            if (opened == 2)
            {
                error_ = session_->error();
                state_ = PlayerState::failed;
            }
            else if (opened == 1)
            {
                duration_ = session_->duration();
                summary_ = session_->summary();
                tracks_ = session_->tracks();
                state_ = PlayerState::buffering;
            }
            return;
        }
        if (state_ != PlayerState::playing && state_ != PlayerState::buffering &&
            state_ != PlayerState::paused)
            return;

        const Session::Status status = session_->status();
        const bool video = session_->has_video();
        if (state_ == PlayerState::paused)
        {
            // A seek while paused still has to show where it landed.
            if (scrub_ && status.pictures > 0)
            {
                if (upload(session_->take_picture()))
                    clock_ = std::max(clock_, shown_pts_);
                scrub_ = false;
            }
            return;
        }

        if (state_ == PlayerState::buffering)
        {
            // The sound plays on while the picture catches up, so the clock does too:
            // frozen, it made the decoder work toward a time the sound had left long
            // ago, and every picture it then produced was already too late to show.
            double audio = 0.0;
            if (session_->audio_clock(&audio))
                clock_ = audio;
            if (status.pictures > 0 || !video || status.video_done)
            {
                if (status.pictures > 0)
                {
                    // The first picture after a start or a seek is shown at once,
                    // and the clock goes to it, so the film never waits for a
                    // picture that is a hair after the position it asked for.
                    if (upload(session_->take_picture()))
                        clock_ = std::max(clock_, shown_pts_);
                }
                state_ = PlayerState::playing;
                starved_ = 0.0f;
            }
            session_->set_clock(clock_);
            return;
        }

        // Playing. The sound is the clock when it is running; otherwise time.
        double audio = 0.0;
        const double step = clock_ + static_cast<double>(dt);
        if (session_->audio_clock(&audio))
            clock_ = std::fabs(audio - step) > 0.2 ? audio : step + (audio - step) * 0.1;
        else
            clock_ = step;
        session_->set_clock(clock_);

        // The newest picture that is due.
        Session::Status now = status;
        while (now.pictures >= 2 && now.second_pts <= clock_)
        {
            session_->take_picture(); // too late to show: dropped
            ++skipped_;
            now = session_->status();
        }
        if (now.pictures > 0 && now.first_pts <= clock_ + 0.005 && upload(session_->take_picture()))
            ++shown_;
        report_ += dt;
        if (report_ >= 10.0f)
        {
            report_ = 0.0f;
            session_->log_stats("playing", clock_, shown_, skipped_);
        }

        starved_ = (video && now.pictures == 0 && !now.video_done) ? starved_ + dt : 0.0f;
        if (starved_ > 0.25f)
        {
            state_ = PlayerState::buffering; // the clock waits for the decoder
            session_->log_stats("stall", clock_, shown_, skipped_);
            return;
        }
        const bool sound_done =
            !session_->has_audio() || (now.audio_done && ring_->available() == 0);
        const bool picture_done = !video || (now.video_done && now.pictures == 0);
        if (now.eof && sound_done && picture_done)
            state_ = PlayerState::ended;
    }

    void pause(bool paused) override
    {
        if (!session_)
            return;
        if (paused && state_ != PlayerState::paused && state_ != PlayerState::ended)
        {
            session_->set_paused(true);
            sound(false);
            state_ = PlayerState::paused;
        }
        else if (!paused && state_ == PlayerState::paused)
        {
            // The mixer kept taking sound out of the ring while paused, so
            // it is empty: start over from here, which refills it.
            session_->set_paused(false);
            restart_at(clock_, PlayerState::buffering);
            sound(true);
        }
    }

    void seek_by(double seconds) override
    {
        seek_to(clock_ + seconds);
    }

    void seek_to(double seconds) override
    {
        if (!session_ || session_->opened() != 1)
            return;
        const double limit = duration_ > 0.0 ? std::max(0.0, duration_ - 1.0) : seconds;
        const double target = std::clamp(seconds, 0.0, std::max(0.0, limit));
        if (state_ == PlayerState::paused)
        {
            clock_ = target;
            session_->seek(target);
            scrub_ = true;
            return;
        }
        restart_at(target, PlayerState::buffering);
    }

    PlayerState state() const override
    {
        return state_;
    }
    double position() const override
    {
        return clock_;
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
        return width_;
    }
    int video_height() const override
    {
        return height_;
    }
    const std::string &error() const override
    {
        return error_;
    }
    const std::string &summary() const override
    {
        return summary_;
    }
    const std::vector<media::Track> &audio_tracks() const override
    {
        return tracks_;
    }
    int audio_track() const override
    {
        return session_ ? session_->audio_choice() : 0;
    }
    void select_audio(int index) override
    {
        if (session_ && session_->opened() == 1)
        {
            session_->choose_audio(index, clock_);
            if (state_ == PlayerState::playing)
                state_ = PlayerState::buffering;
        }
    }

  private:
    void restart_at(double seconds, PlayerState next)
    {
        clock_ = seconds;
        session_->seek(seconds);
        ring_->drain();
        state_ = next;
        starved_ = 0.0f;
    }

    // Puts a picture on the texture. False when there was none.
    bool upload(const Picture *picture)
    {
        if (picture == nullptr || picture->rgba.empty())
            return false;
        if (picture->yuv)
        {
            upload_planes(*picture);
            return true;
        }
        if (texture_ == 0 || picture->width != width_ || picture->height != height_)
        {
            release_texture();
            if (surface_.create)
                texture_ = surface_.create(picture->width, picture->height, picture->rgba.data());
            width_ = picture->width;
            height_ = picture->height;
        }
        else if (surface_.update)
        {
            surface_.update(texture_, picture->width, picture->height, picture->rgba.data());
        }
        shown_pts_ = picture->pts;
        return true;
    }

    void upload_planes(const Picture &picture)
    {
        if (texture_ == 0 || picture.width != width_ || picture.height != height_)
        {
            release_texture();
            if (surface_.create)
                texture_ = surface_.create(picture.width, picture.height, nullptr);
            width_ = picture.width;
            height_ = picture.height;
        }
        media::YuvFrame frame;
        frame.planes = picture.rgba.data();
        frame.layout = picture.layout;
        frame.colour = picture.colour;
        shown_pts_ = picture.pts;
        if (texture_ != 0 && surface_.update_yuv(texture_, width_, height_, frame))
            return;
        // One picture is lost; the next ones come as RGBA, for this film and the rest.
        sys::log("[HUI] video converter failed, converting on the CPU");
        gpu_ok_ = false;
        session_->convert_on_cpu();
    }

    void release_texture()
    {
        if (texture_ != 0 && surface_.release)
            surface_.release(texture_);
        texture_ = 0;
    }

    media::VideoSurface surface_;
    void sound(bool on) const
    {
        if (sound_)
            sound_(on);
    }

    audio::StreamRing *ring_;
    std::function<void(bool)> sound_;
    bool hardware_;
    std::unique_ptr<Session> session_;
    PlayerState state_ = PlayerState::idle;
    double clock_ = 0.0;
    double duration_ = 0.0;
    double shown_pts_ = 0.0;
    float starved_ = 0.0f;
    float report_ = 0.0f;
    int shown_ = 0;
    int skipped_ = 0;
    bool scrub_ = false;
    std::uint32_t texture_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool gpu_ok_ = true; // false once the graphics chip failed to convert a picture
    std::string error_;
    std::string summary_;
    std::vector<media::Track> tracks_;
};

} // namespace

std::unique_ptr<media::Player> make_ffmpeg_player(const media::VideoSurface &surface,
                                                  audio::StreamRing *ring,
                                                  std::function<void(bool)> sound, bool hardware)
{
    return std::make_unique<FfmpegPlayer>(surface, ring, std::move(sound), hardware);
}

} // namespace hui::ps5
