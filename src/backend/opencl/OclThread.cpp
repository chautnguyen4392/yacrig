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

#include "backend/opencl/OclThread.h"
#include "3rdparty/rapidjson/document.h"
#include "base/io/json/Json.h"


#include <algorithm>


namespace xmrig {

static const char *kIndex        = "index";
static const char *kIntensity    = "intensity";
static const char *kStridedIndex = "strided_index";
static const char *kThreads      = "threads";
static const char *kUnroll       = "unroll";
static const char *kWorksize     = "worksize";

#ifdef XMRIG_ALGO_RANDOMX
static const char *kBFactor      = "bfactor";
static const char *kGCNAsm       = "gcn_asm";
static const char* kDatasetHost  = "dataset_host";
#endif

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
static const char *kLookupGap       = "lookup_gap";
static const char *kUseSystemRam    = "use_system_ram";
static const char *kReserveVramMb   = "reserve_vram_mb";
static const char *kHostRamBudgetMb = "host_ram_budget_mb";
#endif

} // namespace xmrig


xmrig::OclThread::OclThread(const rapidjson::Value &value)
{
    if (!value.IsObject()) {
        return;
    }

    m_index         = Json::getUint(value, kIndex);
    m_worksize      = std::max(std::min(Json::getUint(value, kWorksize), 512U), 1U);
    m_unrollFactor  = std::max(std::min(Json::getUint(value, kUnroll, m_unrollFactor), 128U), 1U);

    setIntensity(Json::getUint(value, kIntensity));

    const auto &si = Json::getArray(value, kStridedIndex);
    if (si.IsArray() && si.Size() >= 2) {
        m_stridedIndex = std::min(si[0].GetUint(), 2U);
        m_memChunk     = std::min(si[1].GetUint(), 18U);
    }
    else {
        m_stridedIndex = 0;
        m_memChunk     = 0;
        m_fields.set(STRIDED_INDEX_FIELD, false);
    }

    const auto &threads = Json::getArray(value, kThreads);
    if (threads.IsArray()) {
        m_threads.reserve(threads.Size());

        for (const auto &affinity : threads.GetArray()) {
            m_threads.emplace_back(affinity.GetInt64());
        }
    }

    if (m_threads.empty()) {
        m_threads.emplace_back(-1);
    }

#   ifdef XMRIG_ALGO_RANDOMX
    const auto &gcnAsm = Json::getValue(value, kGCNAsm);
    if (gcnAsm.IsBool()) {
        m_fields.set(RANDOMX_FIELDS, true);

        m_gcnAsm      = gcnAsm.GetBool();
        m_bfactor     = Json::getUint(value, kBFactor, m_bfactor);
        m_datasetHost = Json::getBool(value, kDatasetHost, m_datasetHost);
    }
#   endif

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Each knob is pinned independently: a key that is present sets its has*
    // discriminator, a key that is absent leaves the knob to OclConfig's
    // global default when the launch data is built.
    m_hasWorksize = Json::getValue(value, kWorksize).IsUint();

    const auto &lookupGap = Json::getValue(value, kLookupGap);
    if (lookupGap.IsUint()) {
        m_fields.set(SCRYPT_CHACHA_FIELDS, true);
        m_hasLookupGap = true;
        m_lookupGap    = lookupGap.GetUint();
    }

    const auto &useSystemRam = Json::getValue(value, kUseSystemRam);
    if (useSystemRam.IsBool()) {
        m_fields.set(SCRYPT_CHACHA_FIELDS, true);
        m_hasUseSystemRam = true;
        m_useSystemRam    = useSystemRam.GetBool();
    }

    const auto &reserveVramMb = Json::getValue(value, kReserveVramMb);
    if (reserveVramMb.IsUint()) {
        m_fields.set(SCRYPT_CHACHA_FIELDS, true);
        m_hasReserveVramMb = true;
        m_reserveVramMb    = reserveVramMb.GetUint();
    }

    const auto &hostRamBudgetMb = Json::getValue(value, kHostRamBudgetMb);
    if (hostRamBudgetMb.IsUint()) {
        m_fields.set(SCRYPT_CHACHA_FIELDS, true);
        m_hasHostRamBudgetMb = true;
        m_hostRamBudgetMb    = hostRamBudgetMb.GetUint();
    }
#   endif
}


