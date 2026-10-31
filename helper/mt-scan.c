/* Mediathek - Folder scanner payload: lists the movies folders for the app.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mediathek is a sandboxed title: it can open a file whose path it knows but
 * cannot list a folder. This payload runs outside the sandbox (the app sends
 * it to the ELF loader on port 9021), walks the movies folders on the internal
 * drive and on USB and extended storage, and writes what it found where the
 * app can read it: the title's install folder, which the app sees as /app0.
 *
 *   MT_HOME/mediathek-index.txt
 *     mediathek-index 1
 *     T<TAB>seconds since the epoch     when this scan finished
 *     R<TAB>path                        a movies folder that exists
 *     V<TAB>path<TAB>size<TAB>mtime     a video
 *     I<TAB>path<TAB>readable           a picture beside a video
 *     S<TAB>path<TAB>readable           a subtitle file beside a video
 *     end
 *
 * "readable" is a copy under MT_HOME/art: a USB drive's pictures are not
 * necessarily readable from the sandbox, so the small files are copied in.
 * The videos are read in place; whether the sandbox allows that on a given
 * drive is the app's to find out (it says so when opening fails).
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MT_HOME
#define MT_HOME "/data/homebrew/PPSA66120"
#endif
#define INDEX MT_HOME "/mediathek-index.txt"
#define INDEX_TMP MT_HOME "/mediathek-index.txt.tmp"
#define ART_DIR MT_HOME "/art"

#define MAX_DEPTH 6 /* folder levels below a root; the same as kMaxDepth in library.cpp */
#define MAX_VIDEOS 20000
#define MAX_ART 6000
#define MAX_IMAGE_BYTES (8u << 20)
#define MAX_SUBTITLE_BYTES (2u << 20)

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void
notify(const char *fmt, ...) {
  notify_request_t req;
  va_list args;
  memset(&req, 0, sizeof req);
  va_start(args, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, args);
  va_end(args);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static long g_videos, g_art;

static const char *VIDEO_EXT[] = {".mkv", ".mp4", ".m4v", ".mov", ".avi", ".wmv", ".ts", ".m2ts", ".mpg", ".mpeg", ".webm", ".flv", ".vob", ".ogv", 0};
static const char *IMAGE_EXT[] = {".jpg", ".jpeg", ".png", 0};
static const char *SUBTITLE_EXT[] = {".srt", 0};

static const char *
extension(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot ? dot : "";
}

static int
has_ext(const char *name, const char *const *list) {
  const char *ext = extension(name);
  for (int i = 0; list[i]; i++)
    if (!strcasecmp(ext, list[i])) return 1;
  return 0;
}

/* The copy's name from the source path, so a rescan finds its copy again. */
static void
copy_name(const char *path, char *out, size_t cap) {
  uint64_t h = 1469598103934665603ull;
  for (const char *p = path; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
  const char *ext = extension(path);
  char lower[8] = "";
  for (int i = 0; ext[i] && i < 7; i++) lower[i] = (char)((ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i] + 32 : ext[i]);
  snprintf(out, cap, ART_DIR "/%016llx%s", (unsigned long long)h, lower);
}

/* Copies a small file into the art folder unless an equal copy is there. */
static int
stage(const char *from, const struct stat *st, char *to, size_t cap) {
  copy_name(from, to, cap);
  struct stat have;
  if (!stat(to, &have) && have.st_size == st->st_size && have.st_mtime >= st->st_mtime) return 0;
  FILE *a = fopen(from, "rb");
  if (!a) return -1;
  char part[1400];
  snprintf(part, sizeof part, "%s.part", to);
  FILE *b = fopen(part, "wb");
  if (!b) {
    fclose(a);
    return -1;
  }
  char buf[16384];
  size_t k;
  int ok = 1;
  while ((k = fread(buf, 1, sizeof buf, a)) > 0)
    if (fwrite(buf, 1, k, b) != k) {
      ok = 0;
      break;
    }
  if (ferror(a)) ok = 0;
  fclose(a);
  if (fclose(b)) ok = 0;
  if (!ok || rename(part, to)) {
    unlink(part);
    return -1;
  }
  chmod(to, 0666);
  return 0;
}

static void
walk(FILE *out, const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue; /* ., .., and hidden files such as ._name from a Mac */
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st)) continue;
    if (S_ISDIR(st.st_mode)) {
      if (depth < MAX_DEPTH) walk(out, path, depth + 1);
      continue;
    }
    if (!S_ISREG(st.st_mode)) continue;
    if (has_ext(e->d_name, VIDEO_EXT)) {
      if (g_videos >= MAX_VIDEOS) continue;
      fprintf(out, "V\t%s\t%llu\t%lld\n", path, (unsigned long long)st.st_size, (long long)st.st_mtime);
      g_videos++;
    } else if (has_ext(e->d_name, IMAGE_EXT) && (unsigned)st.st_size <= MAX_IMAGE_BYTES && g_art < MAX_ART) {
      char copy[1300];
      if (!stage(path, &st, copy, sizeof copy)) {
        fprintf(out, "I\t%s\t%s\n", path, copy);
        g_art++;
      }
    } else if (has_ext(e->d_name, SUBTITLE_EXT) && (unsigned)st.st_size <= MAX_SUBTITLE_BYTES && g_art < MAX_ART) {
      char copy[1300];
      if (!stage(path, &st, copy, sizeof copy)) {
        fprintf(out, "S\t%s\t%s\n", path, copy);
        g_art++;
      }
    }
  }
  closedir(d);
}

