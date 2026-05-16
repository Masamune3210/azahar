// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "citra_qt/game_list_cache.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/hle/service/fs/archive.h"

namespace {

constexpr int CACHE_VERSION = 1;

QString GetCachePath() {
    return QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir)) +
           QStringLiteral("game_list_cache.json");
}

/// Returns a compact string fingerprint of @p game_dirs.
/// Any change to the list (paths, deep_scan flags, or order) yields a different string.
QString DirsSignature(const QVector<UISettings::GameDir>& game_dirs) {
    QStringList parts;
    parts.reserve(game_dirs.size());
    for (const auto& dir : game_dirs) {
        parts += dir.path + QLatin1Char('|') + (dir.deep_scan ? QLatin1Char('1') : QLatin1Char('0'));
    }
    return parts.join(QLatin1Char(';'));
}

} // namespace

bool LoadGameListCache(const QVector<UISettings::GameDir>& game_dirs,
                       QVector<GameListCacheEntry>& out_entries) {
    const QString path = GetCachePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        LOG_WARNING(Frontend, "Game list cache parse error: {}", err.errorString().toStdString());
        return false;
    }

    const QJsonObject root = doc.object();
    if (root[QStringLiteral("version")].toInt() != CACHE_VERSION) {
        return false;
    }
    if (root[QStringLiteral("dirs_signature")].toString() != DirsSignature(game_dirs)) {
        return false;
    }

    const QJsonArray arr = root[QStringLiteral("entries")].toArray();
    out_entries.clear();
    out_entries.reserve(arr.size());
    for (const QJsonValue& val : arr) {
        if (!val.isObject()) {
            out_entries.clear();
            return false;
        }
        const QJsonObject obj = val.toObject();
        GameListCacheEntry e;
        e.path          = obj[QStringLiteral("path")].toString();
        e.program_id    = obj[QStringLiteral("program_id")].toString().toULongLong(nullptr, 16);
        e.extdata_id    = obj[QStringLiteral("extdata_id")].toString().toULongLong(nullptr, 16);
        e.media_type    = static_cast<Service::FS::MediaType>(
                              obj[QStringLiteral("media_type")].toInt());
        e.is_encrypted  = obj[QStringLiteral("encrypted")].toBool();
        e.can_insert    = obj[QStringLiteral("can_insert")].toBool();
        e.smdh          = QByteArray::fromBase64(
                              obj[QStringLiteral("smdh")].toString().toLatin1());
        e.compatibility = obj[QStringLiteral("compat")].toString();
        e.file_type     = obj[QStringLiteral("file_type")].toString();
        // file_size is stored as a string to avoid JSON double precision limits.
        e.file_size     = obj[QStringLiteral("size")].toString().toULongLong();
        e.game_dir_index = obj[QStringLiteral("dir_idx")].toInt();
        out_entries.append(std::move(e));
    }
    return true;
}

void SaveGameListCache(const QVector<UISettings::GameDir>& game_dirs,
                       const QVector<GameListCacheEntry>& entries) {
    QJsonArray arr;
    for (const auto& e : entries) {
        QJsonObject obj;
        obj[QStringLiteral("path")]      = e.path;
        obj[QStringLiteral("program_id")] = QString::number(e.program_id, 16);
        obj[QStringLiteral("extdata_id")] = QString::number(e.extdata_id, 16);
        obj[QStringLiteral("media_type")] = static_cast<int>(e.media_type);
        obj[QStringLiteral("encrypted")]  = e.is_encrypted;
        obj[QStringLiteral("can_insert")] = e.can_insert;
        obj[QStringLiteral("smdh")]       = QString::fromLatin1(e.smdh.toBase64());
        obj[QStringLiteral("compat")]     = e.compatibility;
        obj[QStringLiteral("file_type")]  = e.file_type;
        obj[QStringLiteral("size")]       = QString::number(e.file_size);
        obj[QStringLiteral("dir_idx")]    = e.game_dir_index;
        arr.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("version")]        = CACHE_VERSION;
    root[QStringLiteral("dirs_signature")] = DirsSignature(game_dirs);
    root[QStringLiteral("entries")]        = arr;

    QFile file(GetCachePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_ERROR(Frontend, "Failed to write game list cache: {}", GetCachePath().toStdString());
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void InvalidateGameListCache() {
    QFile::remove(GetCachePath());
}
