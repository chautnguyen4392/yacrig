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

#include "backend/cuda/CudaConfig.h"
#include "3rdparty/rapidjson/document.h"
#include "backend/common/Tags.h"
#include "backend/cuda/CudaConfig_gen.h"
#include "backend/cuda/wrappers/CudaLib.h"
#include "base/io/json/Json.h"
#include "base/io/log/Log.h"

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
#   include "base/tools/HostMemoryInfo.h"
#   include <stdexcept>
#endif


namespace xmrig {


static bool generated           = false;
static const char *kBfactorHint = "bfactor-hint";
static const char *kBsleepHint  = "bsleep-hint";
static const char *kDevicesHint = "devices-hint";
static const char *kEnabled     = "enabled";
static const char *kLoader      = "loader";

#ifdef XMRIG_FEATURE_NVML
static const char *kNvml        = "nvml";
#endif

#ifdef XMRIG_ALGO_SCRYPT_CHACHA
static const char *kLookupGap       = "lookup_gap";
static const char *kUseSystemRam    = "use_system_ram";
static const char *kReserveVramMb   = "reserve_vram_mb";
static const char *kReserveRamMb    = "reserve_ram_mb";
static const char *kHostRamBudgetMb = "host_ram_budget_mb";
#endif


extern template class Threads<CudaThreads>;


} // namespace xmrig


rapidjson::Value xmrig::CudaConfig::toJSON(rapidjson::Document &doc) const
{
    using namespace rapidjson;
    auto &allocator = doc.GetAllocator();

    Value obj(kObjectType);

    obj.AddMember(StringRef(kEnabled),  m_enabled, allocator);
    obj.AddMember(StringRef(kLoader),   m_loader.toJSON(), allocator);

#   ifdef XMRIG_ALGO_SCRYPT_CHACHA
    obj.AddMember(StringRef(kLookupGap),       m_lookup_gap,       allocator);
    obj.AddMember(StringRef(kUseSystemRam),    m_use_system_ram,   allocator);
    obj.AddMember(StringRef(kReserveVramMb),   m_reserve_vram_mb,  allocator);
    obj.AddMember(StringRef(kReserveRamMb),    m_reserve_ram_mb,   allocator);
    obj.AddMember(StringRef(kHostRamBudgetMb), m_host_ram_budget_mb, allocator);
#   endif

#   ifdef XMRIG_FEATURE_NVML
    if (m_nvmlLoader.isNull()) {
        obj.AddMember(StringRef(kNvml), m_nvml, allocator);
    }
    else {
        obj.AddMember(StringRef(kNvml), m_nvmlLoader.toJSON(), allocator);
    }
#   endif

    m_threads.toJSON(obj, doc);

    return obj;
}


std::vector<xmrig::CudaLaunchData> xmrig::CudaConfig::get(const Miner *miner, const Algorithm &algorithm, const std::vector<CudaDevice> &devices) const
{
    auto deviceIndex = [&devices](uint32_t index) -> int {
        for (uint32_t i = 0; i < devices.size(); ++i) {
            if (devices[i].index() == index) {
                return i;
            }
        }

        return -1;
    };

    std::vector<CudaLaunchData> out;
    const auto &threads = m_threads.get(algorithm);

    if (threads.isEmpty()) {
        return out;
    }

    out.reserve(threads.count());

    for (const auto &thread : threads.data()) {
        const int index = deviceIndex(thread.index());
        if (index == -1) {
            LOG_INFO("%s" YELLOW(" skip non-existing device with index ") YELLOW_BOLD("%u"), cuda_tag(), thread.index());
            continue;
        }

        out.emplace_back(miner, algorithm, thread, devices[static_cast<size_t>(index)]);

#       ifdef XMRIG_ALGO_SCRYPT_CHACHA
        if (algorithm.family() == Algorithm::SCRYPT_CHACHA) {
            auto &launch = out.back();
            launch.scryptchacha_lookup_gap         = thread.hasLookupGap()        ? thread.lookupGap()         : m_lookup_gap;
            launch.scryptchacha_use_system_ram     = thread.hasUseSystemRam()     ? thread.useSystemRam()      : m_use_system_ram;
            launch.scryptchacha_reserve_vram_mb    = thread.hasReserveVramMb()    ? thread.reserveVramMb()     : m_reserve_vram_mb;
            launch.scryptchacha_host_ram_budget_mb = thread.hasHostRamBudgetMb() ? thread.hostRamBudgetMb()    : m_default_host_ram_budget_mb_per_gpu;
        }
#       endif
    }

    return out;
}


