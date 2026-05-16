// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include "common/archives.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_systemsavedata.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/file_backend.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/service/mcu/mcu_rtc.h"
#include "core/hle/service/ptm/ptm.h"
#include "core/hle/service/ptm/ptm_gets.h"
#include "core/hle/service/ptm/ptm_play.h"
#include "core/hle/service/ptm/ptm_sets.h"
#include "core/hle/service/ptm/ptm_sysm.h"
#include "core/hle/service/ptm/ptm_u.h"

SERIALIZE_EXPORT_IMPL(Service::PTM::Module)
SERVICE_CONSTRUCT_IMPL(Service::PTM::Module)

namespace Service::PTM {

/// Values for the default gamecoin.dat file
static const GameCoin default_game_coin = {0x4F00, 42, 0, 0, 0, 2014, 12, 29};

constexpr u32 PTM_SYSTEM_SAVE_DATA_HIGH = 0;
constexpr u32 PTM_SYSTEM_SAVE_DATA_LOW = 0x00010022;
constexpr u32 PLAY_HISTORY_MAX_ENTRIES = 0x11D28;
constexpr u32 PLAY_HISTORY_FILE_SIZE = 0xD5DE8;
constexpr u32 PLAY_HISTORY_ENTRIES_OFFSET = 0x8;
constexpr u64 PLAY_HISTORY_SPECIAL_TITLE_ID = 0xFFFFFFFFFFFFFFFF;
constexpr u32 PLAY_HISTORY_EVENT_MASK = 0xF;
constexpr u32 PLAY_HISTORY_TIMESTAMP_MASK = 0x0FFFFFFF;

struct PlayHistoryHeader {
    u32_le start_index;
    u32_le total_entries;
};
static_assert(sizeof(PlayHistoryHeader) == PLAY_HISTORY_ENTRIES_OFFSET,
              "PlayHistoryHeader size is wrong");

struct PlayHistoryData {
    u32 start_index = 0;
    u32 total_entries = 0;
    std::vector<PlayHistoryEntry> entries;
};

struct PlayHistoryIpcEntry {
    u32_le title_id_high;
    u32_le title_id_low;
    u32_le info_timestamp;
};
static_assert(sizeof(PlayHistoryIpcEntry) == sizeof(PlayHistoryEntry),
              "PlayHistoryIpcEntry size is wrong");

static u32 GetPlayHistoryTimestamp(Core::System& system) {
    const u64 system_time_ms = system.Kernel().GetSharedPageHandler().GetSystemTimeSince2000();
    return static_cast<u32>((system_time_ms / 60000) & PLAY_HISTORY_TIMESTAMP_MASK);
}

static bool IsKnownTitleIdHigh(u32 title_id_high) {
    return title_id_high >= 0x00040000 && title_id_high <= 0x00048FFF;
}

static bool IsValidPlayHistoryData(const PlayHistoryData& data) {
    return data.start_index < PLAY_HISTORY_MAX_ENTRIES &&
           data.total_entries <= PLAY_HISTORY_MAX_ENTRIES &&
           data.entries.size() == PLAY_HISTORY_MAX_ENTRIES;
}

static FileSys::Path GetPtmSystemSaveDataPath() {
    return FileSys::ConstructSystemSaveDataBinaryPath(PTM_SYSTEM_SAVE_DATA_HIGH,
                                                      PTM_SYSTEM_SAVE_DATA_LOW);
}

static std::unique_ptr<FileSys::ArchiveBackend> OpenPtmSystemSaveDataArchive(bool create) {
    const std::string& nand_directory = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    FileSys::ArchiveFactory_SystemSaveData systemsavedata_archive_factory(nand_directory);
    const FileSys::Path archive_path = GetPtmSystemSaveDataPath();

    auto initial_archive_result = systemsavedata_archive_factory.Open(archive_path, 0);
    if (initial_archive_result.Succeeded()) {
        return std::move(initial_archive_result).Unwrap();
    }

    if (create) {
        const FileSys::ArchiveFormatInfo format_info{
            .total_size = PLAY_HISTORY_FILE_SIZE,
            .number_directories = 0,
            .number_files = 2,
            .duplicate_data = 0,
        };
        systemsavedata_archive_factory.Format(archive_path, format_info, 0, 1, 2);

        auto created_archive_result = systemsavedata_archive_factory.Open(archive_path, 0);
        if (created_archive_result.Succeeded()) {
            return std::move(created_archive_result).Unwrap();
        }
    }

    return nullptr;
}

static std::unique_ptr<FileSys::FileBackend> OpenPlayHistoryFile(FileSys::ArchiveBackend& archive,
                                                                 bool create) {
    FileSys::Path play_history_path("/PlayHistory.dat");
    FileSys::Mode open_mode = {};
    open_mode.read_flag.Assign(1);
    open_mode.write_flag.Assign(1);

    auto initial_play_history_result = archive.OpenFile(play_history_path, open_mode);
    if (initial_play_history_result.Succeeded()) {
        auto play_history = std::move(initial_play_history_result).Unwrap();
        if (play_history->GetSize() != PLAY_HISTORY_FILE_SIZE) {
            play_history->SetSize(PLAY_HISTORY_FILE_SIZE);
        }
        return play_history;
    }

    if (create) {
        archive.CreateFile(play_history_path, PLAY_HISTORY_FILE_SIZE);
        auto created_play_history_result = archive.OpenFile(play_history_path, open_mode);
        if (created_play_history_result.Succeeded()) {
            auto play_history = std::move(created_play_history_result).Unwrap();
            if (play_history->GetSize() != PLAY_HISTORY_FILE_SIZE) {
                play_history->SetSize(PLAY_HISTORY_FILE_SIZE);
            }
            return play_history;
        }
    }

    return nullptr;
}

static void WritePlayHistoryData(const PlayHistoryData& data) {
    auto archive = OpenPtmSystemSaveDataArchive(true);
    if (!archive) {
        LOG_ERROR(Service_PTM, "Could not open PTM SystemSaveData archive!");
        return;
    }

    auto play_history = OpenPlayHistoryFile(*archive, true);
    if (!play_history) {
        LOG_ERROR(Service_PTM, "Could not open PlayHistory.dat!");
        return;
    }

    PlayHistoryHeader header{
        .start_index = data.start_index,
        .total_entries = data.total_entries,
    };
    play_history->Write(0, sizeof(header), true, false, reinterpret_cast<const u8*>(&header));
    play_history->Write(PLAY_HISTORY_ENTRIES_OFFSET, data.entries.size() * sizeof(PlayHistoryEntry),
                        true, false, reinterpret_cast<const u8*>(data.entries.data()));
    play_history->Close();
}

static PlayHistoryData MakeEmptyPlayHistoryData() {
    PlayHistoryData data;
    data.entries.resize(PLAY_HISTORY_MAX_ENTRIES);
    for (auto& entry : data.entries) {
        entry = PlayHistoryEntry{
            .title_id_high = 0xFFFFFFFF,
            .title_id_low = 0xFFFFFFFF,
            .info_timestamp = 0xFFFFFFFF,
        };
    }
    return data;
}

static PlayHistoryData ReadPlayHistoryData(bool create) {
    auto archive = OpenPtmSystemSaveDataArchive(create);
    if (!archive) {
        return MakeEmptyPlayHistoryData();
    }

    auto play_history = OpenPlayHistoryFile(*archive, create);
    if (!play_history) {
        return MakeEmptyPlayHistoryData();
    }

    PlayHistoryData data;
    data.entries.resize(PLAY_HISTORY_MAX_ENTRIES);

    PlayHistoryHeader header{};
    play_history->Read(0, sizeof(header), reinterpret_cast<u8*>(&header));
    data.start_index = header.start_index;
    data.total_entries = header.total_entries;
    play_history->Read(PLAY_HISTORY_ENTRIES_OFFSET, data.entries.size() * sizeof(PlayHistoryEntry),
                       reinterpret_cast<u8*>(data.entries.data()));
    play_history->Close();

    bool migrated_swapped_title_words = false;
    const u32 entries_to_check = std::min(data.total_entries, PLAY_HISTORY_MAX_ENTRIES);
    for (u32 i = 0; i < entries_to_check; ++i) {
        auto& entry = data.entries[i];
        if (!IsKnownTitleIdHigh(entry.title_id_high) &&
            IsKnownTitleIdHigh(static_cast<u32>(entry.title_id_low))) {
            std::swap(entry.title_id_high, entry.title_id_low);
            migrated_swapped_title_words = true;
        }
    }

    const bool zero_filled_empty_file =
        data.total_entries == 0 && !data.entries.empty() && data.entries.front().title_id_high == 0 &&
        data.entries.front().title_id_low == 0 && data.entries.front().info_timestamp == 0;

    if (!IsValidPlayHistoryData(data) || zero_filled_empty_file) {
        data = MakeEmptyPlayHistoryData();
        if (create) {
            WritePlayHistoryData(data);
        }
    } else if (migrated_swapped_title_words && create) {
        WritePlayHistoryData(data);
    }

    return data;
}

static bool IsLikelyTitleId(u64 title_id) {
    const u32 high = static_cast<u32>(title_id >> 32);
    return IsKnownTitleIdHigh(high);
}

static u64 GetCurrentProcessTitleId(Core::System& system) {
    const auto process = system.Kernel().GetCurrentProcess();
    if (process && process->codeset) {
        return process->codeset->program_id;
    }
    return 0;
}

static u64 PickNotifyPlayEventTitleId(Core::System& system, const std::array<u32, 5>& params) {
    for (std::size_t i = 0; i + 1 < params.size(); ++i) {
        const u64 candidate = static_cast<u64>(params[i]) | (static_cast<u64>(params[i + 1]) << 32);
        if (candidate == PLAY_HISTORY_SPECIAL_TITLE_ID || IsLikelyTitleId(candidate)) {
            return candidate;
        }
    }
    return GetCurrentProcessTitleId(system);
}

static u32 PickNotifyPlayEventType(const std::array<u32, 5>& params) {
    for (u32 param : params) {
        if (param <= PLAY_HISTORY_EVENT_MASK) {
            return param;
        }
    }
    return 0;
}

void Module::Interface::RegisterAlarmClient(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void Module::Interface::GetAdapterState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ptm->battery_is_charging);

