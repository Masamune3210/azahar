// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "common/file_util.h"

namespace Common {

/**
 * Process-wide cache of directory listings. Once a directory has been populated, subsequent
 * reads return the cached entries without touching the filesystem -- intended for setups
 * where the title directory tree lives behind an expensive backing store (NAS / rclone mount
 * / network share) and every avoided syscall matters.
 *
 * The cache performs NO automatic staleness detection. Callers that intentionally mutate a
 * cached directory must invalidate it explicitly via Invalidate() or Clear(); otherwise the
 * stored snapshot is treated as authoritative for the lifetime of the process.
 */
class DirectoryCache {
public:
    static DirectoryCache& Instance();

    /**
     * Return the immediate children of `directory`. The returned vector is a snapshot; it can
     * be iterated without holding any internal lock. Empty when the directory does not exist
     * (and that empty result is not cached, so a later creation of the directory will be
     * picked up on the next call).
     */
    std::vector<FileUtil::FSTEntry> ListFlat(const std::string& directory);

    /// Drop the cache entry for `directory` (next ListFlat will rescan unconditionally).
    void Invalidate(const std::string& directory);

    /// Drop every cached directory.
    void Clear();

private:
    DirectoryCache() = default;

    std::mutex mutex;
    std::unordered_map<std::string, std::vector<FileUtil::FSTEntry>> cache;
};

} // namespace Common
