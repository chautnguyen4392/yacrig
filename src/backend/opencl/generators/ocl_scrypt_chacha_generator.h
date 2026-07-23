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

#ifndef XMRIG_OCL_SCRYPT_CHACHA_GENERATOR_H
#define XMRIG_OCL_SCRYPT_CHACHA_GENERATOR_H


#include <cstdint>


namespace xmrig {


class Algorithm;
class OclDevice;
class OclThreads;


// Tuning knobs the scrypt-chacha autotune honours. The generator dispatch
// table in OclDevice.cpp has a fixed (device, algorithm, threads) signature
// with no path to the parsed config, so OclConfig::generate() resolves its
// global "opencl" settings into this struct and calls the tuned overload
// below directly (through generateScryptChacha() in OclConfig_gen.h). The
// defaults match OclConfig's global fields.
struct OclScryptChachaTuning
{
    uint32_t lookupGap       = 32;
    uint32_t worksize        = 32;
    uint32_t reserveVramMb   = 0;
    bool     useSystemRam    = false;

    // Absolute per-GPU share of host RAM, already split across the devices by
    // OclConfig::setupScryptChacha(). Only consumed when useSystemRam is set.
    uint32_t hostRamBudgetMb = 0;
};


bool ocl_scrypt_chacha_generator(const OclDevice &device, const Algorithm &algorithm, OclThreads &threads, const OclScryptChachaTuning &tuning);


} // namespace xmrig


#endif // XMRIG_OCL_SCRYPT_CHACHA_GENERATOR_H
