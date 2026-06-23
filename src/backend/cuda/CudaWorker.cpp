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


#include "backend/cuda/CudaWorker.h"
#include "backend/common/Tags.h"
#include "backend/cuda/runners/CudaCnRunner.h"
#include "backend/cuda/wrappers/CudaDevice.h"
#include "base/io/log/Log.h"
#include "base/tools/Alignment.h"
#include "base/tools/Chrono.h"
#include "core/Miner.h"
#include "crypto/common/Nonce.h"
#include "net/JobResults.h"


#ifdef XMRIG_ALGO_RANDOMX
#   include "backend/cuda/runners/CudaRxRunner.h"
#endif


#ifdef XMRIG_ALGO_KAWPOW
#   include "backend/cuda/runners/CudaKawPowRunner.h"
#endif


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
#   include "backend/cuda/runners/CudaScryptChachaRunner.h"
#endif


#include <cassert>
#include <thread>


namespace xmrig {


std::atomic<bool> CudaWorker::ready;


static inline bool isReady()    { return !Nonce::isPaused() && CudaWorker::ready; }


// Work units to fold into the scrypt-chacha projection for `elapsedMs` of a
// launch whose estimated total duration is `estMs`: the [0, 1]-clamped elapsed
// fraction times the launch's work-unit count. estMs == 0 (no measured rate
// yet) contributes nothing, and the clamp saturates the ramp at the full
// work-unit count once a launch overruns its estimate. Single source for the
// fraction math shared by advanceProjection, commitAbortedProjection, and
// hashrateData.
static inline uint64_t projectionRamp(uint64_t elapsedMs, uint64_t estMs, uint64_t workUnits)
{
    if (estMs == 0) {
        return 0;
    }

    double fraction = static_cast<double>(elapsedMs) / static_cast<double>(estMs);
    if (fraction > 1.0) {
        fraction = 1.0;
    }

    return static_cast<uint64_t>(fraction * static_cast<double>(workUnits));
}


} // namespace xmrig



xmrig::CudaWorker::CudaWorker(size_t id, const CudaLaunchData &data) :
    GpuWorker(id, data.thread.affinity(), -1, data.device.index()),
    m_algorithm(data.algorithm),
    m_miner(data.miner)
{
    switch (m_algorithm.family()) {
    case Algorithm::RANDOM_X:
#       ifdef XMRIG_ALGO_RANDOMX
        m_runner = new CudaRxRunner(id, data);
#       endif
        break;

    case Algorithm::ARGON2:
        break;

    case Algorithm::KAWPOW:
#       ifdef XMRIG_ALGO_KAWPOW
        m_runner = new CudaKawPowRunner(id, data);
#       endif
        break;

    case Algorithm::SCRYPT_CHACHA:
#       ifdef XMRIG_ALGO_SCRYPT_CHACHA
        m_runner = new CudaScryptChachaRunner(id, data);
#       endif
        break;

    default:
        m_runner = new CudaCnRunner(id, data);
        break;
    }

    if (!m_runner) {
        return;
    }

    if (!m_runner->init()) {
        delete m_runner;

        m_runner = nullptr;
    }
}


xmrig::CudaWorker::~CudaWorker()
{
    delete m_runner;
}


void xmrig::CudaWorker::jobEarlyNotification(const Job &job)
{
    if (m_runner) {
        m_runner->jobEarlyNotification(job);
    }
}


bool xmrig::CudaWorker::selfTest()
{
    return m_runner != nullptr;
}


size_t xmrig::CudaWorker::intensity() const
{
    return m_runner ? m_runner->roundSize() : 0;
}


