// Mediathek - The table of texts: id, English, German.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Included twice by i18n.hpp / i18n.cpp. "{}" marks where a time, a name or a
// number goes in (format()). To add a language, add a column here and a
// Language in i18n.hpp.

// ---- shelves and pages
HUI_TEXT(shelf_continue, "Continue Watching", "Weiterschauen")
HUI_TEXT(shelf_recent, "Recently Added", "Neu hinzugefügt")
HUI_TEXT(shelf_all, "All Movies", "Alle Filme")
HUI_TEXT(shelf_yours, "Your Movies", "Deine Filme")
HUI_TEXT(decade, "{}s", "{}er")
HUI_TEXT(tab_home, "Home", "Start")
HUI_TEXT(tab_movies, "Movies", "Filme")
HUI_TEXT(movies_one, "1 movie", "1 Film")
HUI_TEXT(movies_many, "{} movies", "{} Filme")
HUI_TEXT(scanning, "Scanning\xE2\x80\xA6", "Suche\xE2\x80\xA6")
HUI_TEXT(sorted_by, "Sorted by {}", "Sortiert nach {}")
HUI_TEXT(sort_title, "title", "Titel")
HUI_TEXT(sort_newest, "newest", "Neueste")
HUI_TEXT(sort_year, "year", "Jahr")

// ---- nothing found yet
HUI_TEXT(looking_for_movies, "Looking for movies\xE2\x80\xA6", "Suche nach Filmen\xE2\x80\xA6")
HUI_TEXT(no_movies_found, "No movies found", "Keine Filme gefunden")
HUI_TEXT(put_files_in, "Put your files in a folder called movies:",
         "Lege deine Dateien in einen Ordner namens movies:")
HUI_TEXT(place_internal, "/data/movies  (internal)", "/data/movies  (intern)")
HUI_TEXT(place_usb, "/mnt/usb0/movies  (USB drive)", "/mnt/usb0/movies  (USB-Laufwerk)")
HUI_TEXT(place_extended, "/mnt/ext0/movies  (extended storage)",
         "/mnt/ext0/movies  (erweiterter Speicher)")
HUI_TEXT(name_them,
         "Name them like \"Blue Harbor (2011).mkv\"; a poster.jpg next to a movie becomes its "
         "cover.",
         "Benenne sie wie \xE2\x80\x9E" "Blue Harbor (2011).mkv\xE2\x80\x9C; eine poster.jpg neben "
         "einem Film wird sein Cover.")

// ---- the billboard, the details page and its actions
HUI_TEXT(button_resume, "Resume", "Fortsetzen")
HUI_TEXT(button_play, "Play", "Abspielen")
HUI_TEXT(button_details, "Details", "Details")
HUI_TEXT(resume_from, "Resume from {}", "Fortsetzen ab {}")
HUI_TEXT(start_over, "Start from the beginning", "Von vorn beginnen")
HUI_TEXT(mark_watched, "Mark as watched", "Als gesehen markieren")
HUI_TEXT(mark_unwatched, "Mark as not watched", "Als nicht gesehen markieren")
HUI_TEXT(refresh_info, "Refresh cover and info", "Cover & Infos neu laden")
HUI_TEXT(action_back, "Back", "Zur\xC3\xBC" "ck")
HUI_TEXT(time_left, "{} left", "noch {}")
HUI_TEXT(label_file, "FILE", "DATEI")
HUI_TEXT(label_status, "STATUS", "STATUS")
HUI_TEXT(status_watched, "Watched", "Gesehen")
HUI_TEXT(status_not_started, "Not started", "Nicht begonnen")
HUI_TEXT(status_at, "At {}", "Bei {}")
HUI_TEXT(status_subtitles, "Subtitles", "Untertitel")
HUI_TEXT(no_description_try, "No description yet. Try \"Refresh cover and info\".",
         "Noch keine Beschreibung. Probiere \xE2\x80\x9E" "Cover & Infos neu laden"
         "\xE2\x80\x9C.")
HUI_TEXT(no_description, "No description available.", "Keine Beschreibung verf\xC3\xBC" "gbar.")

// ---- messages
HUI_TEXT(press_back_again, "Press Back again to exit", "Zum Beenden nochmals Zur\xC3\xBC" "ck dr\xC3\xBC" "cken")
HUI_TEXT(fetching_info, "Fetching cover, wallpaper and description\xE2\x80\xA6",
         "Lade Cover, Hintergrundbild und Beschreibung\xE2\x80\xA6")
HUI_TEXT(marked_watched, "Marked as watched", "Als gesehen markiert")
HUI_TEXT(marked_unwatched, "Marked as not watched", "Als nicht gesehen markiert")
HUI_TEXT(playback_unavailable, "Playback is not available", "Wiedergabe nicht verf\xC3\xBC" "gbar")
HUI_TEXT(finished, "Finished", "Beendet")
HUI_TEXT(audio_track, "Audio: {}", "Ton: {}")
HUI_TEXT(subtitles_on, "Subtitles on", "Untertitel an")
HUI_TEXT(subtitles_off, "Subtitles off", "Untertitel aus")
HUI_TEXT(unknown_movie, "Unknown movie", "Unbekannter Film")

