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

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Final launch geometry and allocated scratchpad split of this worker's
    // runner (valid once the worker reported ready). Zeroes for every other
    // algorithm family. CudaBackend reads it for the launch table printed at
    // all-workers-ready. The OpenCL twin (OclWorker::scryptChachaLaunchInfo)
    // carries only the memory split, since OpenCL keeps its geometry
    // (intensity, worksize) in the launch data.
    void scryptChachaLaunchInfo(uint32_t &launchThreads, uint32_t &launchBlocks, uint64_t &vramBytes, uint64_t &ramBytes) const;
#   endif

protected:
    bool selfTest() override;
    size_t intensity() const override;
    void start() override;

private:
    bool consumeJob();
    void storeStats();

    const Algorithm m_algorithm;
    const Miner *m_miner;
    ICudaRunner *m_runner = nullptr;
    WorkerJob<1> m_job;
};


} // namespace xmrig


#endif /* XMRIG_CUDAWORKER_H */
