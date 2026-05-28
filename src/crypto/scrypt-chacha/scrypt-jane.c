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
 *
 * --
 *
 * This file vendors a subset of scrypt-jane (public domain / MIT, by
 * Andrew Moon, https://github.com/floodyberry/scrypt-jane) configured for the
 * Yacoin scrypt-chacha variant: SCRYPT_CHACHA mixer + SCRYPT_KECCAK512 hash.
 *
 * Modifications vs upstream scrypt-jane.c:
 *   - SCRYPT_TEST is always defined so the upstream power-on self-test is
 *     skipped; we run our own golden-vector test on a real YAC block header.
 *   - The public entry point is renamed to xmrig_scrypt_chacha_run() and
 *     takes caller-allocated V and YX buffers instead of malloc'ing per call.
 *     The 512 MiB scratchpad is allocated once (huge-page aware, via
 *     VirtualMemory) inside ScryptChachaCtx and reused for every nonce; the
 *     algorithm itself stays bit-for-bit identical to upstream.
 */

#define SCRYPT_TEST
#define SCRYPT_CHACHA
#define SCRYPT_KECCAK512
#define SCRYPT_CHOOSE_RUNTIME

#include <string.h>

#include "crypto/scrypt-chacha/scrypt-jane.h"


/* The vendored scrypt-jane headers ship a handful of test helpers (used by
 * scrypt_power_on_self_test in upstream) that are unused in our static-link
 * configuration, plus an unused vendor-detection variable in detect_cpu().
 * Silence the noise. */
#if defined(__GNUC__) || defined(__clang__)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wunused-function"
#   pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif


#include "crypto/scrypt-chacha/code/scrypt-jane-portable.h"
#include "crypto/scrypt-chacha/code/scrypt-jane-hash.h"
#include "crypto/scrypt-chacha/code/scrypt-jane-romix.h"


#if defined(__GNUC__) || defined(__clang__)
#   pragma GCC diagnostic pop
#endif


void
xmrig_scrypt_chacha_run(const uint8_t *password, size_t password_len,
                        const uint8_t *salt, size_t salt_len,
                        uint8_t Nfactor, uint8_t rfactor, uint8_t pfactor,
                        uint8_t *out, size_t bytes,
                        uint8_t *V_buffer, uint8_t *YX_buffer)
{
    uint8_t *X;
    uint8_t *Y;
    uint32_t N;
    uint32_t r;
    uint32_t p;
    uint32_t chunk_bytes;
    uint32_t i;

    scrypt_ROMixfn scrypt_ROMix = scrypt_getROMix();

    N = (1u << (Nfactor + 1));
    r = (1u << rfactor);
    p = (1u << pfactor);

    chunk_bytes = SCRYPT_BLOCK_BYTES * r * 2u;

    /* YX_buffer layout: Y (chunk_bytes) | X (p * chunk_bytes) */
    Y = YX_buffer;
    X = Y + chunk_bytes;

    /* 1: X = PBKDF2(password, salt) */
    scrypt_pbkdf2(password, password_len, salt, salt_len, 1, X, chunk_bytes * p);

    /* 2: X = ROMix(X) */
    for (i = 0; i < p; i++) {
        scrypt_ROMix((scrypt_mix_word_t *)(X + (chunk_bytes * i)),
                     (scrypt_mix_word_t *)Y,
                     (scrypt_mix_word_t *)V_buffer,
                     N, r);
    }

    /* 3: Out = PBKDF2(password, X) */
    scrypt_pbkdf2(password, password_len, X, chunk_bytes * p, 1, out, bytes);

    scrypt_ensure_zero(YX_buffer, (p + 1) * chunk_bytes);
}
