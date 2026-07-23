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

#include <cinttypes>
#include <cstring>
#include <stdexcept>


#include "backend/opencl/runners/OclScryptChachaRunner.h"
#include "backend/common/Tags.h"
#include "backend/opencl/OclLaunchData.h"
#include "backend/opencl/wrappers/OclError.h"
#include "backend/opencl/wrappers/OclLib.h"
#include "base/io/log/Log.h"
#include "base/net/stratum/Job.h"
#include "crypto/scrypt-chacha/scrypt-chacha.h"


namespace xmrig {


// YAC block header size the search84 kernel consumes (bytes 80..83 hold the
// nonce, which the kernel overwrites per work-item with get_global_id(0)).
constexpr size_t SCRYPT_CHACHA_HEADER_SIZE = 84;


} // namespace xmrig


xmrig::OclScryptChachaRunner::OclScryptChachaRunner(size_t index, const OclLaunchData &data) :
    OclBaseRunner(index, data)
{
    // Effective tuning resolved by OclConfig::get(): a knob pinned on the
    // thread wins over the global "opencl" field.
    m_workGroupSize = data.scryptchacha_worksize;
}


xmrig::OclScryptChachaRunner::~OclScryptChachaRunner()
{
    OclLib::release(m_searchKernel);

    for (cl_mem chunk : m_scratchpads) {
        OclLib::release(chunk);
    }

    OclLib::release(m_controlQueue);
    OclLib::release(m_stop);
}


