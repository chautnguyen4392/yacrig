/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2018-2024 SChernykh   <https://github.com/SChernykh>
 * Copyright 2016-2024 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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

#include <cstdio>
#include <uv.h>

#ifdef XMRIG_FEATURE_TLS
#   include <openssl/opensslv.h>
#endif

#ifdef XMRIG_FEATURE_HWLOC
#   include <hwloc.h>
#endif

#ifdef XMRIG_FEATURE_OPENCL
#   include "backend/opencl/wrappers/OclLib.h"
#   include "backend/opencl/wrappers/OclPlatform.h"
#endif

#ifdef XMRIG_FEATURE_CUDA
#   include "backend/cuda/wrappers/CudaDevice.h"
#   include "backend/cuda/wrappers/CudaLib.h"
#endif

#include "base/kernel/Entry.h"
#include "base/kernel/Process.h"
#include "core/config/usage.h"
#include "version.h"


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
#   include "crypto/scrypt-chacha/ScryptChachaCtx.h"
#   include "crypto/scrypt-chacha/scrypt-chacha.h"
#   include "crypto/scrypt-chacha/scrypt-chacha_test.h"
#   include <cstdlib>
#   include <cstring>
#endif


namespace xmrig {


static int showVersion()
{
    printf(APP_NAME " " APP_VERSION "\n built on " __DATE__

#   if defined(__clang__)
    " with clang " __clang_version__);
#   elif defined(__GNUC__)
    " with GCC");
    printf(" %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#   elif defined(_MSC_VER)
    " with MSVC");
    printf(" %d", MSVC_VERSION);
#   else
    );
#   endif

    printf("\n features:"
#   if defined(__x86_64__) || defined(_M_AMD64) || defined (__arm64__) || defined (__aarch64__)
    " 64-bit"
#   else
    " 32-bit"
#   endif

#   if defined(__AES__) || defined(_MSC_VER) || defined(__ARM_FEATURE_CRYPTO)
    " AES"
#   endif
    "\n");

    printf("\nlibuv/%s\n", uv_version_string());

#   if defined(XMRIG_FEATURE_TLS)
    {
#       if defined(LIBRESSL_VERSION_TEXT)
        printf("LibreSSL/%s\n", LIBRESSL_VERSION_TEXT + 9);
#       elif defined(OPENSSL_VERSION_TEXT)
        constexpr const char *v = &OPENSSL_VERSION_TEXT[8];
        printf("OpenSSL/%.*s\n", static_cast<int>(strchr(v, ' ') - v), v);
#       endif
    }
#   endif

#   if defined(XMRIG_FEATURE_HWLOC)
#   if defined(HWLOC_VERSION)
    printf("hwloc/%s\n", HWLOC_VERSION);
#   elif HWLOC_API_VERSION >= 0x20000
    printf("hwloc/2\n");
#   else
    printf("hwloc/1\n");
#   endif
#   endif

    return 0;
}


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
/* Standalone golden-vector check for the scrypt-chacha CPU kernel. Mirrors
 * tests/scrypt_chacha_test.cpp but runs from the main xmrig binary so users
 * can validate the kernel on the host without building a separate executable.
 * Allocates ~512 MiB transiently and exits — never returns to the App path. */
static int runScryptChachaTest()
{
    using xmrig::scrypt_chacha::createCtx;
    using xmrig::scrypt_chacha::hash;
    using xmrig::scrypt_chacha::kOutputSize;
    using xmrig::scrypt_chacha::kScratchpadBytes;
    using xmrig::scrypt_chacha::kTestVectorCount;
    using xmrig::scrypt_chacha::kTestVectors;
    using xmrig::scrypt_chacha::releaseCtx;

    void *scratchpad = nullptr;
    if (posix_memalign(&scratchpad, 64, kScratchpadBytes) != 0) {
        std::fprintf(stderr, "scrypt-chacha-test: failed to allocate scratchpad (need ~512 MiB)\n");
        return 1;
    }

    auto *ctx = createCtx(static_cast<uint8_t *>(scratchpad));
    if (!ctx) {
        std::fprintf(stderr, "scrypt-chacha-test: createCtx failed\n");
        std::free(scratchpad);
        return 1;
    }

    int failed = 0;

    for (size_t i = 0; i < kTestVectorCount; i++) {
        const auto &v = kTestVectors[i];
        uint8_t out[kOutputSize] = {};

        hash(v.header, sizeof(v.header), out, ctx, 0);

        /* Kernel emits little-endian wire bytes; golden vector is in big-endian
         * display order. Reverse before comparing. */
        uint8_t out_be[kOutputSize];
        for (size_t b = 0; b < kOutputSize; b++) {
            out_be[b] = out[kOutputSize - 1 - b];
        }

        const bool ok = (std::memcmp(out_be, v.hash, kOutputSize) == 0);
        std::printf("vector %zu: %s\n", i + 1, ok ? "OK" : "FAIL");

        if (!ok) {
            std::printf("  expected: ");
            for (size_t b = 0; b < kOutputSize; b++) std::printf("%02x", v.hash[b]);
            std::printf("\n  got:      ");
            for (size_t b = 0; b < kOutputSize; b++) std::printf("%02x", out_be[b]);
            std::printf("\n");
            failed++;
        }
    }

    releaseCtx(ctx);
    std::free(scratchpad);

    if (failed) {
        std::fprintf(stderr, "scrypt-chacha-test: %d/%zu vector(s) failed\n",
                     failed, kTestVectorCount);
        return 1;
    }

    std::printf("scrypt-chacha-test: all %zu vector(s) passed\n", kTestVectorCount);
    return 0;
}
#endif


