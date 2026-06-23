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

#include "crypto/scrypt-chacha/Autotune.h"
#include "backend/cpu/interfaces/ICpuInfo.h"
#include "base/crypto/Algorithm.h"
#include "base/io/log/Log.h"
#include "base/tools/HostMemoryInfo.h"
#include "crypto/scrypt-chacha/scrypt-chacha.h"

#include <algorithm>

#ifdef XMRIG_OS_LINUX
#   include <fstream>
#   include <string>
#endif


namespace xmrig { namespace scrypt_chacha {


#ifdef XMRIG_OS_LINUX
static size_t hugepages_budget()
{
    static constexpr size_t kHugePageBytes  = 2ULL * 1024 * 1024;
    static constexpr size_t kPagesPerWorker = kScratchpadBytes / kHugePageBytes;  // 256 (= 512 MiB / 2 MiB)

    auto read_value = [](const char *path) -> size_t {
        std::ifstream f(path);
        size_t v = 0;
        return (f >> v) ? v : 0;
    };

    /* Prefer per-NUMA-node sysfs path if present (mirrors what
     * LinuxMemory::reserve() writes to); fall back to the system-wide knob.
     * Both report pool size in 2 MiB pages -- not free pages -- which is
     * what we want for a deterministic autotune log line. */
    size_t total = read_value("/sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages");
    if (total == 0) {
        total = read_value("/proc/sys/vm/nr_hugepages");
    }

    return total / kPagesPerWorker;
}
#endif


CpuThreads autotune(const Algorithm & /*algo*/, const ICpuInfo *info,
                    uint32_t limit, bool hugePages, uint32_t reserveMb)
{
    LOG_DEBUG("scrypt-chacha CPU autotune: entry limit=%u hugePages=%d reserveMb=%u"
              " info->cores=%zu info->threads=%zu",
              limit, hugePages ? 1 : 0, reserveMb,
              info->cores(), info->threads());

    if (limit == 0) {
        limit = 100;
    }

    // 1) CPU budget -- one thread per physical core, scaled by limit. When
    //    hwloc isn't compiled in, info->cores() returns 0; fall back to
    //    half of logical threads (HT siblings starve memory bandwidth).
    size_t cores = info->cores();
    if (cores == 0) {
        cores = std::max<size_t>(info->threads() / 2, 1);
        LOG_DEBUG("scrypt-chacha CPU autotune: info->cores() returned 0,"
                  " fallback cores=%zu (info->threads/2)", cores);
    }
    const size_t cpu_budget = std::max<size_t>(cores * limit / 100, 1);
    LOG_INFO("scrypt-chacha CPU autotune: cpu_budget=%zu (cores=%zu × limit=%u%%)",
                cpu_budget, cores, limit);

    // 2) Memory budget -- leave reserveMb free for the OS + other processes.
    const uint64_t reserve_bytes = static_cast<uint64_t>(reserveMb) * 1024ULL * 1024ULL;
    const uint64_t avail_ram     = readProcMemInfoAvailable();
    const size_t   mem_budget    = (avail_ram > reserve_bytes)
        ? static_cast<size_t>((avail_ram - reserve_bytes) / kScratchpadBytes)
        : 0;

    LOG_INFO("scrypt-chacha CPU autotune: mem_budget=%zu"
                " (avail=%.2f GiB - reserve=%.2f GiB ÷ scratchpad=%.0f MiB)",
                mem_budget,
                avail_ram / (1024.0 * 1024.0 * 1024.0),
                reserve_bytes / (1024.0 * 1024.0 * 1024.0),
                kScratchpadBytes / (1024.0 * 1024.0));

    if (mem_budget == 0) {
        LOG_WARN("scrypt-chacha: only %.1f GiB available, need >%.1f GiB for one thread",
                 avail_ram / (1024.0 * 1024.0 * 1024.0),
                 (reserve_bytes + kScratchpadBytes) / (1024.0 * 1024.0 * 1024.0));
        LOG_DEBUG("scrypt-chacha CPU autotune: returning empty CpuThreads (no memory)");
        return {};
    }

    // 3) Final thread count -- depends only on CPU and memory budgets.
    const size_t threads = std::min(cpu_budget, mem_budget);
    LOG_DEBUG("scrypt-chacha CPU autotune: threads=min(cpu_budget=%zu, mem_budget=%zu)=%zu",
              cpu_budget, mem_budget, threads);

    // 4) Huge-pages budget -- INFORMATIONAL ONLY. Tells the user how the
    //    workers will split between explicit huge pages and the
    //    _mm_malloc + MADV_HUGEPAGE fallback inside VirtualMemory. Does
    //    not cap `threads`.
#   ifdef XMRIG_OS_LINUX
    const size_t hp_budget = hugePages ? hugepages_budget() : 0;
#   else
    const size_t hp_budget = 0;
#   endif

    const size_t hp_workers   = std::min(hp_budget, threads);
    const size_t heap_workers = threads - hp_workers;

    LOG_DEBUG("scrypt-chacha CPU autotune: hp_budget=%zu hp_workers=%zu heap_workers=%zu",
              hp_budget, hp_workers, heap_workers);

    LOG_INFO("scrypt-chacha CPU autotune: %zu threads"
             " (cpu_budget=%zu, mem_budget=%zu, hp_budget=%zu;"
             " avail_ram=%.1f GiB, reserve=%u MB)",
             threads, cpu_budget, mem_budget, hp_budget,
             avail_ram / (1024.0 * 1024.0 * 1024.0), reserveMb);

    if (hugePages) {
        LOG_INFO("scrypt-chacha CPU autotune: %zu worker(s) will use explicit huge pages,"
                 " %zu worker(s) will use heap with MADV_HUGEPAGE (transparent huge pages)",
                 hp_workers, heap_workers);
    }

    LOG_DEBUG("scrypt-chacha CPU autotune: exit threads=%zu intensity=1", threads);

    return CpuThreads(threads, 1);
}


}} /* namespace xmrig::scrypt_chacha */
