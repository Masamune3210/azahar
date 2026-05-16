// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <QDir>
#include <QFileInfo>
#include "citra_qt/compatibility_list.h"
#include "citra_qt/game_list.h"
#include "citra_qt/game_list_p.h"
#include "citra_qt/game_list_worker.h"
#include "citra_qt/uisettings.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"

namespace {
bool HasSupportedFileExtension(const std::string& file_name) {
    const QFileInfo file = QFileInfo(QString::fromStdString(file_name));
    return GameList::supported_file_extensions.contains(file.suffix(), Qt::CaseInsensitive);
}
} // Anonymous namespace

GameListWorker::GameListWorker(QVector<UISettings::GameDir>& game_dirs,
                               const CompatibilityList& compatibility_list,
                               const PlayTime::PlayTimeManager& play_time_manager_)
    : game_dirs(game_dirs), compatibility_list(compatibility_list),
      play_time_manager{play_time_manager_} {}

GameListWorker::~GameListWorker() = default;

void GameListWorker::AddFstEntriesToGameList(const std::string& dir_path, unsigned int recursion,
                                             GameListDir* parent_dir,
                                             Service::FS::MediaType media_type) {
    const auto callback = [this, recursion, parent_dir,
                           media_type](u64* num_entries_out, const std::string& directory,
                                       const std::string& virtual_name) -> bool {
        if (stop_processing) {
            // Breaks the callback loop.
            return false;
        }

        const std::string physical_name = directory + DIR_SEP + virtual_name;
        const bool is_dir = FileUtil::IsDirectory(physical_name);
        if (!is_dir && HasSupportedFileExtension(physical_name)) {
            std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(physical_name);
            if (!loader) {
                return true;
            }

            bool executable = false;
            const auto res = loader->IsExecutable(executable);
            if (!executable && res != Loader::ResultStatus::ErrorEncrypted) {
                return true;
            }

            u64 program_id = 0;
            loader->ReadProgramId(program_id);

            u64 extdata_id = 0;
            loader->ReadExtdataId(extdata_id);

            std::vector<u8> smdh;
            // Look for an update icon if available
            if (!(program_id & ~0x00040000FFFFFFFF)) {
                std::string update_path = Service::AM::GetTitleContentPath(
                    Service::FS::MediaType::SDMC, program_id | 0x0000000E00000000);
                if (FileUtil::Exists(update_path)) {
                    std::unique_ptr<Loader::AppLoader> update_loader =
                        Loader::GetLoader(update_path);
                    if (update_loader) {
                        update_loader->ReadIcon(smdh);
                    }
                }
            }

            if (!Loader::IsValidSMDH(smdh)) {
                // Read the original smdh if there is no valid update smdh
                loader->ReadIcon(smdh);
            }

            const auto system_title = ((program_id >> 32) & 0xFFFFFFFF) == 0x00040010;
            if (Loader::IsValidSMDH(smdh)) {
                if (system_title) {
                    auto smdh_struct = reinterpret_cast<Loader::SMDH*>(smdh.data());
                    if (!smdh_struct->flags.visible) {
                        // Skip system titles without the visible flag.
                        return true;
                    }
                }
            } else if (UISettings::values.game_list_hide_no_icon || system_title) {
                // Skip this invalid entry
                return true;
            }

            auto it = FindMatchingCompatibilityEntry(compatibility_list, program_id);

            // The game list uses this as compatibility number for untested games
            QString compatibility(QStringLiteral("99"));
            if (it != compatibility_list.end())
                compatibility = it->second.first;

            const bool is_encrypted = res == Loader::ResultStatus::ErrorEncrypted;
            const bool can_insert = loader->GetFileType() == Loader::FileType::CCI;
            const QString file_type_str = QString::fromStdString(
                Loader::GetFileTypeString(loader->GetFileType(), loader->IsFileCompressed()));
            const quint64 file_size = FileUtil::GetSize(physical_name);

            emit EntryReady(
                {
                    new GameListItemPath(QString::fromStdString(physical_name), smdh, program_id,
                                         extdata_id, media_type, is_encrypted, can_insert),
                    new GameListItemCompat(compatibility),
                    new GameListItemRegion(smdh),
                    new GameListItem(file_type_str),
                    new GameListItemSize(file_size),
                    new GameListItemPlayTime(play_time_manager.GetPlayTime(program_id)),
                },
                parent_dir);

            {
                GameListCacheEntry cache_entry;
                cache_entry.path          = QString::fromStdString(physical_name);
                cache_entry.smdh          = QByteArray(reinterpret_cast<const char*>(smdh.data()),
                                                        static_cast<qsizetype>(smdh.size()));
                cache_entry.program_id    = program_id;
                cache_entry.extdata_id    = extdata_id;
                cache_entry.media_type    = media_type;
                cache_entry.is_encrypted  = is_encrypted;
                cache_entry.can_insert    = can_insert;
                cache_entry.compatibility = compatibility;
                cache_entry.file_type     = file_type_str;
                cache_entry.file_size     = file_size;
                // game_dir_index is filled in by GameList when it receives this signal.
                emit CacheEntryReady(std::move(cache_entry));
            }

        } else if (is_dir && recursion > 0) {
            watch_list.append(QString::fromStdString(physical_name));
            AddFstEntriesToGameList(physical_name, recursion - 1, parent_dir, media_type);
        }

        return true;
    };

    FileUtil::ForeachDirectoryEntry(nullptr, dir_path, callback);
}

void GameListWorker::run() {
    stop_processing = false;
    for (UISettings::GameDir& game_dir : game_dirs) {
        if (game_dir.path == QStringLiteral("INSTALLED")) {
            QString games_path =
                QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)) +
                QStringLiteral("Nintendo "
                               "3DS/00000000000000000000000000000000/"
                               "00000000000000000000000000000000/title/00040000");
            QString demos_path =
                QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)) +
                QStringLiteral(
                    "Nintendo "
                    "3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/"
                    "00040002");
            watch_list.append(games_path);
            watch_list.append(demos_path);
            auto* const game_list_dir = new GameListDir(game_dir, GameListItemType::InstalledDir);
            emit DirEntryReady(game_list_dir);
            AddFstEntriesToGameList(games_path.toStdString(), 2, game_list_dir,
                                    Service::FS::MediaType::SDMC);
            AddFstEntriesToGameList(demos_path.toStdString(), 2, game_list_dir,
                                    Service::FS::MediaType::SDMC);
        } else if (game_dir.path == QStringLiteral("SYSTEM")) {
            QString path =
                QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::NANDDir)) +
                QStringLiteral("00000000000000000000000000000000/title/00040010");
            watch_list.append(path);
            auto* const game_list_dir = new GameListDir(game_dir, GameListItemType::SystemDir);
            emit DirEntryReady(game_list_dir);
            AddFstEntriesToGameList(path.toStdString(), 2, game_list_dir,
                                    Service::FS::MediaType::NAND);
        } else {
            watch_list.append(game_dir.path);
            auto* const game_list_dir = new GameListDir(game_dir);
            emit DirEntryReady(game_list_dir);
            AddFstEntriesToGameList(game_dir.path.toStdString(), game_dir.deep_scan ? 256 : 0,
                                    game_list_dir, Service::FS::MediaType::GameCard);
        }
    }

    emit Finished(watch_list);
}

void GameListWorker::Cancel() {
    this->disconnect();
    stop_processing = true;
}
