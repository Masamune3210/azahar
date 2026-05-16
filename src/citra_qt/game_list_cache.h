// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include "citra_qt/uisettings.h"
#include "common/common_types.h"

namespace Service::FS {
enum class MediaType : u32;
}

/// Cached metadata for a single game entry, sufficient to rebuild all model columns.
struct GameListCacheEntry {
    QString path;
    QByteArray smdh;          ///< Raw SMDH bytes; empty for games that have no icon data.
    u64 program_id = 0;
    u64 extdata_id = 0;
    Service::FS::MediaType media_type{};
    bool is_encrypted = false;
    bool can_insert = false;
    QString compatibility;    ///< Tier string: "0"–"5" or "99" for not tested.
    QString file_type;        ///< Human-readable file-type string from the loader.
    quint64 file_size = 0;
    int game_dir_index = 0;   ///< Index into UISettings::values.game_dirs for the parent dir.
};

/**
 * Attempts to load the game list cache and validates it against the current game_dirs.
 *
 * Returns true and populates @p out_entries when the cache exists, parses without error,
 * and its stored directory list matches the caller's game_dirs exactly.
 * Returns false in all other cases (missing file, parse error, stale dirs).
 */
bool LoadGameListCache(const QVector<UISettings::GameDir>& game_dirs,
                       QVector<GameListCacheEntry>& out_entries);

/**
 * Serialises @p entries to the game list cache file, storing @p game_dirs alongside them
 * so that future loads can detect when the directory list has changed.
 */
void SaveGameListCache(const QVector<UISettings::GameDir>& game_dirs,
                       const QVector<GameListCacheEntry>& entries);

/// Removes the cache file so the next PopulateAsync performs a full scan.
void InvalidateGameListCache();