// ---- button hints
HUI_TEXT(hint_choose, "Choose", "Ausw\xC3\xA4hlen")
HUI_TEXT(hint_back, "Back", "Zur\xC3\xBC" "ck")
HUI_TEXT(hint_look_again, "Look again", "Neu suchen")
HUI_TEXT(hint_details, "Details", "Details")
HUI_TEXT(hint_play, "Play", "Abspielen")
HUI_TEXT(hint_watched, "Watched", "Gesehen")
HUI_TEXT(hint_tab, "Tab", "Reiter")
HUI_TEXT(hint_sort, "Sort", "Sortieren")
HUI_TEXT(hint_pause, "Pause", "Pause")
HUI_TEXT(hint_seek, "Seek", "Spulen")
HUI_TEXT(hint_audio, "Audio", "Ton")
HUI_TEXT(hint_subtitles, "Subtitles", "Untertitel")
HUI_TEXT(hint_stop, "Stop", "Stopp")

// ---- the player
HUI_TEXT(wont_play, "This movie will not play", "Dieser Film l\xC3\xA4sst sich nicht abspielen")
HUI_TEXT(file_not_opened, "The file could not be opened.", "Die Datei konnte nicht ge\xC3\xB6" "ffnet werden.")
HUI_TEXT(opening, "Opening\xE2\x80\xA6", "\xC3\x96" "ffne\xE2\x80\xA6")
HUI_TEXT(err_open_file, "Cannot open the file", "Datei kann nicht ge\xC3\xB6" "ffnet werden")
HUI_TEXT(err_read_file, "Cannot read the file", "Datei kann nicht gelesen werden")
HUI_TEXT(err_nothing_to_play, "Nothing to play in this file", "Nichts zum Abspielen in dieser Datei")
HUI_TEXT(err_no_picture_decoder, "No decoder for this picture format",
         "Kein Decoder f\xC3\xBC" "r dieses Bildformat")
HUI_TEXT(err_start_decoder, "Cannot start the picture decoder",
         "Bilddecoder l\xC3\xA4sst sich nicht starten")

// ---- where a movie lives
HUI_TEXT(drive_internal, "Internal", "Intern")
HUI_TEXT(drive_usb, "USB {}", "USB {}")
HUI_TEXT(drive_extended, "Drive {}", "Laufwerk {}")
HUI_TEXT(drive_library, "Library", "Bibliothek")

// ---- the scan helper
HUI_TEXT(scan_missing, "The scanner (mt-scan.elf) is missing from the app folder.",
         "Der Scanner (mt-scan.elf) fehlt im App-Ordner.")
HUI_TEXT(scan_no_socket, "No network socket for the ELF loader.",
         "Kein Netzwerk-Socket f\xC3\xBC" "r den ELF-Loader.")
HUI_TEXT(scan_no_loader,
         "Nothing answers on port 9021: start the ELF loader (etaHEN) and press Options.",
         "Auf Port 9021 antwortet nichts: ELF-Loader (etaHEN) starten und Optionen dr\xC3\xBC"
         "cken.")
HUI_TEXT(scan_loader_closed, "The ELF loader closed the connection while the scanner was sent.",
         "Der ELF-Loader hat die Verbindung beendet, w\xC3\xA4hrend der Scanner gesendet wurde.")
HUI_TEXT(scan_running, "Scanning the drives\xE2\x80\xA6", "Laufwerke werden durchsucht\xE2\x80\xA6")
HUI_TEXT(scan_silent, "The scanner did not report back. Is the ELF loader running?",
         "Der Scanner hat sich nicht gemeldet. L\xC3\xA4uft der ELF-Loader?")

// ---- covers, wallpapers and descriptions from TMDB
HUI_TEXT(tmdb_no_key, "No TMDB_API_KEY in mediathek.env", "Kein TMDB_API_KEY in mediathek.env")
HUI_TEXT(tmdb_bad_key, "TMDB_API_KEY has characters that do not belong in a key",
         "TMDB_API_KEY enth\xC3\xA4lt Zeichen, die nicht in einen Schl\xC3\xBC" "ssel geh\xC3\xB6" "ren")
HUI_TEXT(tmdb_not_a_picture, "The service answered with something that is not a picture",
         "Der Dienst hat etwas geliefert, das kein Bild ist")
HUI_TEXT(tmdb_cannot_save_cover, "Cannot save a cover: {}",
         "Cover kann nicht gespeichert werden: {}")
HUI_TEXT(tmdb_cannot_save_description, "Cannot save a description: {}",
         "Beschreibung kann nicht gespeichert werden: {}")
HUI_TEXT(tmdb_unreachable, "Could not reach the cover service: {}",
         "Cover-Dienst nicht erreichbar: {}")