void xmrig::OclScryptChachaRunner::init()
{
    // Effective per-GPU tuning this runner mines with (per-device pins merged
    // over the globals by OclConfig::get()). Printed at runner init on both
    // GPU backends in the same shape; the CUDA line has no worksize field
    // because that knob is OpenCL-only.
    LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha tuning: lookup_gap %u, worksize %zu, system_ram %s, reserve_vram %u MiB, host_ram_budget %u MiB",
                ocl_tag(), data().device.index(),
                data().scryptchacha_lookup_gap, m_workGroupSize,
                data().scryptchacha_use_system_ram ? "on" : "off",
                data().scryptchacha_reserve_vram_mb,
                data().scryptchacha_host_ram_budget_mb);

    // Dedicated small buffer for the job blob and the result words instead of
    // OclBaseRunner::init(): on an AMD device the base implementation routes
    // small buffers into a shared allocation that OclSharedData rounds up to a
    // whole GiB, VRAM the autotuned scratchpad table should get instead.
    m_queue  = OclLib::createCommandQueue(m_ctx, data().device.id());
    m_buffer = OclLib::createBuffer(m_ctx, CL_MEM_READ_WRITE, align(bufferSize()));
    m_input  = createSubBuffer(CL_MEM_READ_ONLY | CL_MEM_HOST_WRITE_ONLY, Job::kMaxBlobSize);
    m_output = createSubBuffer(CL_MEM_READ_WRITE, sizeof(cl_uint) * 0x100);

    // Control queue + two-word stop buffer for early-abort, mirroring KawPow:
    // stop[0] is the abort flag armed on the control queue while the main queue
    // is busy with the long kernel, stop[1] counts skipped work-groups.
    m_controlQueue = OclLib::createCommandQueue(m_ctx, data().device.id());
    m_stop         = OclLib::createBuffer(m_ctx, CL_MEM_READ_WRITE, sizeof(uint32_t) * 2);

    // Scratchpad chunk plan. Each work-item needs ceil(N / LOOKUP_GAP)
    // checkpoints of 8 uint4 (128 bytes each, the shared per-work-unit
    // footprint helper), and no single cl_mem may exceed
    // CL_DEVICE_MAX_MEM_ALLOC_SIZE, so the generator's intensity is split into
    // a flat table of chunks, planned and allocated here in whole work-groups.
    const uint32_t lookupGap  = data().scryptchacha_lookup_gap;
    const size_t groupBytes   = scrypt_chacha::perWorkUnitScratchpadBytes(lookupGap) * m_workGroupSize;

    const auto maxGroupsPerChunk = static_cast<uint32_t>(data().device.maxMemAllocSize() / groupBytes);
    if (maxGroupsPerChunk == 0) {
        LOG_ERR("%s" RED(" scrypt-chacha: one work-group of scratchpad (") RED_BOLD("%zu MiB") RED(") exceeds the device's maximum allocation size"),
                ocl_tag(), groupBytes / (1024 * 1024));

        throw std::runtime_error("scrypt-chacha scratchpad does not fit the device");
    }

    // Launch size in work-groups. A config thread normally carries the
    // intensity (autotune-generated or user-pinned); intensity 0 is the auto
    // marker: size the launch here, at init, from this thread's effective
    // tuning (per-GPU knobs merged over the globals by OclConfig::get()) and
    // the current memory state. This is the OpenCL analogue of the CUDA
    // plugin autotuning a -1/-1 launch config at runner init: a hand-written
    // per-device entry pins only the knobs it cares about and the launch
    // size follows the full merged set.
    uint32_t numberGroups = m_intensity / static_cast<uint32_t>(m_workGroupSize);
    if (numberGroups == 0) {
        constexpr uint64_t oneMiB = 1024 * 1024;

        const uint64_t vramGroups = vramBudgetBytes() / groupBytes;
        const uint64_t hostGroups = data().scryptchacha_use_system_ram
            ? static_cast<uint64_t>(data().scryptchacha_host_ram_budget_mb) * oneMiB / groupBytes
            : 0;

        numberGroups = static_cast<uint32_t>(vramGroups + hostGroups);

        LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha auto intensity: " CYAN_BOLD("%" PRIu64) " VRAM + " CYAN_BOLD("%" PRIu64) " host work-group%s, intensity " CYAN_BOLD("%u"),
                    ocl_tag(), data().device.index(), vramGroups, hostGroups, numberGroups > 1 ? "s" : "",
                    numberGroups * static_cast<uint32_t>(m_workGroupSize));
    }

    allocateScratchpads(numberGroups, maxGroupsPerChunk, groupBytes);

    // Build options for the generated entry point. The per-chunk work-item
    // counts are compile-time defines the selection ladder compares against;
    // SCRYPT_CHACHA_GENERATED_SEARCH84 suppresses the base source's default
    // single-buffer search84 in favour of the appended generated one.
    m_options = "-D SCRYPT_CHACHA_GENERATED_SEARCH84=1";
    m_options += " -D LOOKUP_GAP=" + std::to_string(lookupGap);
    m_options += " -D WORKSIZE="   + std::to_string(m_workGroupSize);

    for (size_t i = 0; i < m_chunkThreads.size(); ++i) {
        m_options += " -D THREADS_PER_BUFFER_" + std::to_string(i) + "=" + std::to_string(m_chunkThreads[i]);
    }

    m_generatedSource = OclBaseRunner::source();
    m_generatedSource += generateSearch84();
}


// VRAM budget available to scratchpad chunks, matching the generator's probe
// order: the AMD free-memory extension reports memory that is currently free
// (display and other clients already subtracted), so it needs no margin; the
// fallback only knows total VRAM, so it keeps the donor's 200 MB margin for
// everything else living on the card. The effective reserve_vram comes off
// either way. Shared by the auto-intensity sizing and the VRAM/host split so
// both see the same figure.
uint64_t xmrig::OclScryptChachaRunner::vramBudgetBytes() const
{
    constexpr uint64_t oneMiB          = 1024 * 1024;
    constexpr uint64_t fallbackReserve = 200ull * oneMiB;

    uint64_t budget = data().device.freeMemSizeAmd();
    if (budget == 0) {
        const uint64_t total = data().device.globalMemSize();
        budget = total > fallbackReserve ? total - fallbackReserve : 0;
    }

    const uint64_t reserveBytes = static_cast<uint64_t>(data().scryptchacha_reserve_vram_mb) * oneMiB;
    return budget > reserveBytes ? budget - reserveBytes : 0;
}


