/* XMRig
 * Copyright (c) 2018-2021 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2021 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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


#include "backend/opencl/generators/ocl_scrypt_chacha_generator.h"
#include "backend/common/Tags.h"
#include "backend/opencl/OclThreads.h"
#include "backend/opencl/wrappers/OclDevice.h"
#include "base/crypto/Algorithm.h"
#include "base/io/log/Log.h"
#include "crypto/scrypt-chacha/scrypt-chacha.h"


#include <algorithm>
#include <cinttypes>


namespace xmrig {


// Memory-aware autotune, ported from YACMiner's initCl (ocl.c). The generator
// is the single source of truth for intensity: it sizes the launch from the
// card's memory once, at config time, and the runner consumes the stored value
// without recomputing it. The runner turns the intensity into a flat table of
// scratchpad chunks (host RAM first, then VRAM) and backs off if the
// allocations come up short, so an optimistic figure here degrades gracefully
// instead of failing the thread.
bool ocl_scrypt_chacha_generator(const OclDevice &device, const Algorithm &algorithm, OclThreads &threads, const OclScryptChachaTuning &tuning)
{
    if (algorithm.family() != Algorithm::SCRYPT_CHACHA) {
        return false;
    }

    // The tuning struct carries the yacrig defaults (lookup_gap 32, worksize
    // 32); the global --opencl-* knobs and "opencl" JSON fields replace them.
    // Clamped to the same ranges the launch-data merge enforces.
    const uint32_t worksize  = std::max(std::min(tuning.worksize, 512u), 1u);
    const uint32_t lookupGap = std::max(tuning.lookupGap, 1u);

    // Scratchpad per work-item: ceil(N / lookup_gap) checkpoints of 128 bytes
    // (8 uint4), which is 16 MiB per work-item at N = 2^22, lookup_gap 32. One
    // work-group of 32 items therefore needs 512 MiB.
    constexpr uint64_t oneMiB = 1024 * 1024;

    const uint64_t perThreadBytes = scrypt_chacha::perWorkUnitScratchpadBytes(lookupGap);
    const uint64_t groupBytes     = perThreadBytes * worksize;

    // VRAM budget, matching the donor's probe order. The AMD extension reports
    // memory that is currently free (display, other clients and the OS
    // compositor already subtracted), so it needs no safety margin. The
    // fallback only knows total VRAM, so it reserves the donor's 200 MB margin
    // for everything else living on the card. reserve_vram_mb comes off the
    // budget either way, mirroring the donor's --reserve-vram.
    constexpr uint64_t fallbackReserve = 200ull * oneMiB;

    uint64_t budget = device.freeMemSizeAmd();
    if (budget == 0) {
        const uint64_t total = device.globalMemSize();
        budget = total > fallbackReserve ? total - fallbackReserve : 0;
    }

    const uint64_t reserveBytes = static_cast<uint64_t>(tuning.reserveVramMb) * oneMiB;
    budget = budget > reserveBytes ? budget - reserveBytes : 0;

    // System RAM extends the group count past the VRAM ceiling. The budget is
    // this GPU's absolute share (validated against MemAvailable by
    // OclConfig::setupScryptChacha), and the runner spills exactly the groups
    // that do not fit in VRAM into host chunks.
    const uint64_t vramGroups = budget / groupBytes;
    const uint64_t hostGroups = tuning.useSystemRam
        ? static_cast<uint64_t>(tuning.hostRamBudgetMb) * oneMiB / groupBytes
        : 0;

    const uint64_t numberGroups = vramGroups + hostGroups;
    const uint32_t intensity    = static_cast<uint32_t>(numberGroups * worksize);

    if (intensity == 0) {
        LOG_WARN("%s" YELLOW(" scrypt-chacha: GPU #%u has less than one work-group (") YELLOW_BOLD("%" PRIu64 " MiB") YELLOW(") of free memory, skipping this device"),
                 ocl_tag(), device.index(), groupBytes / oneMiB);

        return true;
    }

    threads.add(OclThread(device.index(), intensity, worksize));

    return true;
}


// Dispatch-table entry (the generators[] table in OclDevice.cpp): the tuning
// struct's defaults, no config access. OclConfig::generate() does not go
// through this, it calls the tuned overload with the resolved global knobs.
bool ocl_scrypt_chacha_generator(const OclDevice &device, const Algorithm &algorithm, OclThreads &threads)
{
    return ocl_scrypt_chacha_generator(device, algorithm, threads, OclScryptChachaTuning());
}


} // namespace xmrig
