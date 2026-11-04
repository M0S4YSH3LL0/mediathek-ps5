// Mediathek - Extra media folders: what the viewer typed, the roots it means, the helper's copy.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/scan_folders.hpp"

#include "media/library.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace hui::media
{

namespace
{

void add_once(std::vector<std::string> *list, std::string item)
{
    if (std::find(list->begin(), list->end(), item) == list->end())
        list->push_back(std::move(item));
}

// A path is itself; a name is that folder on every drive.
std::vector<std::string> roots_of(const std::string &folder)
{
    std::vector<std::string> roots;
    if (folder.empty())
        return roots;
    if (folder.front() == '/')
        roots.push_back(folder);
    else
        for (const std::string &drive : scan_drives())
            roots.push_back(drive + "/" + folder);
    return roots;
}

constexpr std::string_view kContentWords[] = {"", "movies", "series"}; // as FolderContent

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

std::vector<std::string> scan_folder_roots(const std::vector<ScanFolder> &folders)
{
    std::vector<std::string> roots;
    for (const ScanFolder &folder : folders)
        for (std::string &root : roots_of(folder.path))
            add_once(&roots, std::move(root));
    return roots;
}

RootContents root_contents(const std::vector<ScanFolder> &folders)
{
    RootContents contents;
    std::vector<std::string> seen;
    for (const ScanFolder &folder : folders)
        for (std::string &root : roots_of(folder.path))
        {
            if (std::find(seen.begin(), seen.end(), root) != seen.end())
                continue;
            seen.push_back(root);
            if (folder.content != FolderContent::automatic)
                contents.emplace(std::move(root), folder.content);
        }
    return contents;
}

std::vector<std::string> scan_roots(const std::vector<ScanFolder> &folders)
{
    std::vector<std::string> roots = default_roots();
    for (std::string &root : scan_folder_roots(folders))
        add_once(&roots, std::move(root));
    return roots;
}

std::vector<ScanFolder> parse_scan_folders(std::string_view text)
{
    std::vector<ScanFolder> folders;
    while (!text.empty() && folders.size() < kMaxScanFolders)
    {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        text.remove_prefix(std::min(end + 1, text.size()));
        ScanFolder folder;
        if (const std::size_t tab = line.find('\t'); tab != std::string_view::npos)
        {
            const std::string_view word = line.substr(tab + 1);
            for (std::size_t i = 1; i < std::size(kContentWords); ++i)
                if (word == kContentWords[i])
                    folder.content = static_cast<FolderContent>(i);
            line = line.substr(0, tab);
        }
        folder.path = clean_scan_folder(line);
        if (folder.path.empty())
            continue;
        const bool known = std::any_of(folders.begin(), folders.end(), [&](const ScanFolder &other)
                                       { return other.path == folder.path; });
        if (!known)
            folders.push_back(std::move(folder));
    }
    return folders;
}

std::string format_scan_folders(const std::vector<ScanFolder> &folders)
{
    std::string text;
    for (const ScanFolder &folder : folders)
    {
        text += folder.path;
        if (folder.content != FolderContent::automatic)
            text += "\t" + std::string(kContentWords[static_cast<std::size_t>(folder.content)]);
        text += "\n";
    }
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
