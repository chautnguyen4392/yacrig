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

#include <stdexcept>

#include "backend/opencl/OclConfig.h"
#include "3rdparty/rapidjson/document.h"
#include "backend/common/Tags.h"
#include "backend/opencl/OclConfig_gen.h"
#include "backend/opencl/wrappers/OclLib.h"
#include "base/io/json/Json.h"
#include "base/io/log/Log.h"

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
#   include "base/tools/HostMemoryInfo.h"
#   include <algorithm>
#endif


namespace xmrig {


static const char *kCache       = "cache";
static const char *kDevicesHint = "devices-hint";
static const char *kEnabled     = "enabled";
static const char *kLoader      = "loader";

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
static const char *kLookupGap       = "lookup_gap";
static const char *kWorksize        = "worksize";
static const char *kUseSystemRam    = "use_system_ram";
static const char *kReserveVramMb   = "reserve_vram_mb";
static const char *kReserveRamMb    = "reserve_ram_mb";
static const char *kHostRamBudgetMb = "host_ram_budget_mb";
#endif

#ifndef XMRIG_OS_APPLE
static const char *kAMD         = "AMD";
static const char *kINTEL       = "INTEL";
static const char *kNVIDIA      = "NVIDIA";
static const char *kPlatform    = "platform";
#endif

#ifdef XMRIG_FEATURE_ADL
static const char *kAdl         = "adl";
#endif


extern template class Threads<OclThreads>;


} // namespace xmrig


#ifndef XMRIG_OS_APPLE
xmrig::OclConfig::OclConfig() : m_platformVendor(kAMD) {}
#else
xmrig::OclConfig::OclConfig() = default;
#endif


xmrig::OclPlatform xmrig::OclConfig::platform() const
{
    const auto platforms = OclPlatform::get();
    if (platforms.empty()) {
        return {};
    }

#   ifndef XMRIG_OS_APPLE
    if (!m_platformVendor.isEmpty()) {
        String search;
        String vendor = m_platformVendor;
        vendor.toUpper();

        if (vendor == kAMD) {
            search = "Advanced Micro Devices";
        }
        else if (vendor == kNVIDIA) {
            search = kNVIDIA;
        }
        else if (vendor == kINTEL) {
            search = "Intel";
        }
        else {
            search = m_platformVendor;
        }

        for (const auto &platform : platforms) {
            if (platform.vendor().contains(search)) {
                return platform;
            }
        }
    }
    else if (m_platformIndex < platforms.size()) {
        return platforms[m_platformIndex];
    }

    return {};
#   else
    return platforms[0];
#   endif
}


rapidjson::Value xmrig::OclConfig::toJSON(rapidjson::Document &doc) const
{
    using namespace rapidjson;
    auto &allocator = doc.GetAllocator();

    Value obj(kObjectType);

    obj.AddMember(StringRef(kEnabled),  m_enabled, allocator);
    obj.AddMember(StringRef(kCache),    m_cache, allocator);
    obj.AddMember(StringRef(kLoader),   m_loader.toJSON(), allocator);

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    obj.AddMember(StringRef(kLookupGap),       m_lookupGap,       allocator);
    obj.AddMember(StringRef(kWorksize),        m_worksize,        allocator);
    obj.AddMember(StringRef(kUseSystemRam),    m_useSystemRam,    allocator);
    obj.AddMember(StringRef(kReserveVramMb),   m_reserveVramMb,   allocator);
    obj.AddMember(StringRef(kReserveRamMb),    m_reserveRamMb,    allocator);
    obj.AddMember(StringRef(kHostRamBudgetMb), m_hostRamBudgetMb, allocator);
#   endif

#   ifndef XMRIG_OS_APPLE
    obj.AddMember(StringRef(kPlatform), m_platformVendor.isEmpty() ? Value(m_platformIndex) : m_platformVendor.toJSON(), allocator);
#   endif

#   ifdef XMRIG_FEATURE_ADL
    obj.AddMember(StringRef(kAdl),      m_adl, allocator);
#   endif

    m_threads.toJSON(obj, doc);

    return obj;
}


