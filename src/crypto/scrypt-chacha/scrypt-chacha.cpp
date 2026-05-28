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

#include "crypto/scrypt-chacha/scrypt-chacha.h"
#include "crypto/scrypt-chacha/ScryptChachaCtx.h"
#include "crypto/scrypt-chacha/scrypt-jane.h"


void xmrig::scrypt_chacha::hash(const uint8_t *blob, size_t size,
                                uint8_t *out32,
                                ScryptChachaCtx *ctx, uint64_t /*height*/)
{
    /* YAC scrypt-chacha parameters: nFactor = 21 (N = 2^22), r = 1, p = 1.
     * The block header is used as both password and salt. */
    xmrig_scrypt_chacha_run(blob, size, blob, size,
                            kNFactor, /*rfactor=*/0, /*pfactor=*/0,
                            out32, kOutputSize,
                            ctx->V, ctx->YX);
}