// Builds the flat scratchpad chunk table for numberGroups work-groups: host
// (system-RAM) chunks first, then VRAM chunks. VRAM-only mining plans every
// group as a VRAM chunk and lets the back-off find the real ceiling. With
// use_system_ram the split mirrors the donor's initCl: probe the VRAM budget
// (vramBudgetBytes(), the same probe the generator sizes against), fill VRAM
// to that capacity and spill the remaining groups into host chunks, capped by
// this GPU's host-RAM share. Host chunks go to the FRONT of the table so the
// generated selection ladder hands the low relative ids to the slow
// host-mapped work, letting it overlap with the fast VRAM work (the donor
// kernel's dispatch order).
void xmrig::OclScryptChachaRunner::allocateScratchpads(uint32_t numberGroups, uint32_t maxGroupsPerChunk, size_t groupBytes)
{
    constexpr uint64_t oneMiB = 1024 * 1024;

    uint32_t vramGroups = numberGroups;
    uint32_t hostGroups = 0;

    if (data().scryptchacha_use_system_ram) {
        const uint64_t vramCapacityGroups = vramBudgetBytes() / groupBytes;
        if (vramCapacityGroups < vramGroups) {
            vramGroups = static_cast<uint32_t>(vramCapacityGroups);
            hostGroups = numberGroups - vramGroups;

            // The donor disables the GPU when the overflow exceeds the host
            // budget. Here the reduced launch stays honest through
            // roundSize(), so clamping to what the budget backs is safer
            // than dropping the device.
            const uint64_t hostBudgetGroups = static_cast<uint64_t>(data().scryptchacha_host_ram_budget_mb) * oneMiB / groupBytes;
            if (hostGroups > hostBudgetGroups) {
                LOG_WARN("%s" YELLOW(" scrypt-chacha: host_ram_budget ") YELLOW_BOLD("%u MiB") YELLOW(" backs ") YELLOW_BOLD("%" PRIu64) YELLOW(" of the ") YELLOW_BOLD("%u") YELLOW(" overflow work-groups, clamping"),
                         ocl_tag(), data().scryptchacha_host_ram_budget_mb, hostBudgetGroups, hostGroups);

                hostGroups = static_cast<uint32_t>(hostBudgetGroups);
            }
        }
    }

    // VRAM chunk set with back-off. On a partial allocation the first failure
    // is the authoritative VRAM ceiling: keep the chunks that fit and launch
    // what they back. If not even the first chunk fits, halve the group count
    // and re-plan. The host groups stay as budgeted either way.
    std::vector<cl_mem> vramChunks;
    std::vector<uint32_t> vramThreads;

    while (vramGroups > 0) {
        const uint32_t allocated = allocateChunkSet(vramGroups, maxGroupsPerChunk, groupBytes, CL_MEM_READ_WRITE, vramChunks, vramThreads);
        if (allocated == vramGroups) {
            break;
        }

        if (allocated > 0) {
            LOG_WARN("%s" YELLOW(" scrypt-chacha: VRAM allocation backed off, ") YELLOW_BOLD("%u") YELLOW(" of ") YELLOW_BOLD("%u") YELLOW(" VRAM work-groups allocated"),
                     ocl_tag(), allocated, vramGroups);

            vramGroups = allocated;
            break;
        }

        vramGroups /= 2;

        if (vramGroups > 0) {
            LOG_WARN("%s" YELLOW(" scrypt-chacha: VRAM allocation failed outright, retrying with ") YELLOW_BOLD("%u") YELLOW(" work-groups"),
                     ocl_tag(), vramGroups);
        }
    }

    // Host chunk set: sized by the budget, no halve-and-retry (a failure here
    // means host RAM ran out, and a smaller ask is whatever did fit).
    std::vector<cl_mem> hostChunks;
    std::vector<uint32_t> hostThreads;

    if (hostGroups > 0) {
        const uint32_t allocated = allocateChunkSet(hostGroups, maxGroupsPerChunk, groupBytes, CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, hostChunks, hostThreads);
        if (allocated < hostGroups) {
            LOG_WARN("%s" YELLOW(" scrypt-chacha: host RAM allocation backed off, ") YELLOW_BOLD("%u") YELLOW(" of ") YELLOW_BOLD("%u") YELLOW(" host work-groups allocated"),
                     ocl_tag(), allocated, hostGroups);

            hostGroups = allocated;
        }
    }

    // Assemble the table: host chunks first, then VRAM chunks.
    m_hostChunkCount = hostChunks.size();
    m_scratchpads    = std::move(hostChunks);
    m_scratchpads.insert(m_scratchpads.end(), vramChunks.begin(), vramChunks.end());
    m_chunkThreads   = std::move(hostThreads);
    m_chunkThreads.insert(m_chunkThreads.end(), vramThreads.begin(), vramThreads.end());

    if (m_scratchpads.empty()) {
        throw std::runtime_error("scrypt-chacha: not enough memory for one work-group of scratchpad");
    }

    m_launchedIntensity = (vramGroups + hostGroups) * static_cast<uint32_t>(m_workGroupSize);

    if (m_launchedIntensity < m_intensity) {
        LOG_WARN("%s" YELLOW(" scrypt-chacha: launching intensity ") YELLOW_BOLD("%u") YELLOW(" of the configured ") YELLOW_BOLD("%u"),
                 ocl_tag(), m_launchedIntensity, m_intensity);
    }

    // Allocated split behind the launch, kept for the backend's launch table
    // (printed once every worker is ready).
    m_ramScratchpadBytes = static_cast<uint64_t>(hostGroups) * groupBytes;
    m_vramScratchpadBytes = static_cast<uint64_t>(vramGroups) * groupBytes;

    const uint64_t totalBytes = m_ramScratchpadBytes + m_vramScratchpadBytes;

    // Authoritative post-back-off allocation summary. One skeleton shared with
    // the CUDA backend's ready line ("N work units, VRAM X MiB + RAM Y MiB
    // (T MiB total)"), followed by this backend's native geometry: intensity
    // and the chunk split. A VRAM-only launch prints RAM 0 MiB and 0 host
    // chunks so the shape is fixed for log parsers.
    LOG_INFO("%s" CYAN_BOLD(" #%u") " scrypt-chacha ready: " CYAN_BOLD("%u") " work units, VRAM " CYAN_BOLD("%" PRIu64 " MiB") " + RAM " CYAN_BOLD("%" PRIu64 " MiB") " (" CYAN_BOLD("%" PRIu64 " MiB") " total), intensity " CYAN_BOLD("%u") ", " CYAN_BOLD("%zu") " VRAM + " CYAN_BOLD("%zu") " host chunk%s",
             ocl_tag(), data().device.index(),
             m_launchedIntensity,
             m_vramScratchpadBytes / oneMiB, m_ramScratchpadBytes / oneMiB, totalBytes / oneMiB,
             m_launchedIntensity,
             m_scratchpads.size() - m_hostChunkCount, m_hostChunkCount,
             m_scratchpads.size() > 1 ? "s" : "");
}