std::vector<xmrig::OclLaunchData> xmrig::OclConfig::get(const Miner *miner, const Algorithm &algorithm, const OclPlatform &platform, const std::vector<OclDevice> &devices) const
{
    std::vector<OclLaunchData> out;
    const auto &threads = m_threads.get(algorithm);

    if (threads.isEmpty()) {
        return out;
    }

    out.reserve(threads.count() * 2);

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    // Resolve the effective per-GPU tuning: a knob pinned on the thread (JSON
    // per-device entry) wins over the global "opencl" field. The runner and
    // the backend log read these merged values, never the raw thread fields.
    // The unpinned host budget is the per-GPU split derived in
    // setupScryptChacha(); a pinned one is an absolute per-GPU amount.
    auto applyScryptChachaTuning = [this, &algorithm](OclLaunchData &launch, const OclThread &thread) {
        if (algorithm.family() != Algorithm::SCRYPT_CHACHA) {
            return;
        }

        launch.scryptchacha_lookup_gap         = std::max(thread.hasLookupGap() ? thread.lookupGap() : m_lookupGap, 1u);
        launch.scryptchacha_worksize           = std::max(std::min(thread.hasWorksize() ? thread.worksize() : m_worksize, 512u), 1u);
        launch.scryptchacha_use_system_ram     = thread.hasUseSystemRam() ? thread.useSystemRam() : m_useSystemRam;
        launch.scryptchacha_reserve_vram_mb    = thread.hasReserveVramMb() ? thread.reserveVramMb() : m_reserveVramMb;
        launch.scryptchacha_host_ram_budget_mb = thread.hasHostRamBudgetMb() ? thread.hostRamBudgetMb() : m_defaultHostRamBudgetMbPerGpu;
    };
#   endif

    for (const auto &thread : threads.data()) {
        if (thread.index() >= devices.size()) {
            LOG_INFO("%s" YELLOW(" skip non-existing device with index ") YELLOW_BOLD("%u"), ocl_tag(), thread.index());
            continue;
        }

        if (thread.threads().size() > 1) {
            for (int64_t affinity : thread.threads()) {
                out.emplace_back(miner, algorithm, *this, platform, thread, devices[thread.index()], affinity);

#               ifdef XMRIG_ALGO_SCRYPT_CHACHA
                applyScryptChachaTuning(out.back(), thread);
#               endif
            }
        }
        else {
            out.emplace_back(miner, algorithm, *this, platform, thread, devices[thread.index()], thread.threads().front());

#           ifdef XMRIG_ALGO_SCRYPT_CHACHA
            applyScryptChachaTuning(out.back(), thread);
#           endif
        }
    }

    return out;
}


void xmrig::OclConfig::read(const rapidjson::Value &value)
{
    if (value.IsObject()) {
        m_enabled   = Json::getBool(value, kEnabled, m_enabled);
        m_cache     = Json::getBool(value, kCache, m_cache);
        m_loader    = Json::getString(value, kLoader);

#       ifdef XMRIG_ALGO_SCRYPT_CHACHA
        m_lookupGap       = Json::getUint(value, kLookupGap,       m_lookupGap);
        m_worksize        = Json::getUint(value, kWorksize,        m_worksize);
        m_useSystemRam    = Json::getBool(value, kUseSystemRam,    m_useSystemRam);
        m_reserveVramMb   = Json::getUint(value, kReserveVramMb,   m_reserveVramMb);
        m_reserveRamMb    = Json::getUint(value, kReserveRamMb,    m_reserveRamMb);
        m_hostRamBudgetMb = Json::getUint(value, kHostRamBudgetMb, m_hostRamBudgetMb);
#       endif

#       ifndef XMRIG_OS_APPLE
        setPlatform(Json::getValue(value, kPlatform));
#       endif

        setDevicesHint(Json::getString(value, kDevicesHint));

#       ifdef XMRIG_FEATURE_ADL
        m_adl = Json::getBool(value, kAdl, m_adl);
#       endif

        m_threads.read(value);

#       ifdef XMRIG_ALGO_SCRYPT_CHACHA
        // Threads parse without profile context, so the OclThread JSON
        // constructor can set SCRYPT_CHACHA_FIELDS only from pinned knob
        // keys. Stamp the bit on every thread of the scrypt-chacha profile so
        // a knobless thread serializes through toJSON's scrypt-chacha branch
        // instead of the unroll fallback. Strict lookup: the named profile or
        // an explicit alias, never the "*" wildcard, which other algorithms
        // may share.
        auto *scryptChachaProfile = m_threads.profile(m_threads.profileName(Algorithm::SCRYPT_CHACHA_YAC, true));
        if (scryptChachaProfile != nullptr) {
            scryptChachaProfile->setScryptChachaFields();
        }
#       endif

        generate();
    }
    else if (value.IsBool()) {
        m_enabled = value.GetBool();

        generate();
    }
    else {
        m_shouldSave = true;

        generate();
    }
}


