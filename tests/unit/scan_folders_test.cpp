// Mediathek - Tests for the extra movie folders: tidying, roots, the saved list, the helper block.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/library.hpp"
#include "media/scan_folders.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using namespace hui::media;

TEST(ScanFolders, TidiesWhatWasTyped)
{
    EXPECT_EQ(clean_scan_folder("  /mnt/usb0//Films/ "), "/mnt/usb0/Films");
    EXPECT_EQ(clean_scan_folder("\\mnt\\usb1\\Filme"), "/mnt/usb1/Filme");
    EXPECT_EQ(clean_scan_folder("Films"), "Films");
    EXPECT_EQ(clean_scan_folder("media/films/"), "media/films");
    EXPECT_EQ(clean_scan_folder("/data/Film Night (Archive)"), "/data/Film Night (Archive)");
}

TEST(ScanFolders, RefusesWhatCannotBeAFolder)
{
    EXPECT_EQ(clean_scan_folder(""), "");
    EXPECT_EQ(clean_scan_folder("   "), "");
    EXPECT_EQ(clean_scan_folder("/"), "");   // the whole system
    EXPECT_EQ(clean_scan_folder("///"), ""); // ... written another way
    EXPECT_EQ(clean_scan_folder("/mnt/../data"), "");
    EXPECT_EQ(clean_scan_folder(".."), "");
    EXPECT_EQ(clean_scan_folder("./films"), "");
    EXPECT_EQ(clean_scan_folder("/mnt/usb0/a\tb"), ""); // the index separates with tabs
    EXPECT_EQ(clean_scan_folder("/mnt/usb0/a\nb"), "");
    EXPECT_EQ(clean_scan_folder("/" + std::string(kMaxScanFolderLength, 'x')), "");
    EXPECT_EQ(clean_scan_folder("/a..b/c.d"), "/a..b/c.d"); // dots inside a name are fine
}

TEST(ScanFolders, APathIsItselfAndANameIsOnEveryDrive)
{
    const std::vector<std::string> roots = scan_folder_roots({"/mnt/usb1/Films", "Docs"});
    ASSERT_EQ(roots.size(), 1u + scan_drives().size());
    EXPECT_EQ(roots[0], "/mnt/usb1/Films");
    EXPECT_EQ(roots[1], "/data/Docs");
    EXPECT_EQ(roots[2], "/Docs");
    EXPECT_NE(std::find(roots.begin(), roots.end(), "/mnt/ext1/Docs"), roots.end());

    // After the default folders, and nothing twice: "movies" is one of them already.
    const std::vector<std::string> all =
        scan_roots({"movies", "/mnt/usb1/Films", "/mnt/usb1/Films"});
    EXPECT_EQ(all.size(), default_roots().size() + 1);
    EXPECT_EQ(all.back(), "/mnt/usb1/Films");
}

TEST(ScanFolders, TheSavedListSurvivesHandEditing)
{
    const std::vector<std::string> folders = {"/mnt/usb0/Films", "Docs"};
    EXPECT_EQ(parse_scan_folders(format_scan_folders(folders)), folders);
    // Windows line ends, blank lines, a repeat, a refused line, and too many.
    std::string text = "/mnt/usb0/Films\r\n\r\n/mnt/usb0/Films/\n/\nDocs\n";
    for (int i = 0; i < 20; ++i)
        text += "/extra/" + std::to_string(i) + "\n";
    const std::vector<std::string> read = parse_scan_folders(text);
    ASSERT_EQ(read.size(), kMaxScanFolders);
    EXPECT_EQ(read[0], "/mnt/usb0/Films");
    EXPECT_EQ(read[1], "Docs");
    EXPECT_EQ(read[2], "/extra/0");
    EXPECT_TRUE(parse_scan_folders("").empty());
}

// Bytes shaped like the helper: something before, the block, something after.
std::string fake_helper()
{
    std::string block(kHelperRootsSize, '\0');
    block.replace(0, kHelperRootsMark.size(), kHelperRootsMark);
    return std::string("\x7F"
                       "ELF code",
                       9) +
           block + "more data";
}

TEST(ScanFolders, TheRootsAreWrittenIntoTheHelpersBlock)
{
    std::string elf = fake_helper();
    const std::size_t size = elf.size();
    ASSERT_TRUE(embed_helper_roots(&elf, {"/mnt/usb1/Films", "/data/Docs"}));
    EXPECT_EQ(elf.size(), size); // nothing moves in the file
    const std::size_t at = elf.find(kHelperRootsMark) + kHelperRootsMark.size();
    EXPECT_EQ(std::string(elf.c_str() + at), "/mnt/usb1/Films\n/data/Docs\n");
    EXPECT_EQ(elf.substr(elf.size() - 9), "more data");

    // Fewer roots the next time: nothing of the longer list is left behind.
    ASSERT_TRUE(embed_helper_roots(&elf, {"/x"}));
    EXPECT_EQ(std::string(elf.c_str() + at), "/x\n");
    ASSERT_TRUE(embed_helper_roots(&elf, {}));
    EXPECT_EQ(elf[at], '\0');
}

TEST(ScanFolders, AHelperWithoutABlockOrRootsThatDoNotFitAreLeftAlone)
{
    std::string old = "\x7F"
                      "ELF an older helper";
    const std::string before = old;
    EXPECT_FALSE(embed_helper_roots(&old, {"/x"}));
    EXPECT_EQ(old, before);

    std::string elf = fake_helper();
    const std::string original = elf;
    const std::vector<std::string> many(kHelperRootsSize / 10, "/123456789");
    EXPECT_FALSE(embed_helper_roots(&elf, many));
    EXPECT_EQ(elf, original);

    // The worst the settings allow fits: every folder a long name, on every drive.
    std::vector<std::string> longest;
    for (std::size_t i = 0; i < kMaxScanFolders; ++i)
        longest.push_back(std::string(kMaxScanFolderLength - 1, 'a') + std::to_string(i));
    EXPECT_TRUE(embed_helper_roots(&elf, scan_folder_roots(longest)));

    std::string twice = fake_helper() + fake_helper();
    EXPECT_FALSE(embed_helper_roots(&twice, {"/x"})); // which block would it read?
}

TEST(ScanFolders, AFilmIsInTheCollectionOfTheDeepestFolderItIsIn)
{
    ScanResult scan;
    // An extra folder around a movies folder: the films under movies/ keep their collections.
    scan.roots = {"/mnt/usb0/movies", "/mnt/usb0"};
    for (const char *path :
         {"/mnt/usb0/movies/Sagas/One (2001).mkv", "/mnt/usb0/movies/Sagas/Two (2003).mkv",
          "/mnt/usb0/Archive/Old (1950).mkv", "/mnt/usb0/Archive/Older (1940).mkv"})
    {
        FoundFile file;
        file.path = path;
        file.readable = path;
        scan.videos.push_back(file);
    }
    const Library library = Library::from_scan(scan);
    ASSERT_EQ(library.size(), 4u);
    for (const Movie &movie : library.movies())
        EXPECT_EQ(movie.collection,
                  movie.path.find("/movies/") != std::string::npos ? "Sagas" : "Archive")
            << movie.path;
}

} // namespace
