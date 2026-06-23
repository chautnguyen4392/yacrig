/* XMRig
 * Copyright (c) 2016-2025 XMRig <https://github.com/xmrig>, <support@xmrig.com>
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

#include "backend/cuda/runners/CudaScryptChachaRunner.h"
#include "backend/cuda/CudaLaunchData.h"
#include "backend/cuda/wrappers/CudaLib.h"
#include "backend/common/Tags.h"
#include "base/io/log/Log.h"
#include "base/net/stratum/Job.h"


xmrig::CudaScryptChachaRunner::CudaScryptChachaRunner(size_t index, const CudaLaunchData &data) :
    CudaBaseRunner(index, data)
{
}


bool xmrig::CudaScryptChachaRunner::init()
{
    if (!CudaLib::hasScryptChachaHash()) {
        LOG_ERR("%s scrypt-chacha: loaded CUDA plugin does not export scryptChachaHash; rebuild xmrig-cuda with -DWITH_SCRYPT_CHACHA=ON",
                cuda_tag());
        return false;
    }

    // CudaBaseRunner::init() runs the alloc + deviceInfo + deviceInit chain and
    // calls configureCtx() (below) in between, where the per-GPU tuning is
    // pushed so the plugin's autotune (in deviceInfo) and chunked allocator (in
    // deviceInit) see it.
    if (!CudaBaseRunner::init()) {
        return false;
    }

    // The theoretical figure is in work units (threads * blocks / THREADS_PER_WU),
    // matching CudaBackend's launch-config log and the actual count read back
    // below: otherwise the warning would fire on every Pascal run purely from
    // the 4x cooperation factor, not a real back-off.
    const size_t theoretical = m_data.scryptChachaTheoreticalWorkUnits();
    if (CudaLib::hasScryptChachaWorkUnits()) {
        uint32_t wu = 0;
        if (CudaLib::scryptChachaWorkUnits(m_ctx, &wu)) {
            m_workUnits = wu;
        }
    }
    if (m_workUnits == 0) {
        m_workUnits = static_cast<uint32_t>(theoretical);
    }
    else if (m_workUnits != theoretical) {
        LOG_WARN("%s scrypt-chacha: actual work units %u differs from theoretical %zu",
                 cuda_tag(), m_workUnits, theoretical);
    }

    // Actual VRAM / RAM scratchpad split this device allocated, read back from
    // the runner ctx (scryptchacha_init stored it after any back-off). This works
    // for a JSON-pinned thread too, which has no config-time autotune estimate:
    // the split here is the real allocation, not an estimate.
    constexpr size_t oneMiB = 1024 * 1024;
    const size_t ramWarps   = CudaLib::deviceUint(m_ctx, CudaLib::DeviceScryptChachaRamWarps);
    size_t vramBytes = 0, ramBytes = 0, totalBytes = 0;
    m_data.scryptChachaMemorySplit(ramWarps, m_workUnits, vramBytes, ramBytes, totalBytes);

    LOG_INFO("%s" CYAN_BOLD(" #%u") " scrypt-chacha CUDA ready: %u work units allocated (theoretical %zu, VRAM %zu MiB + RAM %zu MiB, lookup_gap %d, system_ram %s)",
                cuda_tag(), m_data.thread.index(), m_workUnits, theoretical,
                vramBytes / oneMiB, ramBytes / oneMiB,
                m_data.scryptchacha_lookup_gap, m_data.scryptchacha_use_system_ram ? "on" : "off");

    return true;
}


// Push the effective per-GPU tuning to the freshly-allocated ctx, between
// alloc and deviceInfo, so the plugin's autotune and chunked allocator see it.
// Older plugins without scryptChachaConfig fall back to their built-in defaults.
void xmrig::CudaScryptChachaRunner::configureCtx()
{
    if (CudaLib::hasScryptChachaConfig()) {
        CudaLib::scryptChachaConfig(
            m_ctx,
            m_data.scryptchacha_lookup_gap,
            m_data.scryptchacha_use_system_ram,
            m_data.scryptchacha_reserve_vram_mb,
            m_data.scryptchacha_host_ram_budget_mb);

        LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha CUDA tuning: lookup_gap %d, system_ram %s, reserve_vram %d MiB, host_ram_budget %d MiB",
                    cuda_tag(), m_data.thread.index(), m_data.scryptchacha_lookup_gap,
                    m_data.scryptchacha_use_system_ram ? "on" : "off",
                    m_data.scryptchacha_reserve_vram_mb, m_data.scryptchacha_host_ram_budget_mb);
    }
}


bool xmrig::CudaScryptChachaRunner::set(const Job &job, uint8_t *blob)
{
    if (!CudaBaseRunner::set(job, blob)) {
        return false;
    }
    m_jobBlob = blob;
    return true;
}


bool xmrig::CudaScryptChachaRunner::run(uint32_t /*startNonce*/, uint32_t *rescount, uint32_t *resnonce)
{
    return callWrapper(CudaLib::scryptChachaHash(m_ctx, m_jobBlob, m_target, rescount, resnonce, &m_skippedHashes));
}


void xmrig::CudaScryptChachaRunner::jobEarlyNotification(const Job&)
{
    if (CudaLib::hasScryptChachaStopHash()) {
        CudaLib::scryptChachaStopHash(m_ctx);
    }
}
