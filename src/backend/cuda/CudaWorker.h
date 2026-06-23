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

#ifndef XMRIG_CUDAWORKER_H
#define XMRIG_CUDAWORKER_H


#include "backend/common/GpuWorker.h"
#include "backend/common/WorkerJob.h"
#include "backend/cuda/CudaLaunchData.h"
#include "base/tools/Object.h"
#include "net/JobResult.h"


#include <mutex>


namespace xmrig {


class ICudaRunner;


class CudaWorker : public GpuWorker
{
public:
    XMRIG_DISABLE_COPY_MOVE_DEFAULT(CudaWorker)

    CudaWorker(size_t id, const CudaLaunchData &data);

    ~CudaWorker() override;

    void jobEarlyNotification(const Job &job) override;

    static std::atomic<bool> ready;

protected:
    bool selfTest() override;
    size_t intensity() const override;
    void start() override;
    void hashrateData(uint64_t &hashCount, uint64_t &timeStamp, uint64_t &rawHashes) const override;

private:
    bool consumeJob();
    void storeStats();

    // scrypt-chacha hashrate projection helpers (see the m_proj* members below).
    void beginProjection(uint64_t launchStart);
    void advanceProjection(uint64_t launchMs);
    void commitAbortedProjection();

    // scrypt-chacha per-launch log lines. Message formatting only; the
    // projection state is updated by the helpers above.
    void logScryptChachaStart(uint32_t startNonce) const;
    void logScryptChachaSkipped(uint32_t startNonce) const;
    void logScryptChachaDone(uint32_t startNonce, uint64_t workUnits, uint64_t launchMs) const;

    const Algorithm m_algorithm;
    const Miner *m_miner;
    ICudaRunner *m_runner = nullptr;
    WorkerJob<1> m_job;

    // scrypt-chacha hashrate projection. A single launch runs for minutes, far
    // longer than the 10s/60s sampling windows, so storeStats records only one
    // point per launch and every window reads stale (speed stays n/a). The tick
    // thread instead reads a smooth, continuous display count that ramps at the
    // measured per-launch rate while run() blocks.
    //
    // The display count is deliberately decoupled from m_count's per-launch
    // steps: it only ever advances by the projection itself. The first projected
    // launch ramps from 0 (no jump to the first launch's full total), and each
    // launch continues from exactly where the previous one's projection ended,
    // so the warm-up handoff and launch boundaries introduce no count
    // discontinuity. A discontinuity would briefly look like a faster-than-real
    // rate to the sampler and pollute the running max. The absolute value is
    // irrelevant (only the slope drives the displayed H/s), so starting at 0 is
    // fine; nonce striding uses intensity()/roundSize(), not this count.
    mutable std::mutex m_projLock;
    uint64_t m_projBase      = 0;   // display count at the current launch's start
    uint64_t m_projStart     = 0;   // current launch start (steadyMSecs)
    uint64_t m_projWorkUnits = 0;   // work units the current launch adds
    uint64_t m_projEstMs     = 0;   // rate denominator: the previous launch's duration (0 until the first completes)
};


} // namespace xmrig


#endif /* XMRIG_CUDAWORKER_H */