void xmrig::CudaWorker::start()
{
    while (Nonce::sequence(Nonce::CUDA) > 0) {
        if (!isReady()) {
            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            while (!isReady() && Nonce::sequence(Nonce::CUDA) > 0);

            if (Nonce::sequence(Nonce::CUDA) == 0) {
                break;
            }

            if (!consumeJob()) {
                return;
            }
        }

        while (!Nonce::isOutdated(Nonce::CUDA, m_job.sequence())) {
            uint32_t foundNonce[16] = { 0 };
            uint32_t foundCount     = 0;

            const bool isScryptChacha  = m_algorithm.family() == Algorithm::SCRYPT_CHACHA;
            const uint32_t startNonce  = readUnaligned(m_job.nonce());
            const uint64_t launchStart = Chrono::steadyMSecs();

            if (isScryptChacha) {
                beginProjection(launchStart);
                logScryptChachaStart(startNonce);
            }

            if (!m_runner->run(startNonce, &foundCount, foundNonce)) {
                return;
            }

            // For scrypt-chacha, a coarse early-abort returns without running the
            // pipeline and reports processedHashes() == 0. That zero (not the
            // launch duration) is the explicit "no work done" signal: such a
            // launch must not advance the nonce, count toward the hashrate, set
            // the estimated launch duration, or log a completed-launch line. Treat
            // it as a no-op and retry the same nonce range.
            if (isScryptChacha && m_runner->processedHashes() == 0) {
                logScryptChachaSkipped(startNonce);
                commitAbortedProjection();
                std::this_thread::yield();
                continue;
            }

            if (isScryptChacha) {
                const uint64_t launchMs = Chrono::steadyMSecs() - launchStart;
                advanceProjection(launchMs);
                logScryptChachaDone(startNonce, intensity(), launchMs);
            }

            if (foundCount) {
                if (isScryptChacha) {
                    LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha %u candidate%s passed the GPU prefilter, submitting for CPU re-verify",
                                cuda_tag(), m_deviceIndex, foundCount, foundCount > 1 ? "s" : "");
                }
                JobResults::submit(m_job.currentJob(), foundNonce, foundCount, m_deviceIndex);
            }

            if (!Nonce::isOutdated(Nonce::CUDA, m_job.sequence()) && !m_job.nextRound(1, intensity())) {
                JobResults::done(m_job.currentJob());
            }

            storeStats();
            std::this_thread::yield();
        }

        if (isReady() && !consumeJob()) {
            return;
        }
    }
}


bool xmrig::CudaWorker::consumeJob()
{
    if (Nonce::sequence(Nonce::CUDA) == 0) {
        return false;
    }

    m_job.add(m_miner->job(), intensity(), Nonce::CUDA);

    return m_runner->set(m_job.currentJob(), m_job.blob());
}


// Anchor the projection at the start of a scrypt-chacha launch. m_projBase
// carries over from the previous launch's end (set in advanceProjection), so the
// ramp is continuous across the boundary.
void xmrig::CudaWorker::beginProjection(uint64_t launchStart)
{
    std::lock_guard<std::mutex> lock(m_projLock);
    m_projStart     = launchStart;
    m_projWorkUnits = intensity();
}


// Advance the display count to exactly where this launch's projection ended (no
// step), using the rate that was in effect during the launch (the previous
// launch's duration). The first launch has no prior duration, so it contributes
// nothing and the next launch ramps from 0.
void xmrig::CudaWorker::advanceProjection(uint64_t launchMs)
{
    std::lock_guard<std::mutex> lock(m_projLock);
    m_projBase += projectionRamp(launchMs, m_projEstMs, m_projWorkUnits);
    m_projEstMs = launchMs;                 // rate denominator for the next launch
    m_projStart = Chrono::steadyMSecs();    // park so a tick before the next launch sees frac ~ 0
}


// Fold the ramp an early-aborted launch already displayed into m_projBase so the
// display count never steps backwards. The projection had ramped the count up to
// m_projBase + frac * m_projWorkUnits while the aborted launch ran, but the
// launch did no work so advanceProjection is not called: without this, the retry
// launch would restart the ramp from the un-advanced base and the count would
// drop, which the sampler reads as an unsigned underflow (a colossal spurious
// hashrate that latches the running max). m_projEstMs is left untouched: an
// aborted launch's truncated duration is not a valid rate denominator.
void xmrig::CudaWorker::commitAbortedProjection()
{
    std::lock_guard<std::mutex> lock(m_projLock);
    if (m_projEstMs == 0) {
        return;
    }

    const uint64_t now     = Chrono::steadyMSecs();
    const uint64_t elapsed = now > m_projStart ? now - m_projStart : 0;
    m_projBase += projectionRamp(elapsed, m_projEstMs, m_projWorkUnits);
    m_projStart = now;                      // ramp folded into base, restart from here so a tick before the retry sees frac ~ 0
}