    LOG_DEBUG(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetShellState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ptm->shell_open);
}

void Module::Interface::GetBatteryLevel(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(ChargeLevels::CompletelyFull)); // Set to a completely full battery

    LOG_DEBUG(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetBatteryChargeState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ptm->battery_is_charging);

    LOG_DEBUG(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetPedometerState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ptm->pedometer_is_counting);

    LOG_DEBUG(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetStepHistory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    u32 hours = rp.Pop<u32>();
    u64 start_time = rp.Pop<u64>();
    auto& buffer = rp.PopMappedBuffer();
    ASSERT_MSG(sizeof(u16) * hours == buffer.GetSize(),
               "Buffer for steps count has incorrect size");

    const u16_le steps_per_hour = Settings::values.steps_per_hour.GetValue();
    for (u32 i = 0; i < hours; ++i) {
        buffer.Write(&steps_per_hour, i * sizeof(u16), sizeof(u16));
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(buffer);

    LOG_WARNING(Service_PTM, "(STUBBED) called, from time(raw): 0x{:x}, for {} hours", start_time,
                hours);
}

void Module::Interface::GetStepHistoryAll(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    const u32 hours = rp.Pop<u32>();
    const u32 start_time = rp.Pop<u32>();
    auto& steps_buffer = rp.PopMappedBuffer();
    auto& timestamps_buffer = rp.PopMappedBuffer();

    if (steps_buffer.GetSize() != 0) {
        std::vector<u8> steps(steps_buffer.GetSize());
        steps_buffer.Write(steps.data(), 0, steps.size());
    }
    if (timestamps_buffer.GetSize() != 0) {
        std::vector<u8> timestamps(timestamps_buffer.GetSize());
        timestamps_buffer.Write(timestamps.data(), 0, timestamps.size());
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 4);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(steps_buffer);
    rb.PushMappedBuffer(timestamps_buffer);

    LOG_DEBUG(Service_PTM,
              "(STUBBED) called, from time(raw): 0x{:x}, hours={}, steps_size={}, "
              "timestamps_size={}",
              start_time, hours, steps_buffer.GetSize(), timestamps_buffer.GetSize());
}

void Module::Interface::GetTotalStepCount(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u32>(0);

    LOG_WARNING(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetSoftwareClosedFlag(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(false);

    LOG_WARNING(Service_PTM, "(STUBBED) called");
}

void CheckNew3DS(IPC::RequestBuilder& rb) {
    const bool is_new_3ds = Settings::values.is_new_3ds.GetValue();

    rb.Push(ResultSuccess);
    rb.Push(is_new_3ds);

    LOG_DEBUG(Service_PTM, "called isNew3DS = 0x{:08x}", static_cast<u32>(is_new_3ds));
}

void Module::Interface::CheckNew3DS(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    Service::PTM::CheckNew3DS(rb);
}

void Module::Interface::SetInfoLEDPattern(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    auto pat = rp.PopRaw<MCU::InfoLedPattern>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    auto mcu_rtc = MCU::RTC::GetService(ptm->system);
    if (mcu_rtc) {
        mcu_rtc->UpdateInfoLEDPattern(pat);
        rb.Push(ResultSuccess);
    } else {
        rb.Push(ResultUnknown);
    }
}

void Module::Interface::SetInfoLEDPatternHeader(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    auto head = rp.PopRaw<MCU::InfoLedPattern::Header>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    auto mcu_rtc = MCU::RTC::GetService(ptm->system);
    if (mcu_rtc) {
        mcu_rtc->UpdateInfoLEDHeader(head);
        rb.Push(ResultSuccess);
    } else {
        rb.Push(ResultUnknown);
    }
}

void Module::Interface::GetInfoLEDStatus(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);

    auto mcu_rtc = MCU::RTC::GetService(ptm->system);
    if (mcu_rtc) {
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u8>(mcu_rtc->GetInfoLEDStatusFinished()));
    } else {
        rb.Push(ResultUnknown);
        rb.Push(u8{});
    }
}

void Module::Interface::GetSystemTime(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    auto& share_page = ptm->system.Kernel().GetSharedPageHandler();
    const u64 console_time = share_page.GetSystemTimeSince2000();

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 0);
    rb.Push(ResultSuccess);
    rb.Push(console_time);
}

