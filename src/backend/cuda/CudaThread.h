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

#ifndef XMRIG_CUDATHREAD_H
#define XMRIG_CUDATHREAD_H


using nvid_ctx = struct nvid_ctx;


#include "3rdparty/rapidjson/fwd.h"


namespace xmrig {


class CudaThread
{
public:
    CudaThread() = delete;
    CudaThread(const rapidjson::Value &value);
    CudaThread(uint32_t index, nvid_ctx *ctx);

    inline bool isValid() const                              { return m_blocks > 0 && m_threads > 0; }
    inline int32_t bfactor() const                           { return static_cast<int32_t>(m_bfactor); }
    inline int32_t blocks() const                            { return m_blocks; }
    inline int32_t bsleep() const                            { return static_cast<int32_t>(m_bsleep); }
    inline int32_t datasetHost() const                       { return m_datasetHost; }
    inline int32_t threads() const                           { return m_threads; }
    inline int64_t affinity() const                          { return m_affinity; }
    inline uint32_t index() const                            { return m_index; }

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Per-GPU overrides. has_*() returns whether the user pinned the value
    // on this device; the unpinned default is resolved in CudaConfig::get()
    // against CudaConfig's global field.
    inline bool hasLookupGap() const         { return m_has_lookup_gap; }
    inline bool hasUseSystemRam() const      { return m_has_use_system_ram; }
    inline bool hasReserveVramMb() const     { return m_has_reserve_vram_mb; }
    inline bool hasHostRamBudgetMb() const   { return m_has_host_ram_budget_mb; }
    inline int  lookupGap() const            { return m_lookup_gap; }
    inline bool useSystemRam() const         { return m_use_system_ram; }
    inline int  reserveVramMb() const        { return m_reserve_vram_mb; }
    inline int  hostRamBudgetMb() const      { return m_host_ram_budget_mb; }

    // Host-mapped (system-RAM) warp count the plugin autotune estimated for this
    // launch, read back from the autotune ctx. Lets the backend report the VRAM /
    // RAM scratchpad split without duplicating the plugin's reserve math. 0 for a
    // VRAM-only launch or a JSON-pinned thread (no autotune read-back).
    inline uint32_t scryptChachaRamWarps() const { return m_scryptChachaRamWarps; }
#   endif

    inline bool operator!=(const CudaThread &other) const    { return !isEqual(other); }
    inline bool operator==(const CudaThread &other) const    { return isEqual(other); }

    bool isEqual(const CudaThread &other) const;
    rapidjson::Value toJSON(rapidjson::Document &doc) const;

private:
    int32_t m_blocks        = 0;
    int32_t m_datasetHost   = -1;
    int32_t m_threads       = 0;
    int64_t m_affinity      = -1;
    uint32_t m_index        = 0;

#   ifdef _WIN32
    uint32_t m_bfactor      = 6;
    uint32_t m_bsleep       = 25;
#   else
    uint32_t m_bfactor      = 0;
    uint32_t m_bsleep       = 0;
#   endif

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    bool m_has_lookup_gap         = false;
    bool m_has_use_system_ram     = false;
    bool m_has_reserve_vram_mb    = false;
    bool m_has_host_ram_budget_mb = false;
    int  m_lookup_gap         = 0;
    bool m_use_system_ram     = false;
    int  m_reserve_vram_mb    = 0;
    int  m_host_ram_budget_mb = 0;
    uint32_t m_scryptChachaRamWarps = 0;
#   endif
};


} /* namespace xmrig */


#endif /* XMRIG_CUDATHREAD_H */
