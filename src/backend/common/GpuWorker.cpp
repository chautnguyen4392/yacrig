/* XMRig
 * Copyright (c) 2018-2020 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2020 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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


#include "backend/common/GpuWorker.h"
#include "base/tools/Chrono.h"

#include "base/io/log/Log.h"


namespace xmrig {


// Work units to fold into the scrypt-chacha projection for `elapsedMs` of a
// launch whose estimated total duration is `estMs`: the [0, 1]-clamped elapsed
// fraction times the launch's work-unit count. estMs == 0 (no measured rate
// yet) contributes nothing, and the clamp saturates the ramp at the full
// work-unit count once a launch overruns its estimate. Single source for the
// fraction math shared by advanceProjection, commitAbortedProjection, and
// hashrateData.
static inline uint64_t projectionRamp(uint64_t elapsedMs, uint64_t estMs, uint64_t workUnits)
{
    if (estMs == 0) {
        return 0;
    }

    double fraction = static_cast<double>(elapsedMs) / static_cast<double>(estMs);
    if (fraction > 1.0) {
        fraction = 1.0;
    }

    return static_cast<uint64_t>(fraction * static_cast<double>(workUnits));
}


} // namespace xmrig


xmrig::GpuWorker::GpuWorker(size_t id, int64_t affinity, int priority, uint32_t deviceIndex) : Worker(id, affinity, priority),
    m_deviceIndex(deviceIndex)
{
}


void xmrig::GpuWorker::storeStats()
{
    // Get index which is unused now
    const uint32_t index = m_index.load(std::memory_order_relaxed) ^ 1;

    // Fill in the data for that index
    m_hashCount[index] = m_count;
    m_timestamp[index] = Chrono::steadyMSecs();

    // Switch to that index
    // All data will be in memory by the time it completes thanks to std::memory_order_seq_cst
    m_index.fetch_xor(1, std::memory_order_seq_cst);
}


void xmrig::GpuWorker::hashrateData(uint64_t &hashCount, uint64_t &timeStamp, uint64_t &rawHashes) const
{
    uint64_t estMs, start, base, wu;
    {
        std::lock_guard<std::mutex> lock(m_projLock);
        estMs = m_projEstMs;
        start = m_projStart;
        base  = m_projBase;
        wu    = m_projWorkUnits;
    }

    // A scrypt-chacha launch processes its work units at a steady rate over a
    // duration that barely varies (memory-bound, constant work). Ramp the
    // continuous display count from the fraction of the previous launch's
    // duration that has elapsed, so the tick thread feeds the sampler a
    // growing count with a current timestamp ~twice a second instead of one
    // stale point per launch. base carries over from the previous launch's
    // end, so the ramp is continuous (no step, no over-read of the running
    // max). estMs stays 0 for every other algorithm family (they never call
    // the projection mutators) and for scrypt-chacha before its first launch
    // completes (no rate yet, so it reads n/a), and those cases fall through
    // to the frozen per-launch snapshot below.
    if (estMs != 0) {
        const uint64_t now       = Chrono::steadyMSecs();
        const uint64_t elapsed   = now > start ? now - start : 0;
        const uint64_t projected = base + projectionRamp(elapsed, estMs, wu);

        hashCount = projected;
        timeStamp = now;
        rawHashes = projected;

        return;
    }

    const uint32_t index = m_index.load(std::memory_order_relaxed);

    rawHashes = m_hashrateData.interpolate(timeStamp);
    hashCount = m_hashCount[index];
    timeStamp = m_timestamp[index];
}


// Anchor the projection at the start of a scrypt-chacha launch. m_projBase
// carries over from the previous launch's end (set in advanceProjection), so
// the ramp is continuous across the boundary.
void xmrig::GpuWorker::beginProjection(uint64_t launchStart)
{
    std::lock_guard<std::mutex> lock(m_projLock);
    m_projStart     = launchStart;
    m_projWorkUnits = intensity();
}


// Advance the display count to exactly where this launch's projection ended
// (no step), using the rate that was in effect during the launch (the
// previous launch's duration). The first launch has no prior duration, so it
// contributes nothing and the next launch ramps from 0.
void xmrig::GpuWorker::advanceProjection(uint64_t launchMs)
{
    std::lock_guard<std::mutex> lock(m_projLock);
    m_projBase += projectionRamp(launchMs, m_projEstMs, m_projWorkUnits);
    m_projEstMs = launchMs;                 // rate denominator for the next launch
    m_projStart = Chrono::steadyMSecs();    // park so a tick before the next launch sees frac ~ 0
}


// Fold the ramp an early-aborted launch already displayed into m_projBase so
// the display count never steps backwards. The projection had ramped the
// count up to m_projBase + frac * m_projWorkUnits while the aborted launch
// ran, but the launch did no work so advanceProjection is not called: without
// this, the retry launch would restart the ramp from the un-advanced base and
// the count would drop, which the sampler reads as an unsigned underflow (a
// colossal spurious hashrate that latches the running max). m_projEstMs is
// left untouched: an aborted launch's truncated duration is not a valid rate
// denominator.
void xmrig::GpuWorker::commitAbortedProjection()
{
    std::lock_guard<std::mutex> lock(m_projLock);
    if (m_projEstMs == 0) {
        return;
    }

    const uint64_t now     = Chrono::steadyMSecs();
    const uint64_t elapsed = now > m_projStart ? now - m_projStart : 0;
    m_projBase += projectionRamp(elapsed, m_projEstMs, m_projWorkUnits);
    m_projStart = now;                      // ramp folded into base, restart from here so a tick before the retry sees frac ~ 0
}


void xmrig::GpuWorker::logScryptChachaStart(const char *tag, const char *backend, uint32_t startNonce) const
{
    LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha %s processing nonces " CYAN_BOLD("%u") "-" CYAN_BOLD("%u")
             " (" CYAN_BOLD("%u") " work units)",
             tag, m_deviceIndex, backend, startNonce,
             static_cast<uint32_t>(startNonce + intensity() - 1), static_cast<uint32_t>(intensity()));
}


void xmrig::GpuWorker::logScryptChachaSkipped(const char *tag, const char *backend, uint32_t startNonce) const
{
    LOG_INFO("%s" CYAN_BOLD(" #%u") " scrypt-chacha %s skipped nonces " CYAN_BOLD("%u") "-" CYAN_BOLD("%u")
             " (early abort)",
             tag, m_deviceIndex, backend, startNonce,
             static_cast<uint32_t>(startNonce + intensity() - 1));
}


void xmrig::GpuWorker::logScryptChachaDone(const char *tag, const char *backend, uint32_t startNonce, uint64_t workUnits, uint64_t launchMs) const
{
    const double hr = launchMs > 0 ? static_cast<double>(workUnits) * 1000.0 / static_cast<double>(launchMs) : 0.0;
    LOG_INFO("%s" CYAN_BOLD(" #%u") " scrypt-chacha %s processed nonces " CYAN_BOLD("%u") "-" CYAN_BOLD("%u")
             " (" CYAN_BOLD("%u") " work units) in " CYAN_BOLD("%llu") " ms (" CYAN_BOLD("%.2f") " H/s)",
             tag, m_deviceIndex, backend, startNonce, static_cast<uint32_t>(startNonce + workUnits - 1),
             static_cast<uint32_t>(workUnits), static_cast<unsigned long long>(launchMs), hr);
}
