/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2020-2025 Raspberry Pi Ltd
 *
 * FreeBSD drive enumeration.
 */

#include "drivelist.h"
#include "embedded_config.h"

namespace Drivelist {

// ============================================================================
// Public API
// ============================================================================

std::vector<DeviceDescriptor> ListStorageDevices()
{
    std::vector<DeviceDescriptor> deviceList;
    return deviceList;
}

// ============================================================================
// Test API
// ============================================================================

#ifdef DRIVELIST_ENABLE_TEST_API

namespace testing {

std::vector<DeviceDescriptor> parseFreeBSDBlockDevices(const std::string& jsonOutput, bool embeddedMode)
{
    std::vector<DeviceDescriptor> deviceList;
    return deviceList;
}

} // namespace testing

#endif // DRIVELIST_ENABLE_TEST_API

} // namespace Drivelist