#ifdef XMRIG_FEATURE_HWLOC
static int exportTopology(const Process &)
{
    const String path = Process::location(Process::ExeLocation, "topology.xml");

    hwloc_topology_t topology = nullptr;
    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);

#   if HWLOC_API_VERSION >= 0x20000
    if (hwloc_topology_export_xml(topology, path, 0) == -1) {
#   else
    if (hwloc_topology_export_xml(topology, path) == -1) {
#   endif
        printf("failed to export hwloc topology.\n");
    }
    else {
        printf("hwloc topology successfully exported to \"%s\"\n", path.data());
    }

    hwloc_topology_destroy(topology);

    return 0;
}
#endif


#if defined(XMRIG_FEATURE_OPENCL) || defined(XMRIG_FEATURE_CUDA)
// Prints every GPU each compiled-in backend can see, then exits. Output goes
// through printf like the other early-exit commands: the log subsystem is not
// up yet at this point.
static int printDevices(const Process &process)
{
    const Arguments &args = process.arguments();

#   ifdef XMRIG_FEATURE_OPENCL
    // An explicit --opencl-loader is honored so the same loader resolution
    // applies as in a mining run; value() returns nullptr when the option is
    // absent, which selects the default loader.
    if (OclLib::init(args.value("--opencl-loader"))) {
        OclPlatform::printDevices();
    }
    else {
        printf("%-28s0 (failed to load the OpenCL runtime)\n\n", "Number of OpenCL platforms:");
    }
#   endif

#   ifdef XMRIG_FEATURE_CUDA
    if (CudaLib::init(args.value("--cuda-loader"))) {
        constexpr size_t oneMiB = 1024 * 1024;
        const auto devices = CudaLib::devices(0, 0, {});

        printf("%-28s%s/%s/%s\n", "CUDA driver/runtime/plugin:",
               CudaLib::version(CudaLib::driverVersion()).c_str(),
               CudaLib::version(CudaLib::runtimeVersion()).c_str(),
               CudaLib::pluginVersion());
        printf("%-28s%zu\n\n", "Number of CUDA devices:", devices.size());

        for (const auto &device : devices) {
            printf("  %-26s%u\n",           "Index:",               device.index());
            printf("  %-26s%s\n",           "Name:",                device.name().data());
            printf("  %-26s%s\n",           "Bus ID:",              device.topology().toString().data());
            printf("  %-26s%u.%u\n",        "Compute capability:",  device.computeCapability(true), device.computeCapability(false));
            printf("  %-26s%u/%u MHz\n",    "Clock (core/memory):", device.clock(), device.memoryClock());
            printf("  %-26s%u\n",           "SMX:",                 device.smx());
            printf("  %-26s%zu/%zu MB\n\n", "Memory (free/total):", device.freeMemSize() / oneMiB, device.globalMemSize() / oneMiB);
        }
    }
    else {
        printf("%-28s0 (%s)\n\n", "Number of CUDA devices:", CudaLib::lastError());
    }
#   endif

    return 0;
}
#endif


} // namespace xmrig


xmrig::Entry::Id xmrig::Entry::get(const Process &process)
{
    const Arguments &args = process.arguments();
    if (args.hasArg("-h") || args.hasArg("--help")) {
         return Usage;
    }

    if (args.hasArg("-V") || args.hasArg("--version") || args.hasArg("--versions")) {
         return Version;
    }

#   ifdef XMRIG_FEATURE_HWLOC
    if (args.hasArg("--export-topology")) {
        return Topo;
    }
#   endif

#   ifdef XMRIG_FEATURE_OPENCL
    if (args.hasArg("--print-platforms")) {
        return Platforms;
    }
#   endif

#   if defined(XMRIG_FEATURE_OPENCL) || defined(XMRIG_FEATURE_CUDA)
    if (args.hasArg("--print-devices")) {
        return Devices;
    }
#   endif

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    if (args.hasArg("--scrypt-chacha-test")) {
        return ScryptChachaTest;
    }
#   endif

    return Default;
}


int xmrig::Entry::exec(const Process &process, Id id)
{
    switch (id) {
    case Usage:
        printf("%s\n", usage().c_str());
        return 0;

    case Version:
        return showVersion();

#   ifdef XMRIG_FEATURE_HWLOC
    case Topo:
        return exportTopology(process);
#   endif

#   ifdef XMRIG_FEATURE_OPENCL
    case Platforms:
        if (OclLib::init()) {
            OclPlatform::print();
        }
        return 0;
#   endif

#   if defined(XMRIG_FEATURE_OPENCL) || defined(XMRIG_FEATURE_CUDA)
    case Devices:
        return printDevices(process);
#   endif

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    case ScryptChachaTest:
        return runScryptChachaTest();
#   endif

    default:
        break;
    }

    return 1;
}
