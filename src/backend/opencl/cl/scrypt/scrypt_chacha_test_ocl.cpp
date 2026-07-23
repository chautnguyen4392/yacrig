/* XMRig
 * Copyright (c) 2016-2026 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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

/* Standalone golden-vector harness for the scrypt-chacha OpenCL kernel.
 *
 * This binary is NOT part of the miner. It builds cl/scrypt/scrypt-chacha.cl on
 * a selected OpenCL GPU and checks the GPU hash against the golden vectors in
 * crypto/scrypt-chacha/scrypt-chacha_test.h (the same vectors the CPU kernel
 * test uses). It exercises two things:
 *
 *   1. The full 32-byte hash. The shipped search84 only emits the nonce of any
 *      candidate, so the harness appends a thin test_hash84 kernel that runs the
 *      identical pipeline (pbkdf2_128B_84 -> ROMix -> pbkdf2_32B_84) and writes
 *      the whole output_hash. It reuses the kernel's static helpers, so a hash
 *      mismatch points at the same crypto core search84 relies on.
 *
 *   2. The shipped search84 itself, launched with target == the golden hash's
 *      most-significant word so the known nonce must be reported in output[].
 *
 * It lists every OpenCL GPU it can see (across all platforms) and runs on one of
 * them. On a machine with both an integrated and a discrete GPU, pick the right
 * one with the OCL_DEVICE environment variable (the index from the printed list,
 * default 0).
 *
 * Build (needs an OpenCL ICD + a libOpenCL.so loader):
 *     cmake -DWITH_OPENCL=ON -DWITH_SCRYPT_CHACHA=ON ..
 *     make scrypt_chacha_test_ocl
 *     ./scrypt_chacha_test_ocl [path/to/scrypt-chacha.cl]
 *     OCL_DEVICE=1 ./scrypt_chacha_test_ocl        # run on the second GPU listed
 *     OCL_LOOKUP_GAP=32 ./scrypt_chacha_test_ocl   # smaller scratchpad, slower
 *
 * Prints "vector N: OK" per vector, exits 0 only if every vector matches.
 */

// The build normally inherits CL_TARGET_OPENCL_VERSION from the project's
// global OpenCL definitions; fall back to 1.2 (all this harness needs) when it
// is built on its own.
#ifndef CL_TARGET_OPENCL_VERSION
#   define CL_TARGET_OPENCL_VERSION 120
#endif

#include "3rdparty/cl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "crypto/scrypt-chacha/scrypt-chacha_test.h"


// Compile-time defaults supplied by cmake; argv[1] overrides the .cl path.
#ifndef SCRYPT_CHACHA_CL_PATH
#   define SCRYPT_CHACHA_CL_PATH "scrypt-chacha.cl"
#endif

// The kernel hard-codes N (scrypt cost). Mirror it here to size the scratchpad.
static constexpr uint32_t kN          = 4194304;   // 2^22
static constexpr uint32_t kWorksize   = 16;        // one work-group == one batch
static constexpr uint32_t kThreads    = kWorksize; // THREADS_PER_BUFFER_0

// LOOKUP_GAP is a pure scrypt time-memory tradeoff: it changes how many ROMix
// checkpoints are stored versus recomputed, never the final hash. A smaller gap
// stores more checkpoints (more scratchpad) and recomputes less (faster), so the
// harness defaults low for quick verification. Scratchpad grows as N/gap, so gap
// 4 needs ~2 GiB here vs ~256 MiB at gap 32. Override with OCL_LOOKUP_GAP.
static constexpr uint32_t kDefaultLookupGap = 4;


// The harness-only kernel that dumps the full 32-byte hash. It mirrors search84
// for the single-buffer case (relative_gid == tid, buffer_xSIZE == kThreads).
static const char *kTestKernel = R"CLC(

