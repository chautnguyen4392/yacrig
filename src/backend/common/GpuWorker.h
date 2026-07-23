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

#ifndef XMRIG_GPUWORKER_H
#define XMRIG_GPUWORKER_H


#include <atomic>

#include <mutex>


#include "backend/common/HashrateInterpolator.h"
#include "backend/common/Worker.h"


namespace xmrig {


class GpuWorker : public Worker
{
public:
    GpuWorker(size_t id, int64_t affinity, int priority, uint32_t m_deviceIndex);

protected:
    inline const VirtualMemory *memory() const override     { return nullptr; }
    inline uint32_t deviceIndex() const                     { return m_deviceIndex; }

    void hashrateData(uint64_t &hashCount, uint64_t &timeStamp, uint64_t &rawHashes) const override;

protected:
    void storeStats();

    // scrypt-chacha hashrate projection, shared by the OpenCL and CUDA
    // workers. A single launch runs for minutes, far longer than the 10s/60s
    // sampling windows, so storeStats records only one point per launch and
    // every window reads stale (speed stays n/a). hashrateData() instead
    // serves a smooth, continuous display count that ramps at the measured
    // per-launch rate while the worker blocks inside its runner.
    //
    // The worker's launch loop drives the projection: beginProjection at
    // launch start, then either advanceProjection (the launch completed,
    // launchMs becomes the rate denominator for the next launch) or
    // commitAbortedProjection (an early-abort skipped the launch: fold the
    // ramp already displayed into the base so the count never steps
    // backwards, but keep the previous rate denominator).
    void beginProjection(uint64_t launchStart);
    void advanceProjection(uint64_t launchMs);
    void commitAbortedProjection();

    // scrypt-chacha per-launch log lines, shared by both GPU workers.
    // Message formatting only; the projection state is updated by the
    // helpers above. tag is the backend log tag (ocl_tag() / cuda_tag()) and
    // backend the in-message backend name ("OpenCL" / "CUDA").
    void logScryptChachaStart(const char *tag, const char *backend, uint32_t startNonce) const;
    void logScryptChachaSkipped(const char *tag, const char *backend, uint32_t startNonce) const;
    void logScryptChachaDone(const char *tag, const char *backend, uint32_t startNonce, uint64_t workUnits, uint64_t launchMs) const;

    const uint32_t m_deviceIndex;
    HashrateInterpolator m_hashrateData;
    std::atomic<uint32_t> m_index   = {};
    uint64_t m_hashCount[2]         = {};
    uint64_t m_timestamp[2]         = {};

private:
    // Projection state. The display count is deliberately decoupled from
    // m_count's per-launch steps: it only ever advances by the projection
    // itself. The first projected launch ramps from 0 (no jump to the first
    // launch's full total), and each launch continues from exactly where the
    // previous one's projection ended, so the warm-up handoff and launch
    // boundaries introduce no count discontinuity. A discontinuity would
    // briefly look like a faster-than-real rate to the sampler and pollute
    // the running max. The absolute value is irrelevant (only the slope
    // drives the displayed H/s), so starting at 0 is fine; nonce striding
    // uses intensity()/roundSize(), not this count. m_projEstMs stays 0 for
    // every other algorithm family (nothing calls the projection mutators),
    // which is what routes hashrateData() to the default snapshot path.
    mutable std::mutex m_projLock;
    uint64_t m_projBase      = 0;   // display count at the current launch's start
    uint64_t m_projStart     = 0;   // current launch start (steadyMSecs)
    uint64_t m_projWorkUnits = 0;   // work units the current launch adds
    uint64_t m_projEstMs     = 0;   // rate denominator: the previous launch's duration (0 until the first completes)
};


} // namespace xmrig


#endif /* XMRIG_GPUWORKER_H */
