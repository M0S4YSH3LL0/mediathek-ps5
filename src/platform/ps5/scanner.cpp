// Mediathek - Finding the movies: the helper payload that lists folders for the sandboxed app.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/scanner.hpp"

#include "core/i18n.hpp"
#include "core/save_file.hpp"
#include "media/scan_folders.hpp"
#include "platform/ps5/system.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>

namespace hui::ps5
{

namespace
{

constexpr const char *kIndexPath = "/app0/mediathek-index.txt";
// The build copies assets/ into the app, and the helper is built into assets/helper.
constexpr const char *kHelperPath = "/app0/assets/helper/mt-scan.elf";
constexpr int kLoaderPort = 9021;
constexpr float kPollSeconds = 0.5f;
// A scan of a few thousand files on a USB drive takes seconds; this is the
// point where waiting longer would only hide that the payload never ran.
constexpr float kGiveUpSeconds = 45.0f;

// The "T<TAB>seconds" line the helper writes: when the scan finished.
std::int64_t scan_time(std::string_view text)
{
    const std::size_t at = text.find("\nT\t");
    if (at == std::string_view::npos)
        return 0;
    return std::atoll(std::string(text.substr(at + 3, 20)).c_str());
}

} // namespace

bool Scanner::load_cached(media::ScanResult *out)
{
    std::string text;
    return save::read_file(kIndexPath, &text, 64u << 20) && media::parse_index(text, out);
}

bool Scanner::start(const std::vector<std::string> &extra_roots)
{
    status_ = Status::failed;
    std::string elf;
    if (!save::read_file(kHelperPath, &elf, 8u << 20) || elf.empty())
    {
        note_ = i18n::tr(i18n::Text::scan_missing);
        return false;
    }
    // Written every time, also with no folders: the block is then empty.
    const bool embedded = media::embed_helper_roots(&elf, extra_roots);
    sys::log("[HUI] scan extra roots=%zu %s", extra_roots.size(),
             embedded ? "embedded" : "NOT embedded (old helper or too many)");
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        note_ = i18n::tr(i18n::Text::scan_no_socket);
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kLoaderPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
    {
        close(fd);
        note_ = i18n::tr(i18n::Text::scan_no_loader);
        return false;
    }
    std::size_t sent = 0;
    while (sent < elf.size())
    {
        const ssize_t written = write(fd, elf.data() + sent, elf.size() - sent);
        if (written <= 0)
            break;
        sent += static_cast<std::size_t>(written);
    }
    close(fd);
    if (sent != elf.size())
    {
        note_ = i18n::tr(i18n::Text::scan_loader_closed);
        return false;
    }
    started_ = static_cast<std::int64_t>(std::time(nullptr));
    since_poll_ = 0.0f;
    waited_ = 0.0f;
    note_ = i18n::tr(i18n::Text::scan_running);
    status_ = Status::running;
    return true;
}

Scanner::Status Scanner::poll(float dt, media::ScanResult *out)
{
    if (status_ != Status::running)
        return status_;
    waited_ += dt;
    since_poll_ += dt;
    if (since_poll_ >= kPollSeconds)
    {
        since_poll_ = 0.0f;
        std::string text;
        // Slack of a few seconds for a clock that differs between the payload
        // and this process; the file is only replaced once a scan is finished.
        if (save::read_file(kIndexPath, &text, 64u << 20) && scan_time(text) >= started_ - 5 &&
            media::parse_index(text, out))
        {
            note_.clear();
            status_ = Status::done;
            return status_;
        }
    }
    if (waited_ > kGiveUpSeconds)
    {
        note_ = i18n::tr(i18n::Text::scan_silent);
        status_ = Status::failed;
    }
    return status_;
}

} // namespace hui::ps5