void Module::Interface::GetPlayHistory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 entry_offset = rp.Pop<u32>();
    const u32 total_entries = rp.Pop<u32>();
    auto& buffer = rp.PopMappedBuffer();

    const auto entries = ptm->GetPlayHistoryEntries(entry_offset, total_entries);
    if (!entries.empty()) {
        std::vector<PlayHistoryIpcEntry> ipc_entries;
        ipc_entries.reserve(entries.size());
        for (const auto& entry : entries) {
            ipc_entries.push_back({
                .title_id_high = entry.title_id_high,
                .title_id_low = entry.title_id_low,
                .info_timestamp = entry.info_timestamp,
            });
        }

        const std::size_t bytes_to_write =
            std::min<std::size_t>(ipc_entries.size() * sizeof(PlayHistoryIpcEntry),
                                  buffer.GetSize());
        buffer.Write(ipc_entries.data(), 0, bytes_to_write);
    }

    const u32 end_index =
        (entry_offset + static_cast<u32>(entries.size())) % PLAY_HISTORY_MAX_ENTRIES;

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(ResultSuccess);
    rb.Push(end_index);
    rb.PushMappedBuffer(buffer);

    LOG_DEBUG(Service_PTM,
              "GetPlayHistory start_index={} requested={} returned={} end_index={} buffer_size={}",
              entry_offset, total_entries, entries.size(), end_index, buffer.GetSize());
}

