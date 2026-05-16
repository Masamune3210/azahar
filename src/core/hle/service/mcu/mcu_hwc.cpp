// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <vector>
#include "common/settings.h"

#include "common/archives.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/service/mcu/mcu_hwc.h"
#include "core/hle/service/mcu/mcu_rtc.h"

SERVICE_CONSTRUCT_IMPL(Service::MCU::HWC)
SERIALIZE_EXPORT_IMPL(Service::MCU::HWC)

namespace Service::MCU {

namespace {

constexpr u8 BatteryTemperatureCelsius = 28;
constexpr u8 Old3dsMcuFirmwareVersionHigh = 0x12;
constexpr u8 Old3dsMcuFirmwareVersionLow = 37;
constexpr u8 New3dsMcuFirmwareVersionHigh = 0x13;
constexpr u8 New3dsMcuFirmwareVersionLow = 56;
constexpr std::array<u8, 0x13> SystemStateInfo = {
    0x00, // Console info
    0x04, // PMIC vendor code
    0x05, // Battery vendor code
    0x00, // MGIC version major
    0x00, // MGIC version minor
    0x00, // RCOMP
    BatteryTemperatureCelsius,
    0x00, // Unknown
    0x00, // Unknown
    0x00, // System model
    0x00, // Red power LED mode
    0x00, // Blue power LED intensity
    0x00, // Unknown
    0x00, // RGB LED red intensity
    0x00, // RGB LED green intensity
    0x00, // RGB LED blue intensity
    0x00, // Unknown
    0x00, // WiFi LED brightness
    0x00, // Raw button state
};

std::vector<u8> GetRegisterData(u8 reg, u8 size) {
    std::vector<u8> data(size);

    switch (reg) {
    case 0x0A:
        if (!data.empty()) {
            data[0] = BatteryTemperatureCelsius;
        }
        break;
    case 0x7F:
        std::copy_n(SystemStateInfo.begin(), std::min<std::size_t>(data.size(), SystemStateInfo.size()),
                    data.begin());
        break;
    default:
        break;
    }

    return data;
}

} // namespace

HWC::HWC(Core::System& _system) : ServiceFramework("mcu::HWC", 1), system(_system) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &HWC::ReadRegister, "ReadRegister"},
        {0x0002, nullptr, "WriteRegister"},
        {0x0003, &HWC::GetInfoRegisters, "GetInfoRegisters"},
        {0x0004, &HWC::GetBatteryVoltage, "GetBatteryVoltage"},
        {0x0005, &HWC::GetBatteryLevel, "GetBatteryLevel"},
        {0x0006, nullptr, "SetPowerLEDPattern"},
        {0x0007, nullptr, "SetWifiLEDState"},
        {0x0008, nullptr, "SetCameraLEDPattern"},
        {0x0009, nullptr, "Set3DLEDState"},
        {0x000A, &HWC::SetInfoLEDPattern, "SetInfoLEDPattern"},
        {0x000B, nullptr, "GetSoundVolume"},
        {0x000C, nullptr, "SetTopScreenFlicker"},
        {0x000D, nullptr, "SetBottomScreenFlicker"},
        {0x000E, &HWC::GetBatteryTemperature, "GetBatteryTemperature"},
        {0x000F, nullptr, "GetRtcTime"},
        {0x0010, &HWC::GetMcuFwVerHigh, "GetMcuFwVerHigh"},
        {0x0011, &HWC::GetMcuFwVerLow, "GetMcuFwVerLow"},
        // clang-format on
    };
    RegisterHandlers(functions);
}

void HWC::ReadRegister(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u8 reg = rp.Pop<u8>();
    const u8 size = rp.Pop<u8>();
    auto& output = rp.PopMappedBuffer();

    const auto data = GetRegisterData(reg, size);
    output.Write(data.data(), 0, std::min(data.size(), output.GetSize()));

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(output);
}

void HWC::GetInfoRegisters(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] const u32 size = rp.Pop<u32>();
    auto& output = rp.PopMappedBuffer();

    const auto write_size = std::min({SystemStateInfo.size(), output.GetSize(),
                                      static_cast<std::size_t>(size)});
    output.Write(SystemStateInfo.data(), 0, write_size);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(ResultSuccess);
    rb.PushMappedBuffer(output);
}

void HWC::GetBatteryVoltage(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u8>(0xD7);
}

void HWC::GetBatteryLevel(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u8>(100);
}

void HWC::GetBatteryTemperature(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] const u32 unknown_1 = rp.Pop<u32>();
    [[maybe_unused]] const u32 unknown_2 = rp.Pop<u32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u8>(BatteryTemperatureCelsius);
}

void HWC::GetMcuFwVerHigh(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u8>(Settings::values.is_new_3ds.GetValue() ? New3dsMcuFirmwareVersionHigh
                                                        : Old3dsMcuFirmwareVersionHigh);
}

void HWC::GetMcuFwVerLow(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u8>(Settings::values.is_new_3ds.GetValue() ? New3dsMcuFirmwareVersionLow
                                                        : Old3dsMcuFirmwareVersionLow);
}

void HWC::SetInfoLEDPattern(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    auto pat = rp.PopRaw<MCU::InfoLedPattern>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    auto mcu_rtc = MCU::RTC::GetService(system);
    if (mcu_rtc) {
        mcu_rtc->UpdateInfoLEDPattern(pat);
        rb.Push(ResultSuccess);
    } else {
        rb.Push(ResultUnknown);
    }
}

} // namespace Service::MCU
