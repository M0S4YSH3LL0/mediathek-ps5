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
    const std::vector<std::string> roots = scan_folder_roots(
        {{"/mnt/usb1/Films", FolderContent::automatic}, {"Docs", FolderContent::automatic}});
    ASSERT_EQ(roots.size(), 1u + scan_drives().size());
    EXPECT_EQ(roots[0], "/mnt/usb1/Films");
    EXPECT_EQ(roots[1], "/data/Docs");
    EXPECT_EQ(roots[2], "/Docs");
    EXPECT_NE(std::find(roots.begin(), roots.end(), "/mnt/ext1/Docs"), roots.end());

    // After the default folders, and nothing twice: "movies" is one of them already.
    const std::vector<std::string> all = scan_roots({{"movies", FolderContent::automatic},
                                                     {"/mnt/usb1/Films", FolderContent::automatic},
                                                     {"/mnt/usb1/Films", FolderContent::movies}});
    EXPECT_EQ(all.size(), default_roots().size() + 1);
    EXPECT_EQ(all.back(), "/mnt/usb1/Films");
}

TEST(ScanFolders, TheSavedListSurvivesHandEditing)
{
    const std::vector<ScanFolder> folders = {{"/mnt/usb0/Films", FolderContent::automatic},
                                             {"Docs", FolderContent::series},
                                             {"/data/Kino", FolderContent::movies}};
    EXPECT_EQ(format_scan_folders(folders), "/mnt/usb0/Films\nDocs\tseries\n/data/Kino\tmovies\n");
    EXPECT_EQ(parse_scan_folders(format_scan_folders(folders)), folders);
    // Windows line ends, blank lines, a repeat, a refused line, and too many.
    std::string text = "/mnt/usb0/Films\r\n\r\n/mnt/usb0/Films/\n/\nDocs\n";
    for (int i = 0; i < 20; ++i)
        text += "/extra/" + std::to_string(i) + "\n";
    const std::vector<ScanFolder> read = parse_scan_folders(text);
    ASSERT_EQ(read.size(), kMaxScanFolders);
    EXPECT_EQ(read[0].path, "/mnt/usb0/Films");
    EXPECT_EQ(read[1].path, "Docs");
    EXPECT_EQ(read[2].path, "/extra/0");
    EXPECT_TRUE(parse_scan_folders("").empty());
    // A content word it does not know is Automatic; the folder is kept.
    const std::vector<ScanFolder> odd = parse_scan_folders("/a\tcartoons\n/b\tseries\r\n");
    ASSERT_EQ(odd.size(), 2u);
    EXPECT_EQ(odd[0].content, FolderContent::automatic);
    EXPECT_EQ(odd[1].content, FolderContent::series);
}

TEST(ScanFolders, TheContentChosenGoesWithEveryRootOfAFolder)
{
    const RootContents contents = root_contents({{"/mnt/usb1/Films", FolderContent::movies},
                                                 {"Anime", FolderContent::series},
                                                 {"Docs", FolderContent::automatic}});
    EXPECT_EQ(contents.size(), 1u + scan_drives().size()); // Automatic is no entry
    EXPECT_EQ(contents.at("/mnt/usb1/Films"), FolderContent::movies);
    EXPECT_EQ(contents.at("/mnt/ext1/Anime"), FolderContent::series);
    EXPECT_EQ(contents.count("/data/Docs"), 0u);
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
    std::vector<ScanFolder> longest;
    for (std::size_t i = 0; i < kMaxScanFolders; ++i)
        longest.push_back({std::string(kMaxScanFolderLength - 1, 'a') + std::to_string(i),
                           FolderContent::series});
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
