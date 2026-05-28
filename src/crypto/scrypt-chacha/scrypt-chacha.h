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

#ifndef XMRIG_SCRYPT_CHACHA_H
#define XMRIG_SCRYPT_CHACHA_H


#include <cstddef>
#include <cstdint>


namespace xmrig { namespace scrypt_chacha {


// YAC scrypt-chacha parameters (nFactor = 21, r = 1, p = 1).
// Values match the ones produced by yacoind's scrypt-jane configuration with
// SCRYPT_CHACHA (ChaCha20/8 mixer) and SCRYPT_KECCAK512 (SHA-3/Keccak-512 hash).
static constexpr uint8_t  kNFactor         = 21;
static constexpr uint64_t kN               = 1ULL << (kNFactor + 1);   // 4 194 304
static constexpr uint32_t kR               = 1;
static constexpr uint32_t kP               = 1;
static constexpr size_t   kBlockBytes      = 64;                       // ChaCha block
static constexpr size_t   kChunkBytes      = 2 * kR * kBlockBytes;     // 128 B
// scrypt-jane requires V_buffer of exactly N * chunk_bytes. The YX_buffer
// ((p + 1) * chunk_bytes = 256 B at p = 1) is owned by ScryptChachaCtx and
// allocated separately via _mm_malloc -- it does NOT belong in the
// huge-pages-backed scratchpad. Sizing V correctly here gives exactly
// 256 huge pages per worker (2^22 * 128 B = 512 MiB, on a 2 MiB boundary).
static constexpr size_t   kScratchpadBytes = kN * kChunkBytes;         // 512 MiB
static constexpr size_t   kHeaderBytes     = 84;                       // YAC v7+ block header
static constexpr size_t   kOutputSize      = 32;


// Forward declaration; the concrete layout is added together with the
// real ROMix-based implementation in a later step.
struct ScryptChachaCtx;


// Compute scrypt-chacha hash of `blob` (size bytes) into `out32` (32 bytes).
// Signature is compatible with xmrig's existing cn_hash_fun so the same CPU
// dispatch path can call it. `height` is unused and kept for signature parity.
void hash(const uint8_t *blob, size_t size, uint8_t *out32,
          ScryptChachaCtx *ctx, uint64_t height);


}} // namespace xmrig::scrypt_chacha


#endif /* XMRIG_SCRYPT_CHACHA_H */
