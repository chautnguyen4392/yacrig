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


#include "backend/opencl/OclWorker.h"
#include "backend/common/Tags.h"
#include "backend/opencl/runners/OclCnRunner.h"
#include "backend/opencl/runners/tools/OclSharedData.h"
#include "backend/opencl/runners/tools/OclSharedState.h"
#include "base/io/log/Log.h"
#include "base/tools/Alignment.h"
#include "base/tools/Chrono.h"
#include "core/Miner.h"
#include "crypto/common/Nonce.h"
#include "net/JobResults.h"


#ifdef XMRIG_ALGO_RANDOMX
#   include "backend/opencl/runners/OclRxJitRunner.h"
#   include "backend/opencl/runners/OclRxVmRunner.h"
#endif

#ifdef XMRIG_ALGO_KAWPOW
#   include "backend/opencl/runners/OclKawPowRunner.h"
#endif

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
#   include "backend/opencl/runners/OclScryptChachaRunner.h"
#endif

#include <cassert>
#include <thread>


namespace xmrig {


std::atomic<bool> OclWorker::ready;


static inline bool isReady()    { return !Nonce::isPaused() && OclWorker::ready; }


static inline void printError(size_t id, const char *error)
{
    LOG_ERR("%s" RED_S " thread " RED_BOLD("#%zu") RED_S " failed with error " RED_BOLD("%s"), ocl_tag(), id, error);
}


} // namespace xmrig



xmrig::OclWorker::OclWorker(size_t id, const OclLaunchData &data) :
    GpuWorker(id, data.affinity, -1, data.device.index()),
    m_algorithm(data.algorithm),
    m_miner(data.miner),
    m_sharedData(OclSharedState::get(data.device.index()))
{
    switch (m_algorithm.family()) {
    case Algorithm::RANDOM_X:
#       ifdef XMRIG_ALGO_RANDOMX
        if (data.thread.isAsm() && data.device.vendorId() == OCL_VENDOR_AMD) {
            m_runner = new OclRxJitRunner(id, data);
        }
        else {
            m_runner = new OclRxVmRunner(id, data);
        }
#       endif
        break;

    case Algorithm::ARGON2:
#       ifdef XMRIG_ALGO_ARGON2
        m_runner = nullptr;
#       endif
        break;

    case Algorithm::KAWPOW:
#       ifdef XMRIG_ALGO_KAWPOW
        m_runner = new OclKawPowRunner(id, data);
#       endif
        break;

    case Algorithm::SCRYPT_CHACHA:
#       ifdef XMRIG_ALGO_SCRYPT_CHACHA
        m_runner = new OclScryptChachaRunner(id, data);
#       endif
        break;

    default:
        m_runner = new OclCnRunner(id, data);
        break;
    }

    if (!m_runner) {
        return;
    }

    try {
        m_runner->init();
        m_runner->build();
    }
    catch (std::exception &ex) {
        printError(id, ex.what());

        delete m_runner;
        m_runner = nullptr;
    }
}


xmrig::OclWorker::~OclWorker()
{
    delete m_runner;
}


void xmrig::OclWorker::jobEarlyNotification(const Job &job)
{
    if (m_runner) {
        m_runner->jobEarlyNotification(job);
    }
}


bool xmrig::OclWorker::selfTest()
{
    return m_runner != nullptr;
}


size_t xmrig::OclWorker::intensity() const
{
    return m_runner ? m_runner->roundSize() : 0;
}


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
void xmrig::OclWorker::scryptChachaLaunchInfo(uint64_t &vramBytes, uint64_t &ramBytes) const
{
    vramBytes = 0;
    ramBytes  = 0;

    // The runner behind a SCRYPT_CHACHA worker is always the scrypt-chacha
    // runner (see the constructor's algorithm switch), so the cast is safe.
    if (m_algorithm.family() == Algorithm::SCRYPT_CHACHA && m_runner != nullptr) {
        const auto *runner = static_cast<const OclScryptChachaRunner *>(m_runner);

        vramBytes = runner->vramScratchpadBytes();
        ramBytes  = runner->ramScratchpadBytes();
    }
}
#endif


