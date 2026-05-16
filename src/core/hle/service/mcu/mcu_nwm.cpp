// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/service/mcu/mcu_nwm.h"

SERIALIZE_EXPORT_IMPL(Service::MCU::NWM)

namespace Service::MCU {

NWM::NWM() : ServiceFramework("mcu::NWM", 1) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &NWM::SetWirelessLedState, "SetWirelessLedState"},
        {0x0002, &NWM::GetWirelessLedState, "GetWirelessLedState"},
        {0x0003, nullptr, "SetGPIO20State"},
        {0x0004, nullptr, "GetGPIO20State"},
        {0x0005, nullptr, "SetEnableWifiGpio"},
        {0x0006, nullptr, "GetEnableWifiGpio"},
        {0x0007, nullptr, "SetWirelessDisabledFlag"},
        {0x0008, nullptr, "GetWirelessDisabledFlag"},
        // clang-format on
    };
    RegisterHandlers(functions);
}

void NWM::SetWirelessLedState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] const u8 state = rp.Pop<u8>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void NWM::GetWirelessLedState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u8>(1);
}

} // namespace Service::MCU
