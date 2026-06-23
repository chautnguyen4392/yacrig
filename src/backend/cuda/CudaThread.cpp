/* XMRig
 * Copyright (c) 2018-2021 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2021 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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

#include "backend/cuda/CudaThread.h"
#include "3rdparty/rapidjson/document.h"
#include "backend/cuda/wrappers/CudaLib.h"
#include "base/io/json/Json.h"


#include <algorithm>


namespace xmrig {

static const char *kAffinity    = "affinity";
static const char *kBFactor     = "bfactor";
static const char *kBlocks      = "blocks";
static const char *kBSleep      = "bsleep";
static const char *kIndex       = "index";
static const char *kThreads     = "threads";
static const char *kDatasetHost = "dataset_host";

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
static const char *kLookupGap        = "lookup_gap";
static const char *kUseSystemRam     = "use_system_ram";
static const char *kReserveVramMb    = "reserve_vram_mb";
static const char *kHostRamBudgetMb  = "host_ram_budget_mb";
#endif

} // namespace xmrig


xmrig::CudaThread::CudaThread(const rapidjson::Value &value)
{
    if (!value.IsObject()) {
        return;
    }

    m_index     = Json::getUint(value, kIndex);
    m_threads   = Json::getInt(value, kThreads);
    m_blocks    = Json::getInt(value, kBlocks);
    m_bfactor   = std::min(Json::getUint(value, kBFactor, m_bfactor), 12U);
    m_bsleep    = Json::getUint(value, kBSleep, m_bsleep);
    m_affinity  = Json::getUint64(value, kAffinity, m_affinity);

    if (Json::getValue(value, kDatasetHost).IsInt()) {
        m_datasetHost = Json::getInt(value, kDatasetHost, m_datasetHost) != 0;
    }
    else {
        m_datasetHost = Json::getBool(value, kDatasetHost);
    }

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    {
        const auto &v = Json::getValue(value, kLookupGap);
        if (v.IsInt())  { m_has_lookup_gap = true; m_lookup_gap = v.GetInt(); }
    }
    {
        const auto &v = Json::getValue(value, kUseSystemRam);
        if (v.IsBool()) { m_has_use_system_ram = true; m_use_system_ram = v.GetBool(); }
    }
    {
        const auto &v = Json::getValue(value, kReserveVramMb);
        if (v.IsInt())  { m_has_reserve_vram_mb = true; m_reserve_vram_mb = v.GetInt(); }
    }
    {
        const auto &v = Json::getValue(value, kHostRamBudgetMb);
        if (v.IsInt())  { m_has_host_ram_budget_mb = true; m_host_ram_budget_mb = v.GetInt(); }
    }
#   endif
}


xmrig::CudaThread::CudaThread(uint32_t index, nvid_ctx *ctx) :
    m_blocks(CudaLib::deviceInt(ctx, CudaLib::DeviceBlocks)),
    m_datasetHost(CudaLib::deviceInt(ctx, CudaLib::DeviceDatasetHost)),
    m_threads(CudaLib::deviceInt(ctx, CudaLib::DeviceThreads)),
    m_index(index),
    m_bfactor(CudaLib::deviceUint(ctx, CudaLib::DeviceBFactor)),
    m_bsleep(CudaLib::deviceUint(ctx, CudaLib::DeviceBSleep))
{
#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // The plugin autotune stashed the host-mapped warp count on the ctx; read it
    // back here while the autotune ctx is still alive. 0 for non-scrypt families.
    m_scryptChachaRamWarps = CudaLib::deviceUint(ctx, CudaLib::DeviceScryptChachaRamWarps);
#   endif
}


bool xmrig::CudaThread::isEqual(const CudaThread &other) const
{
    return m_blocks      == other.m_blocks &&
           m_threads     == other.m_threads &&
           m_affinity    == other.m_affinity &&
           m_index       == other.m_index &&
           m_bfactor     == other.m_bfactor &&
           m_bsleep      == other.m_bsleep &&
           m_datasetHost == other.m_datasetHost;
}


rapidjson::Value xmrig::CudaThread::toJSON(rapidjson::Document &doc) const
{
    using namespace rapidjson;
    auto &allocator = doc.GetAllocator();

    Value out(kObjectType);

    out.AddMember(StringRef(kIndex),        index(), allocator);
    out.AddMember(StringRef(kThreads),      threads(), allocator);
    out.AddMember(StringRef(kBlocks),       blocks(), allocator);
    out.AddMember(StringRef(kBFactor),      bfactor(), allocator);
    out.AddMember(StringRef(kBSleep),       bsleep(), allocator);
    out.AddMember(StringRef(kAffinity),     affinity(), allocator);

    if (m_datasetHost >= 0) {
        out.AddMember(StringRef(kDatasetHost), m_datasetHost > 0, allocator);
    }

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    if (m_has_lookup_gap)         { out.AddMember(StringRef(kLookupGap),        m_lookup_gap,         allocator); }
    if (m_has_use_system_ram)     { out.AddMember(StringRef(kUseSystemRam),     m_use_system_ram,     allocator); }
    if (m_has_reserve_vram_mb)    { out.AddMember(StringRef(kReserveVramMb),    m_reserve_vram_mb,    allocator); }
    if (m_has_host_ram_budget_mb) { out.AddMember(StringRef(kHostRamBudgetMb),  m_host_ram_budget_mb, allocator); }
#   endif

    return out;
}
