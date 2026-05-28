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

#ifndef XMRIG_SCRYPT_CHACHA_CTX_H
#define XMRIG_SCRYPT_CHACHA_CTX_H


#include <cstdint>


namespace xmrig { namespace scrypt_chacha {


/* Per-worker working buffers for scrypt-chacha.
 *
 * The 512 MiB ROMix scratchpad (V) is owned by CpuWorker::m_memory; this
 * struct just borrows the pointer. YX is the tiny chunk-mix scratch
 * ((p + 1) * chunk_bytes = 256 B at r = 1, p = 1) and is owned here.
 */
struct ScryptChachaCtx
{
    uint8_t *V  = nullptr;   // borrowed from CpuWorker::m_memory->scratchpad()
    uint8_t *YX = nullptr;   // owned, 256 B aligned heap
};


/* `scratchpad` points at the worker's pre-allocated 512 MiB region
 * (CpuWorker::m_memory->scratchpad()). The context borrows this pointer for
 * the lifetime of the worker; it does NOT take ownership. */
ScryptChachaCtx *createCtx(uint8_t *scratchpad);


/* Release a context previously returned by createCtx(). Safe to call with
 * nullptr. Does not free V (owned by the worker). */
void releaseCtx(ScryptChachaCtx *ctx);


}} /* namespace xmrig::scrypt_chacha */


#endif /* XMRIG_SCRYPT_CHACHA_CTX_H */
