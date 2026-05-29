// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/directory_cache.h"

namespace Common {

namespace {

std::string NormalizePath(const std::string& path) {
    std::string normalized = path;
    while (normalized.size() > 1 &&
           (normalized.back() == '/' || normalized.back() == '\\')) {
        normalized.pop_back();
    }
    return normalized;
}

} // namespace

DirectoryCache& DirectoryCache::Instance() {
    static DirectoryCache instance;
    return instance;
}

std::vector<FileUtil::FSTEntry> DirectoryCache::ListFlat(const std::string& directory) {
    const std::string key = NormalizePath(directory);

    {
        std::scoped_lock lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }

    // First-time miss: walk the directory once, outside the lock so a slow scan does not
    // block readers of unrelated directories.
    FileUtil::FSTEntry entries;
    FileUtil::ScanDirectoryTree(directory, entries, 0);

    if (!FileUtil::IsDirectory(directory)) {
        // Don't cache a snapshot for a non-existent directory -- if it shows up later,
        // the next call should see it.
        return std::move(entries.children);
    }

    std::scoped_lock lock(mutex);
    auto [it, inserted] = cache.try_emplace(key, std::move(entries.children));
    return it->second;
}

void DirectoryCache::Invalidate(const std::string& directory) {
    std::scoped_lock lock(mutex);
    cache.erase(NormalizePath(directory));
}

void DirectoryCache::Clear() {
    std::scoped_lock lock(mutex);
    cache.clear();
}

} // namespace Common
