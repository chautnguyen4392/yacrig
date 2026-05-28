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
#include "crypto/common/portable/mm_malloc.h"


namespace xmrig { namespace scrypt_chacha {


ScryptChachaCtx *createCtx(uint8_t *scratchpad)
{
    if (!scratchpad) {
        return nullptr;
    }

    auto *ctx = new ScryptChachaCtx;
    ctx->V    = scratchpad;
    ctx->YX   = static_cast<uint8_t *>(_mm_malloc(2 * kChunkBytes, 64));

    if (!ctx->YX) {
        releaseCtx(ctx);
        return nullptr;
    }

    return ctx;
}


void releaseCtx(ScryptChachaCtx *ctx)
{
    if (!ctx) {
        return;
    }

    // V is borrowed from m_memory and freed by the worker's destructor.
    _mm_free(ctx->YX);
    delete ctx;
}


}} /* namespace xmrig::scrypt_chacha */