__attribute__((reqd_work_group_size(WORKSIZE, 1, 1)))
__kernel void test_hash84(__global const uint4 * restrict input,
                          __global uint * restrict hashout,
                          __global uchar * restrict padcache0,
                          volatile __global uint * restrict stop)
{
    uint4 password[6];
    uint4 X[8];
    uint output_hash[8] __attribute__ ((aligned (16)));
    const uint gid = get_global_id(0);
    const uint tid = get_group_id(0) * get_local_size(0) + get_local_id(0);

    password[0] = input[0];
    password[1] = input[1];
    password[2] = input[2];
    password[3] = input[3];
    password[4] = input[4];
    password[5] = input[5];
    password[5].x = gid;

    scrypt_pbkdf2_128B_84(password, password, X);

    // The harness never arms stop[0], so ROMix always runs to completion here.
    scrypt_ROMix(X, (__global uint4 *)padcache0, tid, THREADS_PER_BUFFER_0, stop);

    scrypt_pbkdf2_32B_84(password, X, (uint4 *)output_hash);

    const uint offset = tid * 8;
    #pragma unroll
    for (uint i = 0; i < 8; i++) {
        hashout[offset + i] = output_hash[i];
    }
}
)CLC";


static bool check(cl_int err, const char *what)
{
    if (err != CL_SUCCESS) {
        fprintf(stderr, "error: %s failed (%d)\n", what, err);
        return false;
    }
    return true;
}


static bool readFile(const char *path, std::string &out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<size_t>(size) : 0);
    const size_t got = out.empty() ? 0 : fread(&out[0], 1, out.size(), f);
    fclose(f);
    return got == out.size() && !out.empty();
}