// Plans a balanced chunk set for numberGroups work-groups and allocates it,
// appending every chunk that fits to the caller's vectors. The layout follows
// the donor's configure_vram_padbuffers / configure_ram_padbuffers: the
// groups are spread evenly across the minimum number of chunks, so no chunk
// sits right at the single-allocation limit. maxGroupsPerChunk applies to
// host chunks too: CL_MEM_ALLOC_HOST_PTR buffers are still cl_mem objects
// bound by CL_DEVICE_MAX_MEM_ALLOC_SIZE, and with no cap on the chunk count
// there is no pressure to exceed it. Each chunk is committed with a one-word
// write (OpenCL defers the physical allocation to first use, host-resident
// buffers included, so a bare clCreateBuffer can succeed on memory the system
// cannot actually provide). Stops at the first chunk that fails and returns
// the number of groups the appended chunks back; the caller decides what a
// shortfall means.
uint32_t xmrig::OclScryptChachaRunner::allocateChunkSet(uint32_t numberGroups, uint32_t maxGroupsPerChunk, size_t groupBytes, cl_mem_flags flags, std::vector<cl_mem> &chunks, std::vector<uint32_t> &chunkThreads)
{
    const uint32_t chunkCount = (numberGroups + maxGroupsPerChunk - 1) / maxGroupsPerChunk;

    std::vector<uint32_t> plan(chunkCount);
    uint32_t remaining = numberGroups;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint32_t chunksLeft = chunkCount - i;
        uint32_t assign = remaining / chunksLeft + (remaining % chunksLeft > 0 ? 1 : 0);
        if (assign > maxGroupsPerChunk) {
            assign = maxGroupsPerChunk;
        }

        plan[i]    = assign;
        remaining -= assign;
    }

    uint32_t allocatedGroups = 0;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        cl_int ret   = CL_SUCCESS;
        cl_mem chunk = OclLib::createBuffer(m_ctx, flags, plan[i] * groupBytes, nullptr, &ret);
        if (!chunk) {
            break;
        }

        // Touch the chunk so the driver commits it now, surfacing an
        // out-of-memory condition here where the back-off can respond
        // instead of at the first kernel launch.
        const uint32_t zero = 0;
        if (OclLib::enqueueWriteBuffer(m_queue, chunk, CL_TRUE, 0, sizeof(zero), &zero, 0, nullptr, nullptr) != CL_SUCCESS) {
            OclLib::release(chunk);
            break;
        }

        chunks.push_back(chunk);
        chunkThreads.push_back(plan[i] * static_cast<uint32_t>(m_workGroupSize));
        allocatedGroups += plan[i];
    }

    return allocatedGroups;
}


