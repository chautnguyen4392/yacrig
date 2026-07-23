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

#ifndef XMRIG_OCLTHREAD_H
#define XMRIG_OCLTHREAD_H


#include "3rdparty/rapidjson/fwd.h"


#include <bitset>
#include <vector>


namespace xmrig {


class OclThread
{
public:
    OclThread() = delete;
    OclThread(uint32_t index, uint32_t intensity, uint32_t worksize, uint32_t stridedIndex, uint32_t memChunk, uint32_t threads, uint32_t unrollFactor) :
        m_threads(threads, -1),
        m_index(index),
        m_memChunk(memChunk),
        m_stridedIndex(stridedIndex),
        m_unrollFactor(unrollFactor),
        m_worksize(worksize)
    {
        setIntensity(intensity);
    }

#   ifdef XMRIG_ALGO_RANDOMX
    OclThread(uint32_t index, uint32_t intensity, uint32_t worksize, uint32_t threads, bool gcnAsm, bool datasetHost, uint32_t bfactor) :
        m_datasetHost(datasetHost),
        m_gcnAsm(gcnAsm),
        m_fields(2),
        m_threads(threads, -1),
        m_bfactor(bfactor),
        m_index(index),
        m_memChunk(0),
        m_stridedIndex(0),
        m_worksize(worksize)
    {
        setIntensity(intensity);
    }
#   endif

#   ifdef XMRIG_ALGO_KAWPOW
    OclThread(uint32_t index, uint32_t intensity, uint32_t worksize, uint32_t threads) :
        m_fields(1u << KAWPOW_FIELDS),
        m_threads(threads, -1),
        m_index(index),
        m_memChunk(0),
        m_stridedIndex(0),
        m_unrollFactor(1),
        m_worksize(worksize)
    {
        setIntensity(intensity);
    }
#   endif

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Autotune-generated thread. Only the launch geometry is stored: the
    // tuning knobs (lookup_gap, use_system_ram, reserve_vram_mb,
    // host_ram_budget_mb) stay unpinned so the global "opencl" settings keep
    // steering them across restarts; a per-device JSON entry pins a knob by
    // spelling it out (the has*() discriminators below).
    OclThread(uint32_t index, uint32_t intensity, uint32_t worksize) :
        m_fields(1u << SCRYPT_CHACHA_FIELDS),
        m_threads(1, -1),
        m_index(index),
        m_memChunk(0),
        m_stridedIndex(0),
        m_unrollFactor(1),
        m_worksize(worksize)
    {
        setIntensity(intensity);
    }
#   endif

    OclThread(const rapidjson::Value &value);

    inline bool isAsm() const                               { return m_gcnAsm; }
    inline bool isDatasetHost() const                       { return m_datasetHost; }
    // intensity 0 is the scrypt-chacha auto marker: a per-device JSON entry
    // that pins tuning knobs (SCRYPT_CHACHA_FIELDS set) may omit the
    // intensity, and the runner sizes the launch at init from the merged
    // tuning. Every other family still requires a positive intensity.
    inline bool isValid() const                             { return m_intensity > 0 || m_fields.test(SCRYPT_CHACHA_FIELDS); }
    inline const std::vector<int64_t> &threads() const      { return m_threads; }
    inline uint32_t bfactor() const                         { return m_bfactor; }
    inline uint32_t index() const                           { return m_index; }
    inline uint32_t intensity() const                       { return m_intensity; }
    inline uint32_t memChunk() const                        { return m_memChunk; }
    inline uint32_t stridedIndex() const                    { return m_stridedIndex; }
    inline uint32_t unrollFactor() const                    { return m_unrollFactor; }
    inline uint32_t worksize() const                        { return m_worksize; }

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Per-GPU overrides. has*() returns whether the user pinned the value on
    // this device in the JSON config; the unpinned default is resolved in
    // OclConfig::get() against OclConfig's global field. worksize is a core
    // field of every OclThread, so only its discriminator lives here.
    inline bool hasLookupGap() const                        { return m_hasLookupGap; }
    inline bool hasWorksize() const                         { return m_hasWorksize; }
    inline bool hasUseSystemRam() const                     { return m_hasUseSystemRam; }
    inline bool hasReserveVramMb() const                    { return m_hasReserveVramMb; }
    inline bool hasHostRamBudgetMb() const                  { return m_hasHostRamBudgetMb; }
    inline bool useSystemRam() const                        { return m_useSystemRam; }
    inline uint32_t lookupGap() const                       { return m_lookupGap; }
    inline uint32_t reserveVramMb() const                   { return m_reserveVramMb; }
    inline uint32_t hostRamBudgetMb() const                 { return m_hostRamBudgetMb; }

    // The JSON constructor sets SCRYPT_CHACHA_FIELDS only when a knob key is
    // present, so OclConfig stamps the bit on every thread of the
    // scrypt-chacha profile after the parse. toJSON() then serializes a
    // knobless thread through its scrypt-chacha branch (writing no knob keys)
    // instead of the unroll fallback.
    inline void setScryptChachaFields()                     { m_fields.set(SCRYPT_CHACHA_FIELDS, true); }
#   endif

    inline bool operator!=(const OclThread &other) const    { return !isEqual(other); }
    inline bool operator==(const OclThread &other) const    { return isEqual(other); }

    bool isEqual(const OclThread &other) const;
    rapidjson::Value toJSON(rapidjson::Document &doc) const;

private:
    enum Fields {
        STRIDED_INDEX_FIELD,
        RANDOMX_FIELDS,
        KAWPOW_FIELDS,
        SCRYPT_CHACHA_FIELDS,
        FIELD_MAX
    };

    inline void setIntensity(uint32_t intensity)            { m_intensity = intensity / m_worksize * m_worksize; }

    bool m_datasetHost              = false;
    bool m_gcnAsm                   = true;
    bool m_hasLookupGap             = false;
    bool m_hasWorksize              = false;
    bool m_hasUseSystemRam          = false;
    bool m_hasReserveVramMb         = false;
    bool m_hasHostRamBudgetMb       = false;
    bool m_useSystemRam             = false;
    std::bitset<FIELD_MAX> m_fields = 1;
    std::vector<int64_t> m_threads;
    uint32_t m_bfactor              = 6;
    uint32_t m_index                = 0;
    uint32_t m_intensity            = 0;
    uint32_t m_memChunk             = 2;
    uint32_t m_stridedIndex         = 2;
    uint32_t m_unrollFactor         = 8;
    uint32_t m_worksize             = 0;
    uint32_t m_lookupGap            = 0;
    uint32_t m_reserveVramMb        = 0;
    uint32_t m_hostRamBudgetMb      = 0;
};


} /* namespace xmrig */


#endif /* XMRIG_OCLTHREAD_H */
