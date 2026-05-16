// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/archives.h"
#include "common/common_paths.h"
#include "common/error.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/file_sys/archive_nand.h"
#include "core/file_sys/disk_archive.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/layered_fs.h"
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/path_parser.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"

SERIALIZE_EXPORT_IMPL(FileSys::VirtualFile)
SERIALIZE_EXPORT_IMPL(FileSys::NANDArchive)
SERIALIZE_EXPORT_IMPL(FileSys::ArchiveFactory_NAND)

namespace FileSys {

// TODO(PabloMK7): This code is very similar to the SMDC archive code. Maybe we should look
// into unifying everything in a FAT-like archive, as both the SMDC and NAND archives
// seem to behave the same way.

namespace {

constexpr u64 SYSTEM_DATA_ARCHIVE_TITLE_ID_HIGH = 0x000400DBULL;
constexpr std::array<u32, 7> NVER_OLD_3DS_TITLE_ID_LOWS = {
    0x00016202, 0x00016302, 0x00016102, 0x00016102, 0x00016402, 0x00016502, 0x00016602};
constexpr std::array<u32, 7> NVER_NEW_3DS_TITLE_ID_LOWS = {
    0x20016202, 0x20016302, 0x20016102, 0x20016102, 0, 0x20016502, 0};
constexpr std::array<u32, 7> CVER_TITLE_ID_LOWS = {
    0x00017202, 0x00017302, 0x00017102, 0x00017102, 0x00017402, 0x00017502, 0x00017602};

constexpr std::size_t VERSION_BIN_SIZE = 8;
constexpr u32 INVALID_ROMFS_OFFSET = 0xFFFFFFFF;

struct RomFSDirectoryMetadata {
    u32_le parent_directory_offset;
    u32_le next_sibling_offset;
    u32_le first_child_directory_offset;
    u32_le first_file_offset;
    u32_le hash_bucket_next;
    u32_le name_length;
};
static_assert(sizeof(RomFSDirectoryMetadata) == 0x18,
              "Size of RomFSDirectoryMetadata is not correct");

struct RomFSFileMetadata {
    u32_le parent_directory_offset;
    u32_le next_sibling_offset;
    u64_le file_data_offset;
    u64_le file_data_length;
    u32_le hash_bucket_next;
    u32_le name_length;
};
static_assert(sizeof(RomFSFileMetadata) == 0x20, "Size of RomFSFileMetadata is not correct");

template <typename T>
bool ReadRomFSStruct(RomFSReader& romfs, std::size_t offset, T& out) {
    return romfs.ReadFile(offset, sizeof(T), reinterpret_cast<u8*>(&out)) == sizeof(T);
}

std::string ReadRomFSName(RomFSReader& romfs, std::size_t offset, u32 name_length) {
    std::vector<u16_le> buffer(name_length / sizeof(u16_le));
    if (romfs.ReadFile(offset, name_length, reinterpret_cast<u8*>(buffer.data())) != name_length) {
        return {};
    }

    std::u16string name(buffer.size(), 0);
    std::transform(buffer.begin(), buffer.end(), name.begin(), [](u16_le character) {
        return static_cast<char16_t>(static_cast<u16>(character));
    });
    return Common::UTF16ToUTF8(name);
}

std::optional<std::array<u8, VERSION_BIN_SIZE>> ReadRootRomFSFile(RomFSReader& romfs,
                                                                  std::string_view target_name) {
    RomFSHeader header{};
    if (!ReadRomFSStruct(romfs, 0, header)) {
        return std::nullopt;
    }

    RomFSDirectoryMetadata root_directory{};
    if (!ReadRomFSStruct(romfs, header.directory_metadata_table.offset, root_directory)) {
        return std::nullopt;
    }

    u32 file_offset = root_directory.first_file_offset;
    while (file_offset != INVALID_ROMFS_OFFSET) {
        RomFSFileMetadata file{};
        const auto metadata_offset = header.file_metadata_table.offset + file_offset;
        if (!ReadRomFSStruct(romfs, metadata_offset, file)) {
            return std::nullopt;
        }

        const std::string name =
            ReadRomFSName(romfs, metadata_offset + sizeof(file), file.name_length);
        if (name == target_name && file.file_data_length == VERSION_BIN_SIZE) {
            std::array<u8, VERSION_BIN_SIZE> data{};
            const std::size_t data_offset = header.file_data_offset + file.file_data_offset;
            if (romfs.ReadFile(data_offset, data.size(), data.data()) != data.size()) {
                return std::nullopt;
            }
            return data;
        }

        file_offset = file.next_sibling_offset;
    }

    return std::nullopt;
}

std::vector<std::size_t> GetRegionSearchOrder() {
    std::vector<std::size_t> order;
    const auto configured_region = Settings::values.region_value.GetValue();
    if (configured_region >= 0 && configured_region < static_cast<int>(CVER_TITLE_ID_LOWS.size())) {
        order.push_back(static_cast<std::size_t>(configured_region));
    }

    for (std::size_t region = 0; region < CVER_TITLE_ID_LOWS.size(); ++region) {
        if (std::find(order.begin(), order.end(), region) == order.end()) {
            order.push_back(region);
        }
    }
    return order;
}

std::optional<std::array<u8, VERSION_BIN_SIZE>> ReadVersionBin(u64 title_id) {
    const std::string content_path =
        Service::AM::GetTitleContentPath(Service::FS::MediaType::NAND, title_id);
    if (!FileUtil::Exists(content_path)) {
        return std::nullopt;
    }

    NCCHContainer container(content_path);
    std::shared_ptr<RomFSReader> romfs;
    if (container.ReadRomFS(romfs, false) != Loader::ResultStatus::Success || !romfs) {
        return std::nullopt;
    }

    return ReadRootRomFSFile(*romfs, "version.bin");
}

std::optional<std::array<u8, VERSION_BIN_SIZE>> FindInstalledVersionBin(
    const std::array<u32, 7>& title_id_lows) {
    for (const std::size_t region : GetRegionSearchOrder()) {
        const u32 title_id_low = title_id_lows[region];
        if (title_id_low == 0) {
            continue;
        }

        const u64 title_id = (SYSTEM_DATA_ARCHIVE_TITLE_ID_HIGH << 32) | title_id_low;
        if (auto version = ReadVersionBin(title_id)) {
            return version;
        }
    }

    return std::nullopt;
}

std::optional<std::array<u8, VERSION_BIN_SIZE>> FindInstalledNVer() {
    const bool is_new_3ds = Settings::values.is_new_3ds.GetValue();
    if (auto version =
            FindInstalledVersionBin(is_new_3ds ? NVER_NEW_3DS_TITLE_ID_LOWS
                                               : NVER_OLD_3DS_TITLE_ID_LOWS)) {
        return version;
    }

    return FindInstalledVersionBin(is_new_3ds ? NVER_OLD_3DS_TITLE_ID_LOWS
                                              : NVER_NEW_3DS_TITLE_ID_LOWS);
}

std::optional<std::string> BuildProductLog() {
    const auto cver = FindInstalledVersionBin(CVER_TITLE_ID_LOWS);
    const auto nver = FindInstalledNVer();
    if (!cver || !nver) {
        return std::nullopt;
    }

    const std::string cup = std::to_string((*cver)[2]) + "." + std::to_string((*cver)[1]) + "." +
                            std::to_string((*cver)[0]);
    const std::string nup = std::to_string((*nver)[2]) + static_cast<char>((*nver)[4]);
    return "nup:" + nup + " cup:" + cup + " preInstall:\n";
}

} // namespace

ResultVal<std::unique_ptr<FileBackend>> NANDArchive::OpenFile(const Path& path, const Mode& mode,
                                                              u32 attributes) {
    LOG_DEBUG(Service_FS, "called path={} mode={:01X}", path.DebugStr(), mode.hex);

    // TWL NAND has no host-filesystem counterpart in Azahar; serve a fixed set of virtual files.
    if (archive_type == NANDArchiveType::TWL) {
        if (mode != Mode::ReadOnly()) {
            return ResultInvalidOpenFlags;
        }
        const std::string path_str = path.AsString();
        if (path_str == "/sys/log/product.log") {
            if (auto product_log = BuildProductLog()) {
                return std::make_unique<VirtualFile>(*product_log);
            }
        }
        return ResultNotFound;
    }

    if (!AllowsWrite() && mode != Mode::ReadOnly()) {
        return ResultInvalidOpenFlags;
    }

    const PathParser path_parser(path);

    if (!path_parser.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid path {}", path.DebugStr());
        return ResultInvalidPath;
    }

    if (mode.hex == 0) {
        LOG_ERROR(Service_FS, "Empty open mode");
        return ResultInvalidOpenFlags;
    }

    if (mode.create_flag && !mode.write_flag) {
        LOG_ERROR(Service_FS, "Create flag set but write flag not set");
        return ResultInvalidOpenFlags;
    }

    const auto full_path = path_parser.BuildHostPath(mount_point);

    switch (path_parser.GetHostStatus(mount_point)) {
    case PathParser::InvalidMountPoint:
        LOG_CRITICAL(Service_FS, "(unreachable) Invalid mount point {}", mount_point);
        return ResultNotFound;
    case PathParser::PathNotFound:
    case PathParser::FileInPath:
        LOG_DEBUG(Service_FS, "Path not found {}", full_path);
        return ResultNotFound;
    case PathParser::DirectoryFound:
        LOG_DEBUG(Service_FS, "{} is not a file", full_path);
        return ResultUnexpectedFileOrDirectorySdmc;
    case PathParser::NotFound:
        if (!mode.create_flag) {
            LOG_DEBUG(Service_FS, "Non-existing file {} can't be open without mode create.",
                      full_path);
            return ResultNotFound;
        } else {
            // Create the file
            FileUtil::CreateEmptyFile(full_path);
        }
        break;
    case PathParser::FileFound:
        break; // Expected 'success' case
    }

    FileUtil::IOFile file(full_path, mode.write_flag ? "r+b" : "rb");
    if (!file.IsOpen()) {
        LOG_CRITICAL(Service_FS, "Error opening {}: {}", full_path, Common::GetLastErrorMsg());
        return ResultNotFound;
    }

    return std::make_unique<DiskFile>(std::move(file), mode, nullptr);
}

Result NANDArchive::DeleteFile(const Path& path) const {

    if (!AllowsWrite()) {
        return ResultInvalidOpenFlags;
    }

    const PathParser path_parser(path);

    if (!path_parser.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid path {}", path.DebugStr());
        return ResultInvalidPath;
    }

    const auto full_path = path_parser.BuildHostPath(mount_point);

    switch (path_parser.GetHostStatus(mount_point)) {
    case PathParser::InvalidMountPoint:
        LOG_CRITICAL(Service_FS, "(unreachable) Invalid mount point {}", mount_point);
        return ResultNotFound;
    case PathParser::PathNotFound:
    case PathParser::FileInPath:
    case PathParser::NotFound:
        LOG_DEBUG(Service_FS, "{} not found", full_path);
        return ResultNotFound;
    case PathParser::DirectoryFound:
        LOG_ERROR(Service_FS, "{} is not a file", full_path);
        return ResultUnexpectedFileOrDirectorySdmc;
    case PathParser::FileFound:
        break; // Expected 'success' case
    }

    if (FileUtil::Delete(full_path)) {
        return ResultSuccess;
    }

    LOG_CRITICAL(Service_FS, "(unreachable) Unknown error deleting {}", full_path);
    return ResultNotFound;
}

Result NANDArchive::RenameFile(const Path& src_path, const Path& dest_path) const {

    if (!AllowsWrite()) {
        return ResultInvalidOpenFlags;
    }

    const PathParser path_parser_src(src_path);

    // TODO: Verify these return codes with HW
    if (!path_parser_src.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid src path {}", src_path.DebugStr());
        return ResultInvalidPath;
    }

    const PathParser path_parser_dest(dest_path);

    if (!path_parser_dest.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid dest path {}", dest_path.DebugStr());
        return ResultInvalidPath;
    }

    const auto src_path_full = path_parser_src.BuildHostPath(mount_point);
    const auto dest_path_full = path_parser_dest.BuildHostPath(mount_point);

    if (FileUtil::Rename(src_path_full, dest_path_full)) {
        return ResultSuccess;
    }

    // TODO(yuriks): This code probably isn't right, it'll return a Status even if the file didn't
    // exist or similar. Verify.
    return Result(ErrorDescription::NoData, ErrorModule::FS, // TODO: verify description
                  ErrorSummary::NothingHappened, ErrorLevel::Status);
}

template <typename T>
static Result DeleteDirectoryHelper(const Path& path, const std::string& mount_point, T deleter) {
    const PathParser path_parser(path);

    if (!path_parser.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid path {}", path.DebugStr());
        return ResultInvalidPath;
    }

    if (path_parser.IsRootDirectory())
        return ResultInvalidOpenFlags;

    const auto full_path = path_parser.BuildHostPath(mount_point);

    switch (path_parser.GetHostStatus(mount_point)) {
    case PathParser::InvalidMountPoint:
        LOG_CRITICAL(Service_FS, "(unreachable) Invalid mount point {}", mount_point);
        return ResultNotFound;
    case PathParser::PathNotFound:
    case PathParser::NotFound:
        LOG_ERROR(Service_FS, "Path not found {}", full_path);
        return ResultNotFound;
    case PathParser::FileInPath:
    case PathParser::FileFound:
        LOG_ERROR(Service_FS, "Unexpected file in path {}", full_path);
        return ResultUnexpectedFileOrDirectorySdmc;
    case PathParser::DirectoryFound:
        break; // Expected 'success' case
    }

    if (deleter(full_path)) {
        return ResultSuccess;
    }

    LOG_ERROR(Service_FS, "Directory not empty {}", full_path);
    return ResultUnexpectedFileOrDirectorySdmc;
}

Result NANDArchive::DeleteDirectory(const Path& path) const {
    if (!AllowsWrite()) {
        return ResultInvalidOpenFlags;
    }

    return DeleteDirectoryHelper(path, mount_point, FileUtil::DeleteDir);
}

Result NANDArchive::DeleteDirectoryRecursively(const Path& path) const {
    if (!AllowsWrite()) {
        return ResultInvalidOpenFlags;
    }

    return DeleteDirectoryHelper(
        path, mount_point, [](const std::string& p) { return FileUtil::DeleteDirRecursively(p); });
}

Result NANDArchive::CreateFile(const FileSys::Path& path, u64 size, u32 attributes) const {
    if (!AllowsWrite()) {
        return ResultInvalidOpenFlags;
    }

    const PathParser path_parser(path);

    if (!path_parser.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid path {}", path.DebugStr());
        return ResultInvalidPath;
    }

    const auto full_path = path_parser.BuildHostPath(mount_point);

    switch (path_parser.GetHostStatus(mount_point)) {
    case PathParser::InvalidMountPoint:
        LOG_CRITICAL(Service_FS, "(unreachable) Invalid mount point {}", mount_point);
        return ResultNotFound;
    case PathParser::PathNotFound:
    case PathParser::FileInPath:
        LOG_ERROR(Service_FS, "Path not found {}", full_path);
        return ResultNotFound;
    case PathParser::DirectoryFound:
        LOG_ERROR(Service_FS, "{} already exists", full_path);
        return ResultUnexpectedFileOrDirectorySdmc;
    case PathParser::FileFound:
        LOG_ERROR(Service_FS, "{} already exists", full_path);
        return ResultAlreadyExists;
    case PathParser::NotFound:
        break; // Expected 'success' case
    }

    if (size == 0) {
        FileUtil::CreateEmptyFile(full_path);
        return ResultSuccess;
    }

    FileUtil::IOFile file(full_path, "wb");
    // Creates a sparse file (or a normal file on filesystems without the concept of sparse files)
    // We do this by seeking to the right size, then writing a single null byte.
    if (file.Seek(size - 1, SEEK_SET) && file.WriteBytes("", 1) == 1) {
        return ResultSuccess;
    }

    LOG_ERROR(Service_FS, "Too large file");
    return Result(ErrorDescription::TooLarge, ErrorModule::FS, ErrorSummary::OutOfResource,
                  ErrorLevel::Info);
}

Result NANDArchive::CreateDirectory(const Path& path, u32 attributes) const {
    if (!AllowsWrite()) {
        return ResultInvalidOpenFlags;
    }

    const PathParser path_parser(path);

    if (!path_parser.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid path {}", path.DebugStr());
        return ResultInvalidPath;
    }

    const auto full_path = path_parser.BuildHostPath(mount_point);

    switch (path_parser.GetHostStatus(mount_point)) {
    case PathParser::InvalidMountPoint:
        LOG_CRITICAL(Service_FS, "(unreachable) Invalid mount point {}", mount_point);
        return ResultNotFound;
    case PathParser::PathNotFound:
    case PathParser::FileInPath:
        LOG_ERROR(Service_FS, "Path not found {}", full_path);
        return ResultNotFound;
    case PathParser::DirectoryFound:
    case PathParser::FileFound:
        LOG_DEBUG(Service_FS, "{} already exists", full_path);
        return ResultAlreadyExists;
    case PathParser::NotFound:
        break; // Expected 'success' case
    }

    if (FileUtil::CreateDir(mount_point + path.AsString())) {
        return ResultSuccess;
    }

    LOG_CRITICAL(Service_FS, "(unreachable) Unknown error creating {}", mount_point);
    return Result(ErrorDescription::NoData, ErrorModule::FS, ErrorSummary::Canceled,
                  ErrorLevel::Status);
}

Result NANDArchive::RenameDirectory(const Path& src_path, const Path& dest_path) const {
    if (!AllowsWrite()) {
        return ResultInvalidOpenFlags;
    }

    const PathParser path_parser_src(src_path);

    // TODO: Verify these return codes with HW
    if (!path_parser_src.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid src path {}", src_path.DebugStr());
        return ResultInvalidPath;
    }

    const PathParser path_parser_dest(dest_path);

    if (!path_parser_dest.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid dest path {}", dest_path.DebugStr());
        return ResultInvalidPath;
    }

    const auto src_path_full = path_parser_src.BuildHostPath(mount_point);
    const auto dest_path_full = path_parser_dest.BuildHostPath(mount_point);

    if (FileUtil::Rename(src_path_full, dest_path_full)) {
        return ResultSuccess;
    }

    // TODO(yuriks): This code probably isn't right, it'll return a Status even if the file didn't
    // exist or similar. Verify.
    return Result(ErrorDescription::NoData, ErrorModule::FS, // TODO: verify description
                  ErrorSummary::NothingHappened, ErrorLevel::Status);
}

ResultVal<std::unique_ptr<DirectoryBackend>> NANDArchive::OpenDirectory(const Path& path) {
    const PathParser path_parser(path);

    if (!path_parser.IsValid()) {
        LOG_ERROR(Service_FS, "Invalid path {}", path.DebugStr());
        return ResultInvalidPath;
    }

    const auto full_path = path_parser.BuildHostPath(mount_point);

    switch (path_parser.GetHostStatus(mount_point)) {
    case PathParser::InvalidMountPoint:
        LOG_CRITICAL(Service_FS, "(unreachable) Invalid mount point {}", mount_point);
        return ResultNotFound;
    case PathParser::PathNotFound:
    case PathParser::NotFound:
    case PathParser::FileFound:
        LOG_DEBUG(Service_FS, "{} not found", full_path);
        return ResultNotFound;
    case PathParser::FileInPath:
        LOG_DEBUG(Service_FS, "Unexpected file in path {}", full_path);
        return ResultUnexpectedFileOrDirectorySdmc;
    case PathParser::DirectoryFound:
        break; // Expected 'success' case
    }

    return std::make_unique<DiskDirectory>(full_path);
}

u64 NANDArchive::GetFreeBytes() const {
    // TODO: Stubbed to return 1GiB
    return 1024 * 1024 * 1024;
}

ArchiveFactory_NAND::ArchiveFactory_NAND(const std::string& nand_directory, NANDArchiveType type)
    : nand_directory(nand_directory), archive_type(type) {

    LOG_DEBUG(Service_FS, "Directory {} set as NAND.", nand_directory);
}

bool ArchiveFactory_NAND::Initialize() {
    // TWL NAND is entirely virtual; no host directory is created or required.
    if (archive_type == NANDArchiveType::TWL) {
        return true;
    }

    if (!FileUtil::CreateFullPath(GetPath())) {
        LOG_ERROR(Service_FS, "Unable to create NAND path.");
        return false;
    }

    return true;
}

std::string ArchiveFactory_NAND::GetPath() {
    switch (archive_type) {
    case NANDArchiveType::RW:
        return PathParser("/rw").BuildHostPath(nand_directory) + DIR_SEP;
    case NANDArchiveType::RO:
    case NANDArchiveType::RO_W:
        return PathParser("/ro").BuildHostPath(nand_directory) + DIR_SEP;
    case NANDArchiveType::ROOT:
        return nand_directory;
    case NANDArchiveType::TWL:
        return PathParser("/twl").BuildHostPath(nand_directory) + DIR_SEP;
    default:
        break;
    }

    UNREACHABLE();
    return "";
}

ResultVal<std::unique_ptr<ArchiveBackend>> ArchiveFactory_NAND::Open(const Path& path,
                                                                     u64 program_id) {
    return std::make_unique<NANDArchive>(GetPath(), archive_type);
}

Result ArchiveFactory_NAND::Format(const Path& path, const FileSys::ArchiveFormatInfo& format_info,
                                   u64 program_id, u32 directory_buckets, u32 file_buckets) {
    // TODO(PabloMK7): Find proper error code
    LOG_ERROR(Service_FS, "Unimplemented Format archive {}", GetName());
    return UnimplementedFunction(ErrorModule::FS);
}

ResultVal<ArchiveFormatInfo> ArchiveFactory_NAND::GetFormatInfo(const Path& path,
                                                                u64 program_id) const {
    // TODO(PabloMK7): Implement
    LOG_ERROR(Service_FS, "Unimplemented GetFormatInfo archive {}", GetName());
    return UnimplementedFunction(ErrorModule::FS);
}
} // namespace FileSys
