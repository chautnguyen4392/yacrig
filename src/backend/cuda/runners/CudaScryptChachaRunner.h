/* XMRig
 * Copyright (c) 2016-2025 XMRig <https://github.com/xmrig>, <support@xmrig.com>
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
 */

#ifndef XMRIG_CUDASCRYPTCHACHARUNNER_H
#define XMRIG_CUDASCRYPTCHACHARUNNER_H


#include "backend/cuda/runners/CudaBaseRunner.h"


namespace xmrig {


class CudaScryptChachaRunner : public CudaBaseRunner
{
public:
    CudaScryptChachaRunner(size_t index, const CudaLaunchData &data);

    // Final launch geometry and allocated scratchpad split, valid once init()
    // has run (covers auto geometry and any back-off). The backend reads
    // these through CudaWorker for the launch table it prints when every
    // worker is ready.
    inline uint32_t launchThreads() const       { return m_launchThreads; }
    inline uint32_t launchBlocks() const        { return m_launchBlocks; }
    inline uint64_t vramScratchpadBytes() const { return m_vramScratchpadBytes; }
    inline uint64_t ramScratchpadBytes() const  { return m_ramScratchpadBytes; }

protected:
    bool init() override;
    void configureCtx() override;
    bool set(const Job &job, uint8_t *blob) override;
    bool run(uint32_t startNonce, uint32_t *rescount, uint32_t *resnonce) override;
    size_t intensity() const override     { return m_workUnits > 0 ? m_workUnits : CudaBaseRunner::intensity(); }
    size_t roundSize() const override     { return intensity(); }
    // Work units the last launch actually processed: a coarse early-abort
    // reports the whole launch as skipped, so this returns 0 and CudaWorker
    // treats that launch as a no-op (no count, no nonce advance, no log).
    size_t processedHashes() const override { return intensity() - m_skippedHashes; }
    void jobEarlyNotification(const Job&) override;

private:
    uint8_t *m_jobBlob       = nullptr;
    uint32_t m_workUnits     = 0;
    uint32_t m_skippedHashes = 0;

    uint32_t m_launchThreads        = 0;
    uint32_t m_launchBlocks         = 0;
    uint64_t m_vramScratchpadBytes  = 0;
    uint64_t m_ramScratchpadBytes   = 0;
};


} /* namespace xmrig */


#endif // XMRIG_CUDASCRYPTCHACHARUNNER_H
