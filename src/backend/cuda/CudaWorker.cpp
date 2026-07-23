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


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
void xmrig::CudaWorker::scryptChachaLaunchInfo(uint32_t &launchThreads, uint32_t &launchBlocks, uint64_t &vramBytes, uint64_t &ramBytes) const
{
    launchThreads = 0;
    launchBlocks  = 0;
    vramBytes     = 0;
    ramBytes      = 0;

    // The runner behind a SCRYPT_CHACHA worker is always the scrypt-chacha
    // runner (see the constructor's algorithm switch), so the cast is safe.
    if (m_algorithm.family() == Algorithm::SCRYPT_CHACHA && m_runner != nullptr) {
        const auto *runner = static_cast<const CudaScryptChachaRunner *>(m_runner);

        launchThreads = runner->launchThreads();
        launchBlocks  = runner->launchBlocks();
        vramBytes     = runner->vramScratchpadBytes();
        ramBytes      = runner->ramScratchpadBytes();
    }
}
#endif


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
                logScryptChachaStart(cuda_tag(), "CUDA", startNonce);
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
                logScryptChachaSkipped(cuda_tag(), "CUDA", startNonce);
                commitAbortedProjection();
                std::this_thread::yield();
                continue;
            }

            if (isScryptChacha) {
                const uint64_t launchMs = Chrono::steadyMSecs() - launchStart;
                advanceProjection(launchMs);
                logScryptChachaDone(cuda_tag(), "CUDA", startNonce, intensity(), launchMs);
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
