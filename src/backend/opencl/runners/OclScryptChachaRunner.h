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

#ifndef XMRIG_OCLSCRYPTCHACHARUNNER_H
#define XMRIG_OCLSCRYPTCHACHARUNNER_H


#include "backend/opencl/runners/OclBaseRunner.h"

#include <string>
#include <vector>


namespace xmrig {


// Drives the scrypt-chacha (YAC) search84 kernel. The generator autotunes the
// intensity from the card's free VRAM (plus this GPU's host-RAM share when
// use_system_ram is set), and this runner turns it into a flat table of
// scratchpad chunks (each within CL_DEVICE_MAX_MEM_ALLOC_SIZE, no fixed
// count): host chunks first, so the slow host-mapped work-groups take the low
// relative ids and overlap with the fast VRAM work, then the VRAM chunks.
// OpenCL kernels cannot index an arbitrary argument set, so init() generates
// the search84 entry point (one padcacheN argument per chunk plus the
// selection ladder) and appends it to the base kernel source before build().
// Chunk allocation failures back off before the source is generated, and the
// reduced launch size is reported through roundSize() so the nonce stride
// stays honest. Early-abort mirrors OclKawPowRunner: a control queue arms a
// device-side stop flag the kernel polls in its long loop.
class OclScryptChachaRunner : public OclBaseRunner
{
public:
    XMRIG_DISABLE_COPY_MOVE_DEFAULT(OclScryptChachaRunner)

    OclScryptChachaRunner(size_t index, const OclLaunchData &data);
    ~OclScryptChachaRunner() override;

    // Actual allocated scratchpad split, valid once init() has run (post
    // back-off). The backend reads these through OclWorker for the launch
    // table it prints when every worker is ready. The RAM figure is backed by
    // host-mapped (CL_MEM_ALLOC_HOST_PTR) chunks; the accessor names match
    // the CUDA runner's so both backends read identically.
    inline uint64_t ramScratchpadBytes() const  { return m_ramScratchpadBytes; }
    inline uint64_t vramScratchpadBytes() const { return m_vramScratchpadBytes; }

protected:
    void run(uint32_t nonce, uint32_t nonce_offset, uint32_t *hashOutput) override;
    void set(const Job &job, uint8_t *blob) override;
    void build() override;
    void init() override;
    void jobEarlyNotification(const Job &job) override;
    const char *source() const override { return m_generatedSource.c_str(); }
    uint32_t roundSize() const override { return m_launchedIntensity; }
    uint32_t processedHashes() const override { return m_launchedIntensity - m_skippedHashes; }

private:
    uint64_t vramBudgetBytes() const;
    void allocateScratchpads(uint32_t numberGroups, uint32_t maxGroupsPerChunk, size_t groupBytes);
    uint32_t allocateChunkSet(uint32_t numberGroups, uint32_t maxGroupsPerChunk, size_t groupBytes, cl_mem_flags flags, std::vector<cl_mem> &chunks, std::vector<uint32_t> &chunkThreads);
    std::string generateSearch84() const;

    uint8_t *m_blob             = nullptr;
    uint32_t m_skippedHashes    = 0;
    uint32_t m_target           = 0;

    // Local work-group size. The constructor always overwrites this with the
    // merged per-GPU worksize; the initializer is the global default.
    size_t m_workGroupSize      = 32;

    // The launch size actually backed by allocated scratchpad chunks. Equals
    // the generator's intensity unless allocation backed off; always a whole
    // multiple of the worksize.
    uint32_t m_launchedIntensity = 0;

    // Allocated scratchpad bytes behind m_launchedIntensity: host-mapped
    // (RAM) chunks and VRAM chunks, set by allocateScratchpads.
    uint64_t m_ramScratchpadBytes  = 0;
    uint64_t m_vramScratchpadBytes = 0;

    cl_kernel m_searchKernel    = nullptr;

    // Flat chunk table and the per-chunk work-item counts (the generated
    // kernel's THREADS_PER_BUFFER_N values, in table order). The first
    // m_hostChunkCount entries are host (CL_MEM_ALLOC_HOST_PTR) chunks, the
    // rest are VRAM chunks.
    std::vector<cl_mem> m_scratchpads;
    std::vector<uint32_t> m_chunkThreads;
    size_t m_hostChunkCount     = 0;

    // Base kernel source with the generated search84 appended; source() hands
    // it to the build/cache machinery in place of the stock source.
    std::string m_generatedSource;

    cl_command_queue m_controlQueue = nullptr;
    cl_mem m_stop               = nullptr;
};


} /* namespace xmrig */


#endif // XMRIG_OCLSCRYPTCHACHARUNNER_H
