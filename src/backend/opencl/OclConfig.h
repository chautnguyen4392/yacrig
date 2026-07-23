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

#ifndef XMRIG_OCLCONFIG_H
#define XMRIG_OCLCONFIG_H


#include "backend/common/Threads.h"
#include "backend/opencl/OclLaunchData.h"
#include "backend/opencl/OclThreads.h"
#include "backend/opencl/wrappers/OclPlatform.h"


namespace xmrig {


class OclConfig
{
public:
    OclConfig();

    OclPlatform platform() const;
    rapidjson::Value toJSON(rapidjson::Document &doc) const;
    std::vector<OclLaunchData> get(const Miner *miner, const Algorithm &algorithm, const OclPlatform &platform, const std::vector<OclDevice> &devices) const;
    void read(const rapidjson::Value &value);

    inline bool isCacheEnabled() const                  { return m_cache; }
    inline bool isEnabled() const                       { return m_enabled; }
    inline bool isShouldSave() const                    { return m_shouldSave; }
    inline const String &loader() const                 { return m_loader; }
    inline const Threads<OclThreads> &threads() const   { return m_threads; }

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    inline uint32_t lookupGap() const                   { return m_lookupGap; }
    inline uint32_t worksize() const                    { return m_worksize; }
    inline bool useSystemRam() const                    { return m_useSystemRam; }
    inline uint32_t reserveVramMb() const               { return m_reserveVramMb; }
    inline uint32_t reserveRamMb() const                { return m_reserveRamMb; }
    inline uint32_t hostRamBudgetMb() const             { return m_hostRamBudgetMb; }
    inline uint32_t defaultHostRamBudgetMbPerGpu() const { return m_defaultHostRamBudgetMbPerGpu; }
#   endif

#   ifdef XMRIG_FEATURE_ADL
    inline bool isAdlEnabled() const                    { return m_adl; }
#   endif

private:
    void generate();
    void setDevicesHint(const char *devicesHint);

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    void setupScryptChacha(const std::vector<OclDevice> &devices);
#   endif

    bool m_cache         = true;
    bool m_enabled       = false;
    bool m_shouldSave    = false;
    std::vector<uint32_t> m_devicesHint;
    String m_loader;
    Threads<OclThreads> m_threads;

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Global scrypt-chacha tuning ("opencl" JSON fields / --opencl-* options),
    // CUDA-matching defaults except lookup_gap (the OpenCL donor default is
    // 32). All but reserve_ram_mb can be overridden per device through the
    // "scrypt-chacha" thread entries; the merge happens in get().
    uint32_t m_lookupGap       = 32;
    uint32_t m_worksize        = 32;
    bool     m_useSystemRam    = false;
    uint32_t m_reserveVramMb   = 0;
    uint32_t m_reserveRamMb    = 4096;  // host-level OS reserve, never per-GPU
    uint32_t m_hostRamBudgetMb = 4096;  // global host-RAM cap for all GPUs, split evenly; 0 = use MemAvailable - reserve_ram_mb

    // Derived in setupScryptChacha(), not parsed: the global host budget split
    // evenly across the system-RAM devices.
    uint32_t m_defaultHostRamBudgetMbPerGpu = 0;
#   endif

#   ifndef XMRIG_OS_APPLE
    void setPlatform(const rapidjson::Value &platform);

    String m_platformVendor;
    uint32_t m_platformIndex = 0;
#   endif

#   ifdef XMRIG_FEATURE_ADL
    bool m_adl          = true;
#   endif
};


} /* namespace xmrig */


#endif /* XMRIG_OCLCONFIG_H */