void xmrig::OclWorker::start()
{
    cl_uint results[0x100];

    while (Nonce::sequence(Nonce::OPENCL) > 0) {
        if (!isReady()) {
            m_sharedData.setResumeCounter(0);

            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            while (!isReady() && Nonce::sequence(Nonce::OPENCL) > 0);

            if (Nonce::sequence(Nonce::OPENCL) == 0) {
                break;
            }

            m_sharedData.resumeDelay(id());

            if (!consumeJob()) {
                return;
            }
        }

        while (!Nonce::isOutdated(Nonce::OPENCL, m_job.sequence())) {
            m_sharedData.adjustDelay(id());

            const bool isScryptChacha = m_algorithm.family() == Algorithm::SCRYPT_CHACHA;
            const uint32_t startNonce = readUnaligned(m_job.nonce());
            const uint64_t t          = Chrono::steadyMSecs();

            if (isScryptChacha) {
                beginProjection(t);
                logScryptChachaStart(ocl_tag(), "OpenCL", startNonce);
            }

            try {
                m_runner->run(startNonce, m_job.nonceOffset(), results);
            }
            catch (std::exception &ex) {
                printError(id(), ex.what());

                return;
            }

            // For scrypt-chacha, an early-abort armed before the kernel made
            // progress skips every work-group and reports processedHashes() == 0.
            // That zero is the explicit "no work done" signal: such a launch must
            // not advance the nonce, count toward the hashrate, set the estimated
            // launch duration, or log a completed-launch line. No work-group ran
            // to the output stage, so the output buffer holds no candidates.
            // Treat it as a no-op and retry the same nonce range.
            if (isScryptChacha && m_runner->processedHashes() == 0) {
                logScryptChachaSkipped(ocl_tag(), "OpenCL", startNonce);
                commitAbortedProjection();
                std::this_thread::yield();
                continue;
            }

            if (isScryptChacha) {
                const uint64_t launchMs = Chrono::steadyMSecs() - t;
                advanceProjection(launchMs);
                logScryptChachaDone(ocl_tag(), "OpenCL", startNonce, intensity(), launchMs);
            }

            if (results[0xFF] > 0) {
                if (isScryptChacha) {
                    LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha %u candidate%s passed the GPU prefilter, submitting for CPU re-verify",
                                ocl_tag(), m_deviceIndex, results[0xFF], results[0xFF] > 1 ? "s" : "");
                }
                JobResults::submit(m_job.currentJob(), results, results[0xFF], m_deviceIndex);
            }

            if (!Nonce::isOutdated(Nonce::OPENCL, m_job.sequence()) && !m_job.nextRound(1, intensity())) {
                JobResults::done(m_job.currentJob());
            }

            storeStats(t);
            std::this_thread::yield();
        }

        if (isReady() && !consumeJob()) {
            return;
        }
    }
}


bool xmrig::OclWorker::consumeJob()
{
    if (Nonce::sequence(Nonce::OPENCL) == 0) {
        return false;
    }

    m_job.add(m_miner->job(), intensity(), Nonce::OPENCL);

    try {
        m_runner->set(m_job.currentJob(), m_job.blob());
    }
    catch (std::exception &ex) {
        printError(id(), ex.what());

        return false;
    }

    return true;
}


void xmrig::OclWorker::storeStats(uint64_t t)
{
    if (!isReady()) {
        return;
    }

    m_count += m_runner->processedHashes();
    const uint64_t timeStamp = Chrono::steadyMSecs();

    m_hashrateData.addDataPoint(m_count, timeStamp);

    m_sharedData.setRunTime(timeStamp - t);

    GpuWorker::storeStats();
}