void xmrig::CudaWorker::logScryptChachaStart(uint32_t startNonce) const
{
    LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha CUDA processing nonces " CYAN_BOLD("%u") "-" CYAN_BOLD("%u")
             " (" CYAN_BOLD("%u") " work units)",
             cuda_tag(), m_deviceIndex, startNonce,
             static_cast<uint32_t>(startNonce + intensity() - 1), static_cast<uint32_t>(intensity()));
}


void xmrig::CudaWorker::logScryptChachaSkipped(uint32_t startNonce) const
{
    LOG_INFO("%s" CYAN_BOLD(" #%u") " scrypt-chacha CUDA skipped nonces " CYAN_BOLD("%u") "-" CYAN_BOLD("%u")
             " (early abort)",
             cuda_tag(), m_deviceIndex, startNonce,
             static_cast<uint32_t>(startNonce + intensity() - 1));
}


void xmrig::CudaWorker::logScryptChachaDone(uint32_t startNonce, uint64_t workUnits, uint64_t launchMs) const
{
    const double hr = launchMs > 0 ? static_cast<double>(workUnits) * 1000.0 / static_cast<double>(launchMs) : 0.0;
    LOG_INFO("%s" CYAN_BOLD(" #%u") " scrypt-chacha CUDA processed nonces " CYAN_BOLD("%u") "-" CYAN_BOLD("%u")
             " (" CYAN_BOLD("%u") " work units) in " CYAN_BOLD("%llu") " ms (" CYAN_BOLD("%.2f") " H/s)",
             cuda_tag(), m_deviceIndex, startNonce, static_cast<uint32_t>(startNonce + workUnits - 1),
             static_cast<uint32_t>(workUnits), static_cast<unsigned long long>(launchMs), hr);
}


void xmrig::CudaWorker::storeStats()
{
    if (!isReady()) {
        return;
    }

    m_count += m_runner ? m_runner->processedHashes() : 0;

    const uint64_t timeStamp = Chrono::steadyMSecs();
    m_hashrateData.addDataPoint(m_count, timeStamp);

    GpuWorker::storeStats();
}


void xmrig::CudaWorker::hashrateData(uint64_t &hashCount, uint64_t &timeStamp, uint64_t &rawHashes) const
{
    uint64_t estMs, start, base, wu;
    {
        std::lock_guard<std::mutex> lock(m_projLock);
        estMs = m_projEstMs;
        start = m_projStart;
        base  = m_projBase;
        wu    = m_projWorkUnits;
    }

    // Every algorithm except scrypt-chacha launches in well under a sampling
    // window, so the frozen per-launch snapshot the base class returns is fine.
    // scrypt-chacha before its first launch completes (estMs == 0) also has no
    // rate yet, so it reads n/a until the projection has a real duration.
    if (m_algorithm.family() != Algorithm::SCRYPT_CHACHA || estMs == 0) {
        GpuWorker::hashrateData(hashCount, timeStamp, rawHashes);
        return;
    }

    // A scrypt-chacha launch processes its work units at a steady rate over a
    // duration that barely varies (memory-bound, constant work). Ramp the
    // continuous display count from the fraction of the previous launch's
    // duration that has elapsed, so the tick thread feeds the sampler a growing
    // count with a current timestamp ~twice a second instead of one stale point
    // per launch. base carries over from the previous launch's end, so the ramp
    // is continuous (no step, no over-read of the running max).
    const uint64_t now       = Chrono::steadyMSecs();
    const uint64_t elapsed   = now > start ? now - start : 0;
    const uint64_t projected = base + projectionRamp(elapsed, estMs, wu);

    hashCount = projected;
    timeStamp = now;
    rawHashes = projected;
}
