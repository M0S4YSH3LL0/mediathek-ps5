// Mediathek - Extra movie folders: what the viewer typed, the roots it means, the helper's copy.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/scan_folders.hpp"

#include "media/library.hpp"

#include <algorithm>
#include <cstring>

namespace hui::media
{

namespace
{

void add_once(std::vector<std::string> *list, std::string item)
{
    if (std::find(list->begin(), list->end(), item) == list->end())
        list->push_back(std::move(item));
}

} // namespace

std::string clean_scan_folder(std::string_view typed)
{
    while (!typed.empty() && typed.front() == ' ')
        typed.remove_prefix(1);
    while (!typed.empty() && typed.back() == ' ')
        typed.remove_suffix(1);
    std::string folder;
    for (char c : typed)
    {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            return std::string(); // a tab or a line break would break the scan index
        if (c == '\\')
            c = '/';
        if (c == '/' && !folder.empty() && folder.back() == '/')
            continue;
        folder.push_back(c);
    }
    while (folder.size() > 1 && folder.back() == '/')
        folder.pop_back();
    if (folder.empty() || folder == "/" || folder.size() > kMaxScanFolderLength)
        return std::string();
    // "." and ".." would let one folder be written many ways, and ".." leave the drive.
    std::size_t start = folder.front() == '/' ? 1 : 0;
    while (start <= folder.size())
    {
        const std::size_t end = std::min(folder.find('/', start), folder.size());
        const std::string_view part(folder.data() + start, end - start);
        if (part == "." || part == "..")
            return std::string();
        start = end + 1;
    }
    return folder;
}

std::vector<std::string> scan_folder_roots(const std::vector<std::string> &folders)
{
    std::vector<std::string> roots;
    const std::vector<std::string> drives = scan_drives();
    for (const std::string &folder : folders)
    {
        if (folder.empty())
            continue;
        if (folder.front() == '/')
            add_once(&roots, folder);
        else
            for (const std::string &drive : drives)
                add_once(&roots, drive + "/" + folder);
    }
    return roots;
}

std::vector<std::string> scan_roots(const std::vector<std::string> &folders)
{
    std::vector<std::string> roots = default_roots();
    for (std::string &root : scan_folder_roots(folders))
        add_once(&roots, std::move(root));
    return roots;
}

std::vector<std::string> parse_scan_folders(std::string_view text)
{
    std::vector<std::string> folders;
    while (!text.empty() && folders.size() < kMaxScanFolders)
    {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        text.remove_prefix(std::min(end + 1, text.size()));
        if (std::string folder = clean_scan_folder(line); !folder.empty())
            add_once(&folders, std::move(folder));
    }
    return folders;
}

std::string format_scan_folders(const std::vector<std::string> &folders)
{
    std::string text;
    for (const std::string &folder : folders)
        text += folder + "\n";
    return text;
}

bool embed_helper_roots(std::string *elf, const std::vector<std::string> &roots)
{
    const std::size_t at = elf->find(kHelperRootsMark);
    // A second mark would leave it unclear which block the helper reads.
    if (at == std::string::npos ||
        elf->find(kHelperRootsMark, at + kHelperRootsMark.size()) != std::string::npos ||
        elf->size() - at < kHelperRootsSize)
        return false;
    std::string lines;
    for (const std::string &root : roots)
        lines += root + "\n";
    const std::size_t room = kHelperRootsSize - kHelperRootsMark.size();
    if (lines.size() >= room) // the zero after the last line must fit too
        return false;
    char *block = elf->data() + at + kHelperRootsMark.size();
    std::memset(block, 0, room);
    if (!lines.empty())
        std::memcpy(block, lines.data(), lines.size());
    return true;
}

} // namespace hui::media