int main(int argc, char **argv)
{
    const char *clPath = (argc > 1) ? argv[1] : SCRYPT_CHACHA_CL_PATH;

    std::string source;
    if (!readFile(clPath, source)) {
        return 1;
    }

    cl_int err = CL_SUCCESS;

    // Enumerate every GPU across all platforms, so an iGPU + discrete card (or a
    // multi-GPU rig) can be disambiguated. Pick one with OCL_DEVICE=<index>.
    cl_uint numPlatforms = 0;
    clGetPlatformIDs(0, nullptr, &numPlatforms);
    if (numPlatforms == 0) {
        fprintf(stderr, "error: no OpenCL platform found\n");
        return 1;
    }
    std::vector<cl_platform_id> platforms(numPlatforms);
    clGetPlatformIDs(numPlatforms, platforms.data(), nullptr);

    std::vector<cl_device_id> devices;
    for (cl_platform_id p : platforms) {
        cl_uint n = 0;
        if (clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 0, nullptr, &n) != CL_SUCCESS || n == 0) {
            continue;
        }
        std::vector<cl_device_id> d(n);
        clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, n, d.data(), nullptr);
        devices.insert(devices.end(), d.begin(), d.end());
    }
    if (devices.empty()) {
        fprintf(stderr, "error: no OpenCL GPU device found\n");
        return 1;
    }

    printf("OpenCL GPU devices:\n");
    for (size_t i = 0; i < devices.size(); ++i) {
        char nm[256] = { 0 };
        clGetDeviceInfo(devices[i], CL_DEVICE_NAME, sizeof(nm), nm, nullptr);
        printf("  [%zu] %s\n", i, nm);
    }

    size_t sel = 0;
    if (const char *e = getenv("OCL_DEVICE")) {
        sel = strtoul(e, nullptr, 10);
    }
    if (sel >= devices.size()) {
        fprintf(stderr, "error: OCL_DEVICE=%zu is out of range (%zu device(s))\n", sel, devices.size());
        return 1;
    }

    cl_device_id device = devices[sel];
    char devName[256] = { 0 };
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(devName), devName, nullptr);
    printf("using device [%zu]: %s\n", sel, devName);

    uint32_t lookupGap = kDefaultLookupGap;
    if (const char *e = getenv("OCL_LOOKUP_GAP")) {
        const unsigned long g = strtoul(e, nullptr, 10);
        if (g >= 1) {
            lookupGap = static_cast<uint32_t>(g);
        }
    }

    cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    if (!check(err, "clCreateContext")) {
        return 1;
    }

    cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);
    if (!check(err, "clCreateCommandQueue")) {
        return 1;
    }

    // Two source strings: the shipped kernel + the harness-only hash dumper.
    const char *strings[2] = { source.c_str(), kTestKernel };
    const size_t lengths[2] = { source.size(), strlen(kTestKernel) };
    cl_program program = clCreateProgramWithSource(ctx, 2, strings, lengths, &err);
    if (!check(err, "clCreateProgramWithSource")) {
        return 1;
    }

    char options[256];
    snprintf(options, sizeof(options),
             "-D NUM_PADBUFFERS=1 -D NUM_PADBUFFERS_RAM=0 -D LOOKUP_GAP=%u -D WORKSIZE=%u -D THREADS_PER_BUFFER_0=%u",
             lookupGap, kWorksize, kThreads);

    if (clBuildProgram(program, 1, &device, options, nullptr, nullptr) != CL_SUCCESS) {
        size_t logSize = 0;
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logSize);
        std::string log(logSize, '\0');
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, logSize, &log[0], nullptr);
        fprintf(stderr, "clBuildProgram failed:\n%s\n", log.c_str());
        return 1;
    }

    cl_kernel kTest   = clCreateKernel(program, "test_hash84", &err);
    if (!check(err, "clCreateKernel(test_hash84)")) {
        return 1;
    }
    cl_kernel kSearch = clCreateKernel(program, "search84", &err);
    if (!check(err, "clCreateKernel(search84)")) {
        return 1;
    }

    // Scratchpad: THREADS_PER_BUFFER_0 threads, each ceil(N/LOOKUP_GAP) * 8 uint4.
    const size_t ySize       = (kN + lookupGap - 1) / lookupGap;
    const size_t padBytes    = static_cast<size_t>(kThreads) * ySize * 8 * sizeof(cl_uint4);
    printf("lookup_gap %u, scratchpad %zu MiB\n", lookupGap, padBytes / (1024 * 1024));
    const size_t inputBytes  = 6 * sizeof(cl_uint4);            // 96 bytes (84 used)
    const size_t outputCount = 0x100;                           // search84 nonce buffer
    const size_t hashCount   = static_cast<size_t>(kThreads) * 8;

    cl_mem input  = clCreateBuffer(ctx, CL_MEM_READ_ONLY,  inputBytes, nullptr, &err);
    if (!check(err, "clCreateBuffer(input)")) { return 1; }
    cl_mem output = clCreateBuffer(ctx, CL_MEM_READ_WRITE, outputCount * sizeof(cl_uint), nullptr, &err);
    if (!check(err, "clCreateBuffer(output)")) { return 1; }
    cl_mem hashes = clCreateBuffer(ctx, CL_MEM_READ_WRITE, hashCount * sizeof(cl_uint), nullptr, &err);
    if (!check(err, "clCreateBuffer(hashes)")) { return 1; }
    cl_mem padcache = clCreateBuffer(ctx, CL_MEM_READ_WRITE, padBytes, nullptr, &err);
    if (!check(err, "clCreateBuffer(padcache)")) {
        fprintf(stderr, "  (scratchpad request was %zu MiB)\n", padBytes / (1024 * 1024));
        return 1;
    }

    // Two-word early-abort buffer (stop[0] = abort flag, stop[1] = skipped-group
    // count). The harness leaves it zeroed so every launch runs to completion.
    cl_mem stopbuf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, 2 * sizeof(cl_uint), nullptr, &err);
    if (!check(err, "clCreateBuffer(stop)")) { return 1; }
    const cl_uint stopZero[2] = { 0, 0 };
    clEnqueueWriteBuffer(queue, stopbuf, CL_TRUE, 0, sizeof(stopZero), stopZero, 0, nullptr, nullptr);

    using namespace xmrig::scrypt_chacha;

    const size_t gws = kWorksize;
    const size_t lws = kWorksize;
    int failures = 0;

    for (size_t v = 0; v < kTestVectorCount; ++v) {
        const ScryptChachaTestVector &vec = kTestVectors[v];

        // Upload the 84-byte header into a zero-padded 96-byte input.
        uint8_t header96[96] = { 0 };
        memcpy(header96, vec.header, sizeof(vec.header));
        if (!check(clEnqueueWriteBuffer(queue, input, CL_TRUE, 0, inputBytes, header96, 0, nullptr, nullptr),
                   "write input")) {
            return 1;
        }

        // The nonce lives at header bytes 80..83 (little-endian); search84 sets
        // password[5].x = get_global_id(0), so the matching work-item is the one
        // whose global id equals this nonce. Launch the batch starting there.
        uint32_t nonce = 0;
        memcpy(&nonce, vec.header + 80, sizeof(nonce));
        const size_t offset = nonce;

        // --- Full-hash check via test_hash84 -----------------------------------
        clSetKernelArg(kTest, 0, sizeof(cl_mem), &input);
        clSetKernelArg(kTest, 1, sizeof(cl_mem), &hashes);
        clSetKernelArg(kTest, 2, sizeof(cl_mem), &padcache);
        clSetKernelArg(kTest, 3, sizeof(cl_mem), &stopbuf);
        if (!check(clEnqueueNDRangeKernel(queue, kTest, 1, &offset, &gws, &lws, 0, nullptr, nullptr),
                   "enqueue test_hash84")) {
            return 1;
        }
        clFinish(queue);

        std::vector<cl_uint> hashOut(hashCount);
        if (!check(clEnqueueReadBuffer(queue, hashes, CL_TRUE, 0, hashCount * sizeof(cl_uint), hashOut.data(), 0, nullptr, nullptr),
                   "read hashes")) {
            return 1;
        }

        // The matching work-item is local id 0 (global id == nonce == offset),
        // so its tid is 0 and its hash sits at hashOut[0..7]. Those eight words
        // are little-endian wire order; reverse the 32 bytes to display order.
        const uint8_t *wire = reinterpret_cast<const uint8_t *>(hashOut.data());
        uint8_t display[32];
        for (int b = 0; b < 32; ++b) {
            display[b] = wire[31 - b];
        }
        const bool hashOk = memcmp(display, vec.hash, 32) == 0;

        // --- search84 nonce check ----------------------------------------------
        // target == the golden hash's top (display) word == output_hash[7] for
        // the golden nonce, so search84's prefilter accepts it (<= comparison).
        const cl_uint target = (static_cast<cl_uint>(vec.hash[0]) << 24) |
                               (static_cast<cl_uint>(vec.hash[1]) << 16) |
                               (static_cast<cl_uint>(vec.hash[2]) << 8)  |
                                static_cast<cl_uint>(vec.hash[3]);

        std::vector<cl_uint> zero(outputCount, 0);
        clEnqueueWriteBuffer(queue, output, CL_TRUE, 0, outputCount * sizeof(cl_uint), zero.data(), 0, nullptr, nullptr);

        clSetKernelArg(kSearch, 0, sizeof(cl_mem), &input);
        clSetKernelArg(kSearch, 1, sizeof(cl_mem), &output);
        clSetKernelArg(kSearch, 2, sizeof(cl_mem), &stopbuf);
        clSetKernelArg(kSearch, 3, sizeof(cl_mem), &padcache);
        clSetKernelArg(kSearch, 4, sizeof(cl_uint), &target);
        if (!check(clEnqueueNDRangeKernel(queue, kSearch, 1, &offset, &gws, &lws, 0, nullptr, nullptr),
                   "enqueue search84")) {
            return 1;
        }
        clFinish(queue);

        std::vector<cl_uint> found(outputCount);
        if (!check(clEnqueueReadBuffer(queue, output, CL_TRUE, 0, outputCount * sizeof(cl_uint), found.data(), 0, nullptr, nullptr),
                   "read output")) {
            return 1;
        }

        cl_uint count = found[0xFF];
        if (count > 0xFF) {
            count = 0xFF;
        }
        bool nonceOk = false;
        for (cl_uint i = 0; i < count; ++i) {
            if (found[i] == nonce) {
                nonceOk = true;
                break;
            }
        }

        if (hashOk && nonceOk) {
            printf("vector %zu: OK\n", v + 1);
        }
        else {
            ++failures;
            printf("vector %zu: FAIL (hash %s, search84 nonce %s)\n",
                   v + 1, hashOk ? "OK" : "mismatch", nonceOk ? "OK" : "missing");
            char got[65];
            for (int b = 0; b < 32; ++b) {
                snprintf(got + b * 2, 3, "%02x", display[b]);
            }
            printf("  got hash:  %s\n", got);
            for (int b = 0; b < 32; ++b) {
                snprintf(got + b * 2, 3, "%02x", vec.hash[b]);
            }
            printf("  want hash: %s\n", got);
        }
    }

    clReleaseMemObject(stopbuf);
    clReleaseMemObject(padcache);
    clReleaseMemObject(hashes);
    clReleaseMemObject(output);
    clReleaseMemObject(input);
    clReleaseKernel(kSearch);
    clReleaseKernel(kTest);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);

    if (failures == 0) {
        printf("all %zu vectors OK\n", kTestVectorCount);
        return 0;
    }
    printf("%d vector(s) FAILED\n", failures);
    return 1;
}
