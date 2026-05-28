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

#include "crypto/scrypt-chacha/ScryptChachaCtx.h"
#include "crypto/scrypt-chacha/scrypt-chacha.h"
#include "crypto/scrypt-chacha/scrypt-chacha_test.h"


#include <cstdio>
#include <cstdlib>
#include <cstring>


using xmrig::scrypt_chacha::createCtx;
using xmrig::scrypt_chacha::hash;
using xmrig::scrypt_chacha::kOutputSize;
using xmrig::scrypt_chacha::kScratchpadBytes;
using xmrig::scrypt_chacha::kTestVectorCount;
using xmrig::scrypt_chacha::kTestVectors;
using xmrig::scrypt_chacha::releaseCtx;


static void printHex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        std::printf("%02x", data[i]);
    }
}


int main()
{
    /* The full xmrig build hands the scratchpad in from CpuWorker::m_memory
     * (a huge-page-aware VirtualMemory). The standalone test doesn't link
     * VirtualMemory, so allocate a plain aligned heap region instead. */
    void *scratchpad = nullptr;
    if (posix_memalign(&scratchpad, 64, kScratchpadBytes) != 0) {
        std::fprintf(stderr, "scrypt_chacha_test: failed to allocate scratchpad (need ~512 MiB)\n");
        return 1;
    }

    auto *ctx = createCtx(static_cast<uint8_t *>(scratchpad));
    if (!ctx) {
        std::fprintf(stderr, "scrypt_chacha_test: createCtx failed\n");
        std::free(scratchpad);
        return 1;
    }

    int failed = 0;

    for (size_t i = 0; i < kTestVectorCount; i++) {
        const auto &v = kTestVectors[i];
        uint8_t out[kOutputSize] = {};

        hash(v.header, sizeof(v.header), out, ctx, 0);

        /* The kernel emits little-endian wire bytes; the golden vector is in
         * big-endian display order. Reverse before comparing. */
        uint8_t out_be[kOutputSize];
        for (size_t b = 0; b < kOutputSize; b++) {
            out_be[b] = out[kOutputSize - 1 - b];
        }

        const bool ok = (std::memcmp(out_be, v.hash, kOutputSize) == 0);
        std::printf("vector %zu: %s\n", i + 1, ok ? "OK" : "FAIL");

        if (!ok) {
            std::printf("  expected: ");
            printHex(v.hash, kOutputSize);
            std::printf("\n  got:      ");
            printHex(out_be, kOutputSize);
            std::printf("\n");
            failed++;
        }
    }

    releaseCtx(ctx);
    std::free(scratchpad);

    if (failed) {
        std::fprintf(stderr, "scrypt_chacha_test: %d/%zu vector(s) failed\n",
                     failed, kTestVectorCount);
        return 1;
    }

    std::printf("scrypt_chacha_test: all %zu vector(s) passed\n", kTestVectorCount);
    return 0;
}