void Module::Interface::GetPlayHistoryStart(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 start = ptm->GetPlayHistoryStart();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(start);

    LOG_DEBUG(Service_PTM, "GetPlayHistoryStart returned={}", start);
}

void Module::Interface::GetPlayHistoryLength(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 length = ptm->GetPlayHistoryLength();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(length);

    LOG_DEBUG(Service_PTM, "GetPlayHistoryLength returned={}", length);
}

void Module::Interface::ClearPlayHistory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    ptm->ClearPlayHistory();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void Module::Interface::CalcPlayHistoryStart(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 start = rp.Pop<u32>();
    const s32 entries = rp.Pop<s32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    const u32 result = ptm->CalcPlayHistoryStart(start, entries);
    rb.Push(result);

    LOG_DEBUG(Service_PTM, "CalcPlayHistoryStart start={} entries={} returned={}", start, entries,
              result);
}

void Module::Interface::NotifyPlayEvent(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const std::array<u32, 5> params{
        rp.Pop<u32>(), rp.Pop<u32>(), rp.Pop<u32>(), rp.Pop<u32>(), rp.Pop<u32>()};
    const u64 title_id = PickNotifyPlayEventTitleId(ptm->system, params);
    const u32 event_type = PickNotifyPlayEventType(params);

    if (title_id != 0) {
        ptm->NotifyPlayEvent(title_id, event_type);
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);

    LOG_DEBUG(Service_PTM,
              "NotifyPlayEvent title_id={:016X} event_type={} raw={:08X} {:08X} {:08X} "
              "{:08X} {:08X}",
              title_id, event_type, params[0], params[1], params[2], params[3], params[4]);
}

void Module::Interface::ClearSoftwareClosedFlag(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);

    LOG_DEBUG(Service_PTM, "(STUBBED) ClearSoftwareClosedFlag called");
}