void xmrig::OclConfig::generate()
{
    if (!isEnabled() || m_threads.has("*")) {
        return;
    }

    if (!OclLib::init(loader())) {
        return;
    }

    const auto devices = m_devicesHint.empty() ? platform().devices() : filterDevices(platform().devices(), m_devicesHint);
    if (devices.empty()) {
        return;
    }

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    setupScryptChacha(devices);
#   endif

    size_t count = 0;

    count += xmrig::generate<Algorithm::CN>(m_threads, devices);
    count += xmrig::generate<Algorithm::CN_LITE>(m_threads, devices);
    count += xmrig::generate<Algorithm::CN_HEAVY>(m_threads, devices);
    count += xmrig::generate<Algorithm::CN_PICO>(m_threads, devices);
    count += xmrig::generate<Algorithm::CN_FEMTO>(m_threads, devices);
    count += xmrig::generate<Algorithm::RANDOM_X>(m_threads, devices);
    count += xmrig::generate<Algorithm::KAWPOW>(m_threads, devices);

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    {
        OclScryptChachaTuning tuning;
        tuning.lookupGap       = m_lookupGap;
        tuning.worksize        = m_worksize;
        tuning.reserveVramMb   = m_reserveVramMb;
        tuning.useSystemRam    = m_useSystemRam;
        tuning.hostRamBudgetMb = m_defaultHostRamBudgetMbPerGpu;

        count += xmrig::generateScryptChacha(m_threads, devices, tuning);
    }
#   endif

    m_shouldSave = count > 0;
}


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
// Pre-autotune setup for the SCRYPT_CHACHA family: size and validate the
// host-RAM budget and derive the per-GPU default share the autotune and the
// launch-data merge hand out. Ported from the CUDA backend's
// CudaConfig::setupScryptChacha.
//
// reserve_ram_mb and host_ram_budget_mb are host-RAM knobs: they only take
// effect when at least one device spills its scratchpad into system RAM. With
// use_system_ram off everywhere the budget is zero and the over-subscription
// guard never runs, so a low-RAM host mining in VRAM only is not blocked at
// startup.
void xmrig::OclConfig::setupScryptChacha(const std::vector<OclDevice> &devices)
{
    const auto &threadList = m_threads.get(Algorithm::SCRYPT_CHACHA_YAC);
    auto threadFor = [&threadList](uint32_t index) -> const OclThread * {
        if (!threadList.isEmpty()) {
            for (const auto &t : threadList.data()) {
                if (t.index() == index) {
                    return &t;
                }
            }
        }
        return nullptr;
    };

    // Only the GPUs that will actually mine take part in the host-RAM budget
    // math. When a "scrypt-chacha" array is present (Case B) generation is
    // skipped for the whole family, so a device with no entry runs no worker
    // and claims no host RAM: it must not dilute the per-GPU default share nor
    // count toward the over-subscription guard. With no array (Case A) the
    // generator autotunes every enumerated GPU, so all of them mine.
    const bool arrayPresent = !threadList.isEmpty();

    // Resolve every device's effective system-RAM settings once. The
    // remaining knobs need no resolution here: the generator takes the
    // globals (per-device pins cannot exist for threads it is about to
    // create) and the launch-data merge in get() covers the runners.
    struct HostRamTuning {
        bool     mines;
        bool     use_system_ram;
        bool     has_host_ram_override;
        uint32_t host_ram_override_mb;
    };

    std::vector<HostRamTuning> tuning;
    tuning.reserve(devices.size());
    bool   any_system_ram      = false;
    size_t mining_device_count = 0;
    for (const auto &device : devices) {
        const OclThread *thread = threadFor(device.index());
        HostRamTuning tn;
        tn.mines                 = !arrayPresent || thread != nullptr;
        tn.use_system_ram        = (thread && thread->hasUseSystemRam()) ? thread->useSystemRam() : m_useSystemRam;
        tn.has_host_ram_override = thread && thread->hasHostRamBudgetMb();
        tn.host_ram_override_mb  = tn.has_host_ram_override ? thread->hostRamBudgetMb() : 0;

        if (tn.mines) {
            ++mining_device_count;
            any_system_ram = any_system_ram || tn.use_system_ram;
        }
        tuning.push_back(tn);
    }

    m_defaultHostRamBudgetMbPerGpu = 0;

    if (!any_system_ram) {
        return;
    }

    const size_t mem_avail_mb    = readProcMemInfoAvailableMB();
    const size_t total_budget_mb = mem_avail_mb > static_cast<size_t>(m_reserveRamMb)
                                 ? mem_avail_mb - static_cast<size_t>(m_reserveRamMb)
                                 : 0;

    // A global host_ram_budget_mb (> 0) caps the system RAM all GPUs together
    // may use, split evenly. 0 falls back to the full MemAvailable-minus-
    // reserve budget. A cap the host cannot actually provide is clamped to
    // that same fallback (loudly): mining continues within physical RAM, the
    // reserve stays intact, and nothing over-subscribes.
    size_t configured_budget_mb = total_budget_mb;
    if (m_hostRamBudgetMb > 0) {
        if (static_cast<size_t>(m_hostRamBudgetMb) > total_budget_mb) {
            LOG_ERR("%s scrypt-chacha: global host_ram_budget_mb=%u MiB exceeds the %zu MiB "
                    "available (MemAvailable=%zu MiB, reserve_ram_mb=%u MiB), clamping to %zu MiB.",
                    ocl_tag(), m_hostRamBudgetMb, total_budget_mb, mem_avail_mb, m_reserveRamMb, total_budget_mb);
        }
        else {
            configured_budget_mb = static_cast<size_t>(m_hostRamBudgetMb);
        }
    }

    m_defaultHostRamBudgetMbPerGpu = mining_device_count == 0
        ? 0
        : static_cast<uint32_t>(configured_budget_mb / mining_device_count);

    LOG_VERBOSE("%s scrypt-chacha host RAM: MemAvailable %zu MiB, reserve_ram %u MiB, budget %zu MiB, per-GPU default %u MiB (%zu mining device%s)",
                ocl_tag(), mem_avail_mb, m_reserveRamMb, configured_budget_mb,
                m_defaultHostRamBudgetMbPerGpu, mining_device_count, mining_device_count > 1 ? "s" : "");

    // Sum the per-GPU host RAM each system-RAM device will claim. The
    // unpinned shares cannot exceed the budget by construction, so an
    // over-subscribed sum means absolute per-device pins that physical RAM
    // cannot back. That is a fatal misconfiguration: log it and abort rather
    // than start mining with a budget the host cannot honour.
    size_t sum_requested_mb = 0;
    for (const auto &tn : tuning) {
        if (!tn.mines || !tn.use_system_ram) {
            continue;
        }
        const uint32_t hrb = tn.has_host_ram_override ? tn.host_ram_override_mb
                                                      : m_defaultHostRamBudgetMbPerGpu;
        sum_requested_mb += static_cast<size_t>(hrb);
    }
    if (sum_requested_mb > total_budget_mb) {
        LOG_ERR("%s scrypt-chacha: per-GPU host_ram_budget_mb sums to %zu MiB but only "
                "%zu MiB is available (MemAvailable=%zu MiB, reserve_ram_mb=%u MiB), aborting.",
                ocl_tag(), sum_requested_mb, total_budget_mb, mem_avail_mb, m_reserveRamMb);

        throw std::runtime_error("scrypt-chacha: per-GPU host_ram_budget_mb over-subscribes system RAM");
    }
}
#endif


void xmrig::OclConfig::setDevicesHint(const char *devicesHint)
{
    if (devicesHint == nullptr) {
        return;
    }

    const auto indexes = String(devicesHint).split(',');
    m_devicesHint.reserve(indexes.size());

    for (const auto &index : indexes) {
        m_devicesHint.push_back(strtoul(index, nullptr, 10));
    }
}


#ifndef XMRIG_OS_APPLE
void xmrig::OclConfig::setPlatform(const rapidjson::Value &platform)
{
    if (platform.IsString()) {
        m_platformVendor = platform.GetString();
    }
    else if (platform.IsUint()) {
        m_platformVendor = nullptr;
        m_platformIndex  = platform.GetUint();
    }
}
#endif
