// Mediathek - Extra movie folders: what the viewer typed, the roots it means, the helper's copy.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

// Settings > Library keeps up to this many, each at most this long: a name expands to a root on
// every drive, and all of them must fit into the helper's block (kHelperRootsSize).
constexpr std::size_t kMaxScanFolders = 8;
constexpr std::size_t kMaxScanFolderLength = 160;

// What the viewer typed, tidied: blanks around it go, backslashes become slashes, doubled and
// trailing slashes go ("/mnt/usb0//Films/" -> "/mnt/usb0/Films"). A folder that starts with
// "/" is that folder; any other ("Films", "media/films") is looked for on every drive, like
// "movies". "" when it cannot be a folder: empty, "/" alone (the whole system), "." or ".."
// in it, a control character or a tab (the scan index separates with tabs), or too long.
std::string clean_scan_folder(std::string_view typed);

// The folders to scan for the extra folders, in order and without repeats: a path as it is,
// a name on each of scan_drives().
std::vector<std::string> scan_folder_roots(const std::vector<std::string> &folders);

// default_roots() followed by scan_folder_roots(), without repeats: what a scan walks.
std::vector<std::string> scan_roots(const std::vector<std::string> &folders);

// The saved list: one folder a line. Lines that clean_scan_folder() refuses, repeats and
// those past kMaxScanFolders are dropped, so a hand-edited file cannot break a scan.
std::vector<std::string> parse_scan_folders(std::string_view text);
std::string format_scan_folders(const std::vector<std::string> &folders);

// The scan helper (helper/mt-scan.c) cannot be given arguments: the ELF loader only runs it.
// It carries a block in its data instead, kHelperRootsSize bytes that start with
// kHelperRootsMark, and scans every line that follows the mark after its own folders. This
// writes `roots` there, in the helper's bytes before they are sent. False when the bytes have
// no such block (an older helper) or the roots do not fit; the bytes are left as they were.
inline constexpr std::string_view kHelperRootsMark = "MT-EXTRA-ROOTS-1\n";
constexpr std::size_t kHelperRootsSize = 20480; // 8 names on 12 drives fit
bool embed_helper_roots(std::string *elf, const std::vector<std::string> &roots);

} // namespace hui::media