static void WriteGameCoinData(GameCoin gamecoin_data) {
    const std::string& nand_directory = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    FileSys::ArchiveFactory_ExtSaveData extdata_archive_factory(nand_directory,
                                                                FileSys::ExtSaveDataType::Shared);

    FileSys::Path archive_path(ptm_shared_extdata_id);
    auto archive_result = extdata_archive_factory.Open(archive_path, 0);
    std::unique_ptr<FileSys::ArchiveBackend> archive;

    FileSys::Path gamecoin_path("/gamecoin.dat");
    // If the archive didn't exist, create the files inside
    if (archive_result.Code() == FileSys::ResultNotFormatted) {
        // Format the archive to create the directories
        extdata_archive_factory.FormatAsExtData(archive_path, FileSys::ArchiveFormatInfo(), 0, 0, 0,
                                                std::nullopt);
        // Open it again to get a valid archive now that the folder exists
        archive = extdata_archive_factory.Open(archive_path, 0).Unwrap();
        // Create the game coin file
        archive->CreateFile(gamecoin_path, sizeof(GameCoin));
    } else {
        ASSERT_MSG(archive_result.Succeeded(), "Could not open the PTM SharedExtSaveData archive!");
        archive = std::move(archive_result).Unwrap();
    }

    FileSys::Mode open_mode = {};
    open_mode.write_flag.Assign(1);
    // Open the file and write the default gamecoin information
    auto gamecoin_result = archive->OpenFile(gamecoin_path, open_mode);
    if (gamecoin_result.Succeeded()) {
        auto gamecoin = std::move(gamecoin_result).Unwrap();
        gamecoin->Write(0, sizeof(GameCoin), true, false,
                        reinterpret_cast<const u8*>(&gamecoin_data));
        gamecoin->Close();
    }
}

static GameCoin ReadGameCoinData() {
    const std::string& nand_directory = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    FileSys::ArchiveFactory_ExtSaveData extdata_archive_factory(nand_directory,
                                                                FileSys::ExtSaveDataType::Shared);

    FileSys::Path archive_path(ptm_shared_extdata_id);
    auto archive_result = extdata_archive_factory.Open(archive_path, 0);
    if (!archive_result.Succeeded()) {
        LOG_ERROR(Service_PTM, "Could not open the PTM SharedExtSaveData archive!");
        return default_game_coin;
    }

    FileSys::Path gamecoin_path("/gamecoin.dat");
    FileSys::Mode open_mode = {};
    open_mode.read_flag.Assign(1);

    auto gamecoin_result = (*archive_result)->OpenFile(gamecoin_path, open_mode);
    if (!gamecoin_result.Succeeded()) {
        LOG_ERROR(Service_PTM, "Could not open the game coin data file!");
        return default_game_coin;
    }

    auto gamecoin = std::move(gamecoin_result).Unwrap();
    GameCoin gamecoin_data;
    gamecoin->Read(0, sizeof(GameCoin), reinterpret_cast<u8*>(&gamecoin_data));
    gamecoin->Close();
    return gamecoin_data;
}

Module::Module(Core::System& system_) : system(system_) {
    // Open the SharedExtSaveData archive 0xF000000B and create the gamecoin.dat file if it doesn't
    // exist
    const std::string& nand_directory = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    FileSys::ArchiveFactory_ExtSaveData extdata_archive_factory(nand_directory,
                                                                FileSys::ExtSaveDataType::Shared);
    const FileSys::Path archive_path(ptm_shared_extdata_id);
    const auto archive_result = extdata_archive_factory.Open(archive_path, 0);
    // If the archive didn't exist, write the default game coin file
    if (archive_result.Code() == FileSys::ResultNotFormatted) {
        WriteGameCoinData(default_game_coin);
    }

    EnsurePlayHistoryLoaded();
}

template <class Archive>
void Module::serialize(Archive& ar, const unsigned int) {
    DEBUG_SERIALIZATION_POINT;
    ar & shell_open;
    ar & battery_is_charging;
    ar & pedometer_is_counting;
}
SERIALIZE_IMPL(Module)

u16 Module::GetPlayCoins() {
    return ReadGameCoinData().total_coins;
}

void Module::SetPlayCoins(u16 play_coins) {
    GameCoin game_coin = ReadGameCoinData();
    game_coin.total_coins = play_coins;
    // TODO: This may introduce potential race condition if the game is reading the
    // game coin data at the same time
    WriteGameCoinData(game_coin);
}

void Module::RecordPlayEvent(u64 title_id, u32 event_type) {
    NotifyPlayEvent(title_id, event_type);
}

