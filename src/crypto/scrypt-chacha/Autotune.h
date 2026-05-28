/* XMRig
 * Copyright (c) 2016-2026 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XMRIG_SCRYPT_CHACHA_AUTOTUNE_H
#define XMRIG_SCRYPT_CHACHA_AUTOTUNE_H


#include "backend/cpu/CpuThreads.h"


namespace xmrig {

class Algorithm;
class ICpuInfo;

namespace scrypt_chacha {


/* Returns a CpuThreads describing one thread per chosen physical core,
 * intensity 1, default affinity (-1). Worker count is
 *   min( cpu_budget, mem_budget )
 * where
 *   cpu_budget = max(1, physical_cores * limit / 100)
 *   mem_budget = (available_ram - reserveMb MiB) / 512 MiB
 *
 * `available_ram` is `MemAvailable` from /proc/meminfo on Linux, falling
 * back to uv_get_free_memory() elsewhere. `hugePages` selects whether to
 * report a third, informational `hp_budget` (the explicit huge-pages pool
 * size in workers). `hp_budget` does NOT cap the worker count — workers
 * that don't get explicit huge pages fall back to _mm_malloc + MADV_HUGEPAGE
 * inside VirtualMemory.
 *
 * Returns an empty CpuThreads if the memory budget is 0 (caller decides
 * what to do — generate() will skip the family).
 */
CpuThreads autotune(const Algorithm &algo, const ICpuInfo *info,
                    uint32_t limit, bool hugePages, uint32_t reserveMb);


}} /* namespace xmrig::scrypt_chacha */


#endif /* XMRIG_SCRYPT_CHACHA_AUTOTUNE_H */