bool xmrig::OclThread::isEqual(const OclThread &other) const
{
    return other.m_threads.size() == m_threads.size() &&
           std::equal(m_threads.begin(), m_threads.end(), other.m_threads.begin()) &&
           other.m_bfactor      == m_bfactor &&
           other.m_datasetHost  == m_datasetHost &&
           other.m_gcnAsm       == m_gcnAsm &&
           other.m_index        == m_index &&
           other.m_intensity    == m_intensity &&
           other.m_memChunk     == m_memChunk &&
           other.m_stridedIndex == m_stridedIndex &&
           other.m_unrollFactor == m_unrollFactor &&
           other.m_worksize     == m_worksize &&
           other.m_hasLookupGap       == m_hasLookupGap &&
           other.m_hasWorksize        == m_hasWorksize &&
           other.m_hasUseSystemRam    == m_hasUseSystemRam &&
           other.m_hasReserveVramMb   == m_hasReserveVramMb &&
           other.m_hasHostRamBudgetMb == m_hasHostRamBudgetMb &&
           other.m_useSystemRam == m_useSystemRam &&
           other.m_lookupGap    == m_lookupGap &&
           other.m_reserveVramMb   == m_reserveVramMb &&
           other.m_hostRamBudgetMb == m_hostRamBudgetMb;
}


rapidjson::Value xmrig::OclThread::toJSON(rapidjson::Document &doc) const
{
    using namespace rapidjson;
    auto &allocator = doc.GetAllocator();

    Value out(kObjectType);

    out.AddMember(StringRef(kIndex),        index(), allocator);
    out.AddMember(StringRef(kIntensity),    intensity(), allocator);
    out.AddMember(StringRef(kWorksize),     worksize(), allocator);

    if (m_fields.test(STRIDED_INDEX_FIELD)) {
        Value si(kArrayType);
        si.Reserve(2, allocator);
        si.PushBack(stridedIndex(), allocator);
        si.PushBack(memChunk(), allocator);
        out.AddMember(StringRef(kStridedIndex), si, allocator);
    }

    Value threads(kArrayType);
    threads.Reserve(m_threads.size(), allocator);

    for (auto thread : m_threads) {
        threads.PushBack(thread, allocator);
    }

    out.AddMember(StringRef(kThreads), threads, allocator);

    if (m_fields.test(RANDOMX_FIELDS)) {
#       ifdef XMRIG_ALGO_RANDOMX
        out.AddMember(StringRef(kBFactor),      bfactor(), allocator);
        out.AddMember(StringRef(kGCNAsm),       isAsm(), allocator);
        out.AddMember(StringRef(kDatasetHost),  isDatasetHost(), allocator);
#       endif
    }
    else if (m_fields.test(SCRYPT_CHACHA_FIELDS)) {
#       ifdef XMRIG_ALGO_SCRYPT_CHACHA
        // Only pinned knobs are written back, so an autotune-generated thread
        // keeps following the global "opencl" settings after a config save.
        if (m_hasLookupGap)       { out.AddMember(StringRef(kLookupGap),       lookupGap(),       allocator); }
        if (m_hasUseSystemRam)    { out.AddMember(StringRef(kUseSystemRam),    useSystemRam(),    allocator); }
        if (m_hasReserveVramMb)   { out.AddMember(StringRef(kReserveVramMb),   reserveVramMb(),   allocator); }
        if (m_hasHostRamBudgetMb) { out.AddMember(StringRef(kHostRamBudgetMb), hostRamBudgetMb(), allocator); }
#       endif
    }
    else if (!m_fields.test(KAWPOW_FIELDS)) {
        out.AddMember(StringRef(kUnroll), unrollFactor(), allocator);
    }

    return out;
}
