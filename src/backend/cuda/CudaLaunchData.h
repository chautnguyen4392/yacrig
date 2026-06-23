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

#ifndef XMRIG_CUDALAUNCHDATA_H
#define XMRIG_CUDALAUNCHDATA_H


#include "backend/cuda/CudaThread.h"
#include "base/crypto/Algorithm.h"
#include "crypto/common/Nonce.h"


namespace xmrig {


class CudaDevice;
class Miner;


class CudaLaunchData
{
public:
    CudaLaunchData(const Miner *miner, const Algorithm &algorithm, const CudaThread &thread, const CudaDevice &device);

    bool isEqual(const CudaLaunchData &other) const;

    inline constexpr static Nonce::Backend backend() { return Nonce::CUDA; }

    inline bool operator!=(const CudaLaunchData &other) const    { return !isEqual(other); }
    inline bool operator==(const CudaLaunchData &other) const    { return isEqual(other); }

    static const char *tag();

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // CUDA threads cooperating on one scrypt-chacha work unit: 1 on the
    // Volta-family kernel (compute capability >= 7), 4 on Pascal. Single source
    // for the cc >= 7 ? 1 : 4 split shared by the launch-config log and the
    // runner's work-unit read-back.
    uint32_t scryptChachaThreadsPerWU() const;

    // The launch's theoretical work-unit count: threads * blocks divided by the
    // per-WU cooperation factor above. Matches what scryptchacha_autotune
    // targets before deviceInit's VRAM back-off.
    size_t scryptChachaTheoreticalWorkUnits() const;

    // Scratchpad split (bytes) for a launch of `workUnits` work units of which
    // `ramWarps` warps are host-mapped: VRAM holds the rest. The split is decided
    // by the plugin (its host-RAM-adaptive reserve), so the caller passes the
    // plugin's warp count rather than re-deriving it: the startup table passes
    // the autotune estimate (CudaThread::scryptChachaRamWarps), the runner passes
    // the actual post-deviceInit count read back from its ctx. Only the stable
    // per-warp scratchpad size is computed here.
    void scryptChachaMemorySplit(size_t ramWarps, size_t workUnits, size_t &vramBytes, size_t &ramBytes, size_t &totalBytes) const;
#   endif

    const Algorithm algorithm;
    const CudaDevice &device;
    const CudaThread thread;
    const int64_t affinity;
    const Miner *miner;
    const uint32_t benchSize = 0;

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Effective per-GPU tuning. Resolved at CudaConfig::get time as
    // CudaThread.field.value_or(CudaConfig.field).
    int  scryptchacha_lookup_gap         = 64;
    bool scryptchacha_use_system_ram     = false;
    int  scryptchacha_reserve_vram_mb    = 0;
    int  scryptchacha_host_ram_budget_mb = 0;
#   endif
};


} // namespace xmrig


#endif /* XMRIG_OCLLAUNCHDATA_H */
