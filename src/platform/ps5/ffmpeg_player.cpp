// Mediathek - Movie playback on the console: FFmpeg decodes, the app shows and plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/ffmpeg_player.hpp"

#include "audio/mixer.hpp"
#include "core/i18n.hpp"
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
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

extern "C" std::size_t hui_heap_capacity(void);

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
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    double pts = 0.0;
    int serial = 0;
};

const char *video_name(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_H264:
        return "H.264";
    case AV_CODEC_ID_HEVC:
        return "HEVC";
    case AV_CODEC_ID_AV1:
        return "AV1";
    case AV_CODEC_ID_VP9:
        return "VP9";
    case AV_CODEC_ID_MPEG4:
        return "MPEG-4";
    case AV_CODEC_ID_MPEG2VIDEO:
        return "MPEG-2";
    default:
        return avcodec_get_name(id);
    }
}

std::string audio_name(AVCodecID id)
{
    std::string name = avcodec_get_name(id);
    for (char &c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

const char *layout_name(int channels)
{
    switch (channels)
    {
    case 1:
        return "1.0";
    case 2:
        return "2.0";
    case 6:
        return "5.1";
    case 8:
        return "7.1";
    default:
        return "";
    }
}

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
    Session(std::string path, double start, audio::StreamRing *ring)
        : path_(std::move(path)), ring_(ring), discard_before_(start)
    {
        pthread_mutex_init(&mutex_, nullptr);
        pthread_cond_init(&wake_, nullptr);
        start_ = start;
        pthread_create(&demux_thread_, nullptr, &Session::demux_entry, this);
        pthread_create(&video_thread_, nullptr, &Session::video_entry, this);
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
        sys::log("[HUI] %s at %.1fs: %d shown, %d skipped on screen; video queue %zu packets %zu "
                 "KB, audio %zu packets, "
                 "%zu pictures; drive %.1f MB/s while reading (%lld MB total); "
                 "decode+convert %.1f ms/frame over %lld frames, %lld late",
                 what, position, shown, skipped, video_packets, video_bytes >> 10, audio_packets,
                 pictures, static_cast<double>(read_bytes) / static_cast<double>(read_us),
                 static_cast<long long>(read_bytes >> 20),
                 static_cast<double>(decode_us_.load()) / 1000.0 / static_cast<double>(frames),
                 static_cast<long long>(decoded_.load()), static_cast<long long>(late_.load()));
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
            const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
            if (codec == nullptr)
                return fail(i18n::tr(i18n::Text::err_no_picture_decoder),
                            AVERROR_DECODER_NOT_FOUND);
            video_context_ = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(video_context_, parameters);
            video_context_->thread_count = 4;
            video_context_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            // Not AV_CODEC_FLAG2_FAST: its non-conforming shortcuts leave
            // blocks and smears in busy, high-bitrate H.264 for a few percent.
            // Beyond 1080p the CPU is the limit: skip the in-loop filter on
            // frames nothing else is predicted from.
            if (parameters->width > kMaxWidth)
                video_context_->skip_loop_filter = AVDISCARD_NONREF;
            code = avcodec_open2(video_context_, codec, nullptr);
            if (code < 0)
                return fail(i18n::tr(i18n::Text::err_start_decoder), code);
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
    }
    void clear_queues()
    {
        Lock lock(mutex_);
        clear_queues_locked();
    }

    void recycle_locked(Picture &&picture)
    {
        if (spare_.size() < kMaxPictures + 1 && !picture.rgba.empty())
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
                // Backward: the nearest key frame before the target, so the
                // decoder can run forward to it without grey pictures.
                if (av_seek_frame(format_, -1, stamp, AVSEEK_FLAG_BACKWARD) < 0)
                    av_seek_frame(format_, -1, stamp, 0);
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

    void video()
    {
        if (!wait_opened() || video_stream_ < 0)
            return;
        AVFrame *frame = av_frame_alloc();
        int decoder_serial = -1;
        for (;;)
        {
            Item item;
            {
                Lock lock(mutex_);
                while (!stop_ && video_queue_.empty())
                    pthread_cond_wait(&wake_, &mutex_);
                if (stop_)
                    break;
                item = video_queue_.front();
                video_queue_.pop_front();
                if (item.packet != nullptr)
                    video_bytes_ -= static_cast<std::size_t>(item.packet->size);
                pthread_cond_broadcast(&wake_);
            }
            if (item.serial != serial_.load())
            {
                av_packet_free(&item.packet);
                continue;
            }
            if (item.serial != decoder_serial)
            {
                avcodec_flush_buffers(video_context_);
                decoder_serial = item.serial;
            }
            const bool ended = item.packet == nullptr; // av_packet_free clears the pointer
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

        int width = frame->width;
        int height = frame->height;
        const AVRational aspect = av_guess_sample_aspect_ratio(
            format_, const_cast<AVStream *>(stream), const_cast<AVFrame *>(frame));
        if (aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den)
            width = static_cast<int>(static_cast<double>(width) * aspect.num / aspect.den);
        const double fit = std::min({1.0, static_cast<double>(kMaxWidth) / width,
                                     static_cast<double>(kMaxHeight) / height});
        width = std::max(2, static_cast<int>(width * fit) & ~1);
        height = std::max(2, static_cast<int>(height * fit) & ~1);

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
        {
            Lock lock(mutex_);
            while (!stop_ && serial == serial_.load() && pictures_.size() >= kMaxPictures)
                pthread_cond_wait(&wake_, &mutex_);
            if (stop_ || serial != serial_.load())
                return;
            if (!spare_.empty())
            {
                picture.rgba = std::move(spare_.back());
                spare_.pop_back();
            }
        }
        picture.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        std::uint8_t *planes[4] = {picture.rgba.data(), nullptr, nullptr, nullptr};
        const int strides[4] = {width * 4, 0, 0, 0};
        sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, planes, strides);
        picture.width = width;
        picture.height = height;
        picture.pts = pts;
        picture.serial = serial;

        Lock lock(mutex_);
        if (stop_ || serial != serial_.load())
            return;
        pictures_.push_back(std::move(picture));
        video_ready_.store(true);
        pthread_cond_broadcast(&wake_);
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
        for (int waited = 0; waited < 400; ++waited) // 2 s at most
        {
            {
                Lock lock(mutex_);
                if (stop_ || serial != serial_.load())
                    return false;
                if (video_ready_.load() || video_eof_)
                    return true;
            }
            usleep(5000);
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

    // Guarded by mutex_.
    std::deque<Item> video_queue_;
    std::deque<Item> audio_queue_;
    std::size_t video_bytes_ = 0;
    std::size_t audio_bytes_ = 0;
    const std::size_t video_limit_ = max_video_bytes();
    const std::size_t audio_limit_ = max_audio_bytes();
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
    double last_video_pts_ = 0.0;
    // Frame thread only.
    Picture upload_;
};

class FfmpegPlayer final : public media::Player
{
  public:
    FfmpegPlayer(const media::VideoSurface &surface, audio::StreamRing *ring,
                 std::function<void(bool)> sound)
        : surface_(surface), ring_(ring), sound_(std::move(sound))
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
        session_ = std::make_unique<Session>(path, clock_, ring_);
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
    std::string error_;
    std::string summary_;
    std::vector<media::Track> tracks_;
};

} // namespace

std::unique_ptr<media::Player> make_ffmpeg_player(const media::VideoSurface &surface,
                                                  audio::StreamRing *ring,
                                                  std::function<void(bool)> sound)
{
    return std::make_unique<FfmpegPlayer>(surface, ring, std::move(sound));
}

} // namespace hui::ps5