// Generates the search84 entry point for the actual chunk table: the donor's
// explicit padcacheN arguments (OpenCL 1.2, no SVM) and the selection ladder
// that maps a work-item's batch-relative id onto its chunk, but produced for
// the exact chunk count instead of a preprocessor ladder with a fixed cap.
// The generated kernel does the mapping and hands off to search84_core in the
// base source.
std::string xmrig::OclScryptChachaRunner::generateSearch84() const
{
    std::string args;
    for (size_t i = 0; i < m_scratchpads.size(); ++i) {
        args += ",\n\t__global uchar * restrict padcache" + std::to_string(i);
    }

    // The ladder compares against running sums of the preceding chunks' thread
    // counts and rebases relative_gid into the selected chunk, exactly like the
    // donor's fixed-count ladder.
    std::string ladder;
    std::string prefixSum;
    for (size_t i = 0; i < m_scratchpads.size(); ++i) {
        const std::string threads = "THREADS_PER_BUFFER_" + std::to_string(i);
        const std::string bound   = prefixSum.empty() ? threads : prefixSum + " + " + threads;

        ladder += prefixSum.empty() ? "\tif" : "\telse if";
        ladder += " (relative_gid < " + bound + ") {\n";
        ladder += "\t\tpadcache = padcache" + std::to_string(i) + ";\n";
        ladder += "\t\tbuffer_xSIZE = " + threads + ";\n";
        if (!prefixSum.empty()) {
            ladder += "\t\trelative_gid -= (" + prefixSum + ");\n";
        }
        ladder += "\t}\n";

        prefixSum = bound;
    }

    return "\n"
        "__attribute__((reqd_work_group_size(WORKSIZE, 1, 1)))\n"
        "__kernel void search84(__global const uint4 * restrict input,\n"
        "\tvolatile __global uint * restrict output,\n"
        "\tvolatile __global uint * restrict stop" + args + ",\n"
        "\tconst uint target)\n"
        "{\n"
        "\t// Determine which padbuffer to use based on relative thread ID\n"
        "\t// Calculate relative gid within the work batch first\n"
        "\t// This works correctly with global_work_offset because get_group_id/get_local_id\n"
        "\t// are relative to the work batch, not absolute\n"
        "\tuint relative_gid = get_group_id(0) * WORKSIZE + get_local_id(0);\n"
        "\t__global uchar * padcache = (__global uchar *)0;\n"
        "\tuint buffer_xSIZE = 0;\n"
        "\n"
        + ladder +
        "\n"
        "\tsearch84_core(input, output, stop, padcache, relative_gid, buffer_xSIZE, target);\n"
        "}\n";
}


void xmrig::OclScryptChachaRunner::build()
{
    OclBaseRunner::build();

    m_searchKernel = OclLib::createKernel(m_program, "search84");
}


