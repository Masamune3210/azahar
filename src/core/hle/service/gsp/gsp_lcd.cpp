// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/service/gsp/gsp_lcd.h"

SERIALIZE_EXPORT_IMPL(Service::GSP::GSP_LCD)

namespace Service::GSP {

GSP_LCD::GSP_LCD() : ServiceFramework("gsp::Lcd") {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x000A, nullptr, "SetBrightnessRaw"},
        {0x000B, nullptr, "SetBrightness"},
        {0x000F, nullptr, "PowerOnAllBacklights"},
        {0x0010, nullptr, "PowerOffAllBacklights"},
        {0x0011, nullptr, "PowerOnBacklight"},
        {0x0012, nullptr, "PowerOffBacklight"},
        {0x0013, nullptr, "SetLedForceOff"},
        {0x0014, &GSP_LCD::GetVendor, "GetVendor"},
        {0x0015, nullptr, "GetBrightness"},
        // clang-format on
    };
    RegisterHandlers(functions);
};

void GSP_LCD::GetVendor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push<u8>(0xCC); // SHARP/TN for both screens.
}

} // namespace Service::GSP
