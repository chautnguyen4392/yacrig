/* XMRig
 * Copyright (c) 2016-2026 XMRig <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 */

#include "base/tools/HostMemoryInfo.h"


#include <uv.h>

#ifdef XMRIG_OS_LINUX
#   include <fstream>
#   include <string>
#endif


uint64_t xmrig::readProcMemInfoAvailable()
{
#   ifdef XMRIG_OS_LINUX
    std::ifstream f("/proc/meminfo");
    std::string key;
    uint64_t value_kb = 0;
    std::string unit;
    while (f >> key >> value_kb >> unit) {
        if (key == "MemAvailable:") {
            return value_kb * 1024ULL;
        }
    }
#   endif

    return uv_get_free_memory();
}


size_t xmrig::readProcMemInfoAvailableMB()
{
    return static_cast<size_t>(readProcMemInfoAvailable() / (1024ULL * 1024ULL));
}
