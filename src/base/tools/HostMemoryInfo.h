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

#ifndef XMRIG_HOSTMEMORYINFO_H
#define XMRIG_HOSTMEMORYINFO_H


#include <cstddef>
#include <cstdint>


namespace xmrig {


/*
 * Reads MemAvailable (Linux) / uv_get_free_memory() (Windows / macOS) and
 * returns the result in bytes. Used by every autotuner that has to answer
 * "can I allocate N bytes without thrashing?". A single source of truth so
 * the CPU autotuner, the CUDA autotuner, and a future OpenCL autotuner agree
 * on the available-memory probe.
 *
 * Linux: parses /proc/meminfo's MemAvailable: line (added in Linux 3.14,
 *        Mar 2014). The kernel computes it as roughly
 *          MemFree + reclaimable_page_cache + reclaimable_slab - low_watermark
 *        which is the right answer for "can I allocate N GiB without
 *        thrashing?".
 *
 * Windows: uv_get_free_memory() wraps GlobalMemoryStatusEx() and returns
 *          ullAvailPhys, semantically equivalent to MemAvailable.
 *
 * macOS: libuv 1.34 returns Mach's "free" page count, a known
 *        under-estimate but acceptable until xmrig bumps libuv to 1.45+
 *        which adds uv_get_available_memory().
 */
uint64_t readProcMemInfoAvailable();


/*
 * Same probe as readProcMemInfoAvailable(), result in mebibytes (MiB). The
 * MB-flavoured callers (CUDA host_ram_budget_mb, OpenCL plans) use this to
 * stay in MB-granularity end-to-end and avoid bytes / MiB conversions at
 * the call site.
 */
size_t readProcMemInfoAvailableMB();


} /* namespace xmrig */


#endif /* XMRIG_HOSTMEMORYINFO_H */
