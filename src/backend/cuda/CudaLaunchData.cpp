/* XMRig
 * Copyright (c) 2018-2020 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2020 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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


#include "backend/cuda/CudaLaunchData.h"
#include "backend/cuda/wrappers/CudaDevice.h"
#include "backend/common/Tags.h"

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
#   include "crypto/scrypt-chacha/scrypt-chacha.h"
#   include <algorithm>
#endif


xmrig::CudaLaunchData::CudaLaunchData(const Miner *miner, const Algorithm &algorithm, const CudaThread &thread, const CudaDevice &device) :
    algorithm(algorithm),
    device(device),
    thread(thread),
    affinity(thread.affinity()),
    miner(miner)
{
}


bool xmrig::CudaLaunchData::isEqual(const CudaLaunchData &other) const
{
    return (other.algorithm.family() == algorithm.family() &&
            other.algorithm.l3()     == algorithm.l3() &&
            other.thread             == thread
#           ifdef XMRIG_ALGO_SCRYPT_CHACHA
            // The merged per-GPU tuning is part of the launch identity, like
            // on the OpenCL launch data: a global-knob change that shifts a
            // merged value must make setJob recreate the workers even though
            // the thread itself (which stores only pins) is unchanged.
            && other.scryptchacha_lookup_gap         == scryptchacha_lookup_gap
            && other.scryptchacha_use_system_ram     == scryptchacha_use_system_ram
            && other.scryptchacha_reserve_vram_mb    == scryptchacha_reserve_vram_mb
            && other.scryptchacha_host_ram_budget_mb == scryptchacha_host_ram_budget_mb
#           endif
            );
}


const char *xmrig::CudaLaunchData::tag()
{
    return cuda_tag();
}


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
uint32_t xmrig::CudaLaunchData::scryptChachaThreadsPerWU() const
{
    return device.computeCapability(true) >= 7 ? 1 : 4;
}


size_t xmrig::CudaLaunchData::scryptChachaTheoreticalWorkUnits() const
{
    return static_cast<size_t>(thread.threads()) * thread.blocks() / scryptChachaThreadsPerWU();
}


void xmrig::CudaLaunchData::scryptChachaMemorySplit(size_t ramWarps, size_t workUnits, size_t &vramBytes, size_t &ramBytes, size_t &totalBytes) const
{
    const int    lookup_gap  = scryptchacha_lookup_gap > 0 ? scryptchacha_lookup_gap : 1;
    const size_t wu_per_warp = 32 / scryptChachaThreadsPerWU();                       // 32 on Volta, 8 on Pascal
    const size_t per_warp    = wu_per_warp * scrypt_chacha::perWorkUnitScratchpadBytes(static_cast<uint32_t>(lookup_gap));
    const size_t total_warps = wu_per_warp ? workUnits / wu_per_warp : 0;

    // The host-mapped warp count is the plugin's figure (the host-RAM-adaptive
    // reserve that decides it lives only in the plugin). Only the per-warp
    // scratchpad size, a stable algorithm constant, is computed here.
    const size_t ram_warps  = std::min(ramWarps, total_warps);
    const size_t vram_warps = total_warps - ram_warps;

    vramBytes  = vram_warps  * per_warp;
    ramBytes   = ram_warps   * per_warp;
    totalBytes = total_warps * per_warp;
}
#endif