/* A USB or extended drive ignores letter case, so ".../movies" and
 * ".../Movies" can be one folder; scanning it twice would list every film
 * twice (and count it twice). Roots are told apart by device and inode. */
#define MAX_ROOTS 64
static dev_t g_root_dev[MAX_ROOTS];
static ino_t g_root_ino[MAX_ROOTS];
static int g_roots;

static void
scan_root(FILE *out, const char *root) {
  struct stat st;
  if (stat(root, &st) || !S_ISDIR(st.st_mode)) return;
  for (int i = 0; i < g_roots; i++)
    if (g_root_dev[i] == st.st_dev && g_root_ino[i] == st.st_ino) return;
  if (g_roots < MAX_ROOTS) {
    g_root_dev[g_roots] = st.st_dev;
    g_root_ino[g_roots] = st.st_ino;
    g_roots++;
  }
  fprintf(out, "R\t%s\n", root);
  walk(out, root, 1);
}

int
main(void) {
  mkdir(MT_HOME, 0777);
  mkdir(ART_DIR, 0777);
  FILE *out = fopen(INDEX_TMP, "w");
  if (!out) {
    notify("Mediathek: can't write " INDEX_TMP);
    return 1;
  }
  fprintf(out, "mediathek-index 1\n");
  fprintf(out, "T\t%lld\n", (long long)time(NULL));
#ifdef MT_TEST_ROOT
  scan_root(out, MT_TEST_ROOT); /* PC test build: one folder, no console */
#ifdef MT_TEST_ROOT_ALIAS
  scan_root(out, MT_TEST_ROOT_ALIAS); /* the same folder under another name */
#endif
#else
  /* the same folders, in the same order, as default_roots() in src/media/library.cpp */
  static const char *const fixed[] = {"/data/movies", "/data/Movies", "/movies", 0};
  for (int i = 0; fixed[i]; i++) scan_root(out, fixed[i]);
  for (int i = 0; i < 8; i++) {
    char root[64];
    snprintf(root, sizeof root, "/mnt/usb%d/movies", i);
    scan_root(out, root);
    snprintf(root, sizeof root, "/mnt/usb%d/Movies", i);
    scan_root(out, root);
  }
  for (int i = 0; i < 2; i++) {
    char root[64];
    snprintf(root, sizeof root, "/mnt/ext%d/movies", i);
    scan_root(out, root);
    snprintf(root, sizeof root, "/mnt/ext%d/Movies", i);
    scan_root(out, root);
  }
#endif
  fprintf(out, "end\n");
  fclose(out);
  chmod(INDEX_TMP, 0666);
  if (rename(INDEX_TMP, INDEX)) {
    notify("Mediathek: can't write " INDEX);
    return 1;
  }
  /* The app says how many it found, when its Debug info setting is on. */
  return 0;
}