void xmrig::CudaConfig::read(const rapidjson::Value &value)
{
    if (value.IsObject()) {
        m_enabled   = Json::getBool(value, kEnabled, m_enabled);
        m_loader    = Json::getString(value, kLoader);
        m_bfactor   = std::min(Json::getUint(value, kBfactorHint, m_bfactor), 12U);
        m_bsleep    = Json::getUint(value, kBsleepHint, m_bsleep);

#       ifdef XMRIG_ALGO_SCRYPT_CHACHA
        m_lookup_gap        = Json::getInt(value, kLookupGap,       m_lookup_gap);
        m_use_system_ram    = Json::getBool(value, kUseSystemRam,    m_use_system_ram);
        m_reserve_vram_mb   = Json::getInt(value, kReserveVramMb,   m_reserve_vram_mb);
        m_reserve_ram_mb    = Json::getInt(value, kReserveRamMb,    m_reserve_ram_mb);
        m_host_ram_budget_mb = Json::getInt(value, kHostRamBudgetMb, m_host_ram_budget_mb);
#       endif

        setDevicesHint(Json::getString(value, kDevicesHint));

#       ifdef XMRIG_FEATURE_NVML
        auto &nvml = Json::getValue(value, kNvml);
        if (nvml.IsString()) {
            m_nvmlLoader = nvml.GetString();
        }
        else if (nvml.IsBool()) {
            m_nvml = nvml.GetBool();
        }
#       endif

        m_threads.read(value);

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


void xmrig::CudaConfig::generate()
{
    if (generated) {
        return;
    }

    if (!isEnabled() || m_threads.has("*")) {
        return;
    }

    if (!CudaLib::init(loader())) {
        return;
    }

    if (!CudaLib::runtimeVersion() || !CudaLib::driverVersion() || !CudaLib::deviceCount()) {
        return;
    }

    const auto devices = CudaLib::devices(bfactor(), bsleep(), m_devicesHint);
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
    count += xmrig::generate<Algorithm::SCRYPT_CHACHA>(m_threads, devices);
#   endif

    generated    = true;
    m_shouldSave = count > 0;
}


#ifdef XMRIG_ALGO_SCRYPT_CHACHA
// Pre-autotune setup for the SCRYPT_CHACHA family: resolve the effective
// per-GPU tuning (a per-device JSON override wins over the global default),
// size and validate the host-RAM budget, and push the result to each device's
// autotune ctx so the plugin's cuda_get_deviceinfo SCRYPT_CHACHA branch sees
// it.
//
// reserve_ram_mb and host_ram_budget_mb are host-RAM knobs: they only take
// effect when at least one device spills its scratchpad into system RAM. With
// use_system_ram off everywhere the budget is zero and the over-subscription
// guard never runs, so a low-RAM host mining in VRAM only is not blocked at
// startup. lookup_gap and reserve_vram_mb are not host-RAM knobs and always
// apply.
void xmrig::CudaConfig::setupScryptChacha(const std::vector<CudaDevice> &devices)
{
    struct Tuning {
        int  lookup_gap;
        bool use_system_ram;
        int  reserve_vram_mb;
        bool has_host_ram_override;
        int  host_ram_override_mb;
    };

    const auto &threadList = m_threads.get(Algorithm::SCRYPT_CHACHA_YAC);
    auto threadFor = [&threadList](uint32_t index) -> const CudaThread * {
        if (!threadList.isEmpty()) {
            for (const auto &t : threadList.data()) {
                if (t.index() == index) {
                    return &t;
                }
            }
        }
        return nullptr;
    };

    // Resolve every device's effective tuning once. host_ram_budget_mb is
    // resolved later, once the per-GPU default budget is known.
    std::vector<Tuning> tuning;
    tuning.reserve(devices.size());
    bool any_system_ram = false;
    for (const auto &d : devices) {
        const CudaThread *t = threadFor(d.index());
        Tuning tn;
        tn.lookup_gap            = (t && t->hasLookupGap())       ? t->lookupGap()       : m_lookup_gap;
        tn.use_system_ram        = (t && t->hasUseSystemRam())    ? t->useSystemRam()    : m_use_system_ram;
        tn.reserve_vram_mb       = (t && t->hasReserveVramMb())   ? t->reserveVramMb()   : m_reserve_vram_mb;
        tn.has_host_ram_override = t && t->hasHostRamBudgetMb();
        tn.host_ram_override_mb  = tn.has_host_ram_override       ? t->hostRamBudgetMb() : 0;

        any_system_ram = any_system_ram || tn.use_system_ram;
        tuning.push_back(tn);
    }

    m_default_host_ram_budget_mb_per_gpu = 0;

    if (any_system_ram) {
        const size_t mem_avail_mb    = readProcMemInfoAvailableMB();
        const size_t total_budget_mb = mem_avail_mb > static_cast<size_t>(m_reserve_ram_mb)
                                     ? mem_avail_mb - static_cast<size_t>(m_reserve_ram_mb)
                                     : 0;

        // A global host_ram_budget_mb (> 0) caps the system RAM all GPUs
        // together may use, split evenly. 0 falls back to the full
        // MemAvailable-minus-reserve budget. Either way the total must fit in
        // physical host RAM.
        size_t configured_budget_mb = total_budget_mb;
        if (m_host_ram_budget_mb > 0) {
            if (static_cast<size_t>(m_host_ram_budget_mb) > total_budget_mb) {
                LOG_ERR("%s scrypt-chacha: global host_ram_budget_mb=%d MB exceeds the %zu MB "
                        "available (MemAvailable=%zu MB, reserve_ram_mb=%d MB).",
                        cuda_tag(), m_host_ram_budget_mb, total_budget_mb, mem_avail_mb, m_reserve_ram_mb);
                throw std::runtime_error("scrypt-chacha host_ram_budget over-subscribed");
            }
            configured_budget_mb = static_cast<size_t>(m_host_ram_budget_mb);
        }

        m_default_host_ram_budget_mb_per_gpu = devices.empty()
            ? 0
            : static_cast<int>(configured_budget_mb / devices.size());

        LOG_VERBOSE("%s scrypt-chacha CUDA host RAM: MemAvailable %zu MB, reserve_ram %d MB, budget %zu MB, per-GPU default %d MB (%zu device%s)",
                    cuda_tag(), mem_avail_mb, m_reserve_ram_mb, configured_budget_mb,
                    m_default_host_ram_budget_mb_per_gpu, devices.size(), devices.size() > 1 ? "s" : "");

        // Sum the per-GPU host RAM each system-RAM device will claim and refuse
        // to start if it over-subscribes the budget.
        size_t sum_requested_mb = 0;
        for (const auto &tn : tuning) {
            if (!tn.use_system_ram) {
                continue;
            }
            const int hrb = tn.has_host_ram_override ? tn.host_ram_override_mb
                                                     : m_default_host_ram_budget_mb_per_gpu;
            if (hrb > 0) {
                sum_requested_mb += static_cast<size_t>(hrb);
            }
        }
        if (sum_requested_mb > total_budget_mb) {
            LOG_ERR("%s scrypt-chacha CUDA: per-GPU host_ram_budget_mb sums to %zu MB but only "
                    "%zu MB is available (MemAvailable=%zu MB, reserve_ram_mb=%d MB).",
                    cuda_tag(), sum_requested_mb, total_budget_mb, mem_avail_mb, m_reserve_ram_mb);
            throw std::runtime_error("scrypt-chacha host_ram_budget over-subscribed");
        }
    }

    if (!CudaLib::hasScryptChachaConfig()) {
        return;
    }

    // Push the effective tuning to each device's autotune ctx. The host RAM
    // budget is zero unless that device uses system RAM, so the plugin sizes
    // MAXWARPS_RAM from a real budget only when system RAM is in play.
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto &tn  = tuning[i];
        const int   hrb = tn.use_system_ram
            ? (tn.has_host_ram_override ? tn.host_ram_override_mb : m_default_host_ram_budget_mb_per_gpu)
            : 0;
        CudaLib::scryptChachaConfig(devices[i].ctx(), tn.lookup_gap, tn.use_system_ram,
                                    tn.reserve_vram_mb, hrb);

        LOG_VERBOSE("%s" CYAN_BOLD(" #%u") " scrypt-chacha CUDA autotune tuning: lookup_gap %d, system_ram %s, reserve_vram %d MiB, host_ram_budget %d MiB",
                    cuda_tag(), devices[i].index(), tn.lookup_gap, tn.use_system_ram ? "on" : "off",
                    tn.reserve_vram_mb, hrb);
    }
}
#endif


void xmrig::CudaConfig::setDevicesHint(const char *devicesHint)
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