void xmrig::OclScryptChachaRunner::set(const Job &job, uint8_t *blob)
{
    m_blob = blob;

    // Prefilter target: the top 32-bit word of the 256-bit target. rawTarget32()
    // is 32-byte little-endian, so its most-significant word is the little-endian
    // uint32 at offset 28. The kernel keeps a candidate when output_hash[7] (the
    // hash's top big-endian word) is <= this value.
    memcpy(&m_target, job.rawTarget32() + 28, sizeof(m_target));

    // The argument indices past the fixed input/output/stop trio shift with the
    // chunk count: one padcacheN per chunk, then the target.
    OclLib::setKernelArg(m_searchKernel, 0, sizeof(cl_mem), &m_input);
    OclLib::setKernelArg(m_searchKernel, 1, sizeof(cl_mem), &m_output);
    OclLib::setKernelArg(m_searchKernel, 2, sizeof(cl_mem), &m_stop);

    for (size_t i = 0; i < m_scratchpads.size(); ++i) {
        OclLib::setKernelArg(m_searchKernel, static_cast<cl_uint>(3 + i), sizeof(cl_mem), &m_scratchpads[i]);
    }

    OclLib::setKernelArg(m_searchKernel, static_cast<cl_uint>(3 + m_scratchpads.size()), sizeof(cl_uint), &m_target);

    enqueueWriteBuffer(m_input, CL_TRUE, 0, SCRYPT_CHACHA_HEADER_SIZE, m_blob);
}


void xmrig::OclScryptChachaRunner::run(uint32_t nonce, uint32_t /*nonce_offset*/, uint32_t *hashOutput)
{
    const size_t local_work_size    = m_workGroupSize;
    const size_t global_work_offset = nonce;
    const size_t global_work_size   = m_launchedIntensity;

    enqueueWriteBuffer(m_input, CL_FALSE, 0, SCRYPT_CHACHA_HEADER_SIZE, m_blob);

    // Zero the candidate counter (output[0xFF]) and the stop buffer before the
    // launch. Nonces fill output[0..count-1], so only the counter needs clearing.
    const uint32_t zero[2] = {};
    enqueueWriteBuffer(m_output, CL_FALSE, 0xFF * sizeof(uint32_t), sizeof(uint32_t), zero);
    enqueueWriteBuffer(m_stop, CL_FALSE, 0, sizeof(uint32_t) * 2, zero);

    m_skippedHashes = 0;

    const cl_int ret = OclLib::enqueueNDRangeKernel(m_queue, m_searchKernel, 1, &global_work_offset, &global_work_size, &local_work_size, 0, nullptr, nullptr);
    if (ret != CL_SUCCESS) {
        LOG_ERR("%s" RED(" error ") RED_BOLD("%s") RED(" when calling ") RED_BOLD("clEnqueueNDRangeKernel") RED(" for kernel ") RED_BOLD("search84"),
            ocl_tag(), OclError::toString(ret));

        throw std::runtime_error(OclError::toString(ret));
    }

    uint32_t stop[2] = {};
    enqueueReadBuffer(m_stop, CL_FALSE, 0, sizeof(stop), stop);

    // finalize() blocks on the read-back, so it also syncs the launch. It reads
    // output[0..0xFF] into hashOutput and clamps hashOutput[0xFF] (the candidate
    // count) exactly as scrypt-chacha's SETFOUND convention expects.
    finalize(hashOutput);

    m_skippedHashes = stop[1] * static_cast<uint32_t>(m_workGroupSize);
}


void xmrig::OclScryptChachaRunner::jobEarlyNotification(const Job &)
{
    // Arm the stop flag on the control queue so it lands while the main queue is
    // still busy with the long kernel. The ROMix poll then aborts the in-flight
    // launch promptly instead of running to completion.
    const uint32_t one = 1;
    const cl_int ret = OclLib::enqueueWriteBuffer(m_controlQueue, m_stop, CL_TRUE, 0, sizeof(one), &one, 0, nullptr, nullptr);
    if (ret != CL_SUCCESS) {
        throw std::runtime_error(OclError::toString(ret));
    }
}