void Module::EnsurePlayHistoryLoaded() {
    if (play_history_loaded) {
        return;
    }

    PlayHistoryData data = ReadPlayHistoryData(true);
    play_history_start_index = data.start_index;
    play_history_total_entries = data.total_entries;
    play_history_entries = std::move(data.entries);
    play_history_loaded = true;
}

void Module::PersistPlayHistoryData() const {
    WritePlayHistoryData({
        .start_index = play_history_start_index,
        .total_entries = play_history_total_entries,
        .entries = play_history_entries,
    });
}

std::vector<PlayHistoryEntry> Module::GetPlayHistoryEntries(u32 start_index, u32 count) {
    EnsurePlayHistoryLoaded();
    std::vector<PlayHistoryEntry> entries;

    if (start_index >= PLAY_HISTORY_MAX_ENTRIES || play_history_total_entries == 0) {
        return entries;
    }

    const u32 logical_offset =
        (start_index + PLAY_HISTORY_MAX_ENTRIES - play_history_start_index) %
        PLAY_HISTORY_MAX_ENTRIES;
    if (logical_offset >= play_history_total_entries) {
        return entries;
    }

    const u32 entries_to_return = std::min(count, play_history_total_entries - logical_offset);
    entries.reserve(entries_to_return);
    for (u32 i = 0; i < entries_to_return; ++i) {
        const u32 physical_index = (start_index + i) % PLAY_HISTORY_MAX_ENTRIES;
        entries.push_back(play_history_entries[physical_index]);
    }
    return entries;
}

u32 Module::GetPlayHistoryStart() {
    EnsurePlayHistoryLoaded();
    return play_history_start_index;
}

u32 Module::GetPlayHistoryLength() {
    EnsurePlayHistoryLoaded();
    return play_history_total_entries;
}

u32 Module::CalcPlayHistoryStart(u32 start, s32 entries) {
    const s64 result = static_cast<s64>(start) + entries;
    const s64 wrapped = result % PLAY_HISTORY_MAX_ENTRIES;
    return static_cast<u32>(wrapped < 0 ? wrapped + PLAY_HISTORY_MAX_ENTRIES : wrapped);
}

void Module::ClearPlayHistory() {
    const PlayHistoryData data = MakeEmptyPlayHistoryData();
    play_history_start_index = data.start_index;
    play_history_total_entries = data.total_entries;
    play_history_entries = data.entries;
    play_history_loaded = true;
    PersistPlayHistoryData();
}

void Module::NotifyPlayEvent(u64 title_id, u32 event_type) {
    EnsurePlayHistoryLoaded();
    if (play_history_entries.size() != PLAY_HISTORY_MAX_ENTRIES) {
        const PlayHistoryData data = MakeEmptyPlayHistoryData();
        play_history_start_index = data.start_index;
        play_history_total_entries = data.total_entries;
        play_history_entries = data.entries;
    }

    const u32 write_index = play_history_total_entries < PLAY_HISTORY_MAX_ENTRIES
                                ? (play_history_start_index + play_history_total_entries) %
                                      PLAY_HISTORY_MAX_ENTRIES
                                : play_history_start_index;
    if (play_history_total_entries < PLAY_HISTORY_MAX_ENTRIES) {
        ++play_history_total_entries;
    } else {
        play_history_start_index = (play_history_start_index + 1) % PLAY_HISTORY_MAX_ENTRIES;
    }

    play_history_entries[write_index] = PlayHistoryEntry{
        .title_id_high = static_cast<u32>(title_id >> 32),
        .title_id_low = static_cast<u32>(title_id),
        .info_timestamp =
            static_cast<u32>((GetPlayHistoryTimestamp(system) << 4) | (event_type & 0xF)),
    };
    PersistPlayHistoryData();
}

Module::Interface::Interface(std::shared_ptr<Module> ptm, const char* name, u32 max_session)
    : ServiceFramework(name, max_session), ptm(std::move(ptm)) {}

std::shared_ptr<Module> Module::Interface::GetModule() const {
    return ptm;
}

std::shared_ptr<Module> GetModule(Core::System& system) {
    auto ptm = system.ServiceManager().GetService<Module::Interface>("ptm:play");
    if (!ptm) {
        return nullptr;
    }
    return ptm->GetModule();
}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    auto ptm = std::make_shared<Module>(system);
    std::make_shared<PTM_Gets>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_Play>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_Sets>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_S>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_Sysm>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_U>(ptm)->InstallAsService(service_manager);
}

} // namespace Service::PTM
