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

#ifndef XMRIG_SCRYPT_CHACHA_JANE_H
#define XMRIG_SCRYPT_CHACHA_JANE_H


#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


/* Run scrypt-jane configured for SCRYPT_CHACHA + SCRYPT_KECCAK512 (the YAC
 * variant) over (password, salt) and write `bytes` of output into `out`.
 *
 * Working buffers must be supplied by the caller (ScryptChachaCtx allocates
 * them once via VirtualMemory and reuses them across nonces):
 *   V_buffer  : N * chunk_bytes        bytes, 64-byte aligned
 *   YX_buffer : (p + 1) * chunk_bytes  bytes, 64-byte aligned
 *
 * Where:
 *   N           = 1 << (Nfactor + 1)
 *   r           = 1 << rfactor
 *   p           = 1 << pfactor
 *   chunk_bytes = 64 * r * 2           (chacha block is 64 bytes)
 */
void xmrig_scrypt_chacha_run(const uint8_t *password, size_t password_len,
                             const uint8_t *salt, size_t salt_len,
                             uint8_t Nfactor, uint8_t rfactor, uint8_t pfactor,
                             uint8_t *out, size_t bytes,
                             uint8_t *V_buffer, uint8_t *YX_buffer);


#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* XMRIG_SCRYPT_CHACHA_JANE_H */
