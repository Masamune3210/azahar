// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hle/service/service.h"

namespace Service::MCU {

class NWM final : public ServiceFramework<NWM> {
public:
    NWM();

private:
    void SetWirelessLedState(Kernel::HLERequestContext& ctx);
    void GetWirelessLedState(Kernel::HLERequestContext& ctx);

    SERVICE_SERIALIZATION_SIMPLE
};

} // namespace Service::MCU

BOOST_CLASS_EXPORT_KEY(Service::MCU::NWM)
