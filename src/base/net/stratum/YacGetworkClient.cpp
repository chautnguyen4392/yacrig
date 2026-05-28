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

#include "base/net/stratum/YacGetworkClient.h"


#ifdef XMRIG_FEATURE_HTTP


#include "3rdparty/rapidjson/document.h"
#include "3rdparty/rapidjson/error/en.h"
#include "base/io/json/Json.h"
#include "base/io/json/JsonRequest.h"
#include "base/io/log/Log.h"
#include "base/kernel/interfaces/IClientListener.h"
#include "base/net/http/Fetch.h"
#include "base/net/http/HttpData.h"
#include "base/net/http/HttpListener.h"
#include "base/net/stratum/SubmitResult.h"
#include "base/tools/Cvt.h"
#include "base/tools/Timer.h"
#include "net/JobResult.h"


#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <ctime>
#include <string>


namespace xmrig {


/* Hex-encode a 32-byte uint256 with byte-order reversed, matching yacoind's
 * `GetHex()` convention. yacoind stores uint256 little-endian in memory but
 * prints it big-endian everywhere a human reads it (block hashes, prev_block,
 * merkle_root in `gettimechaininfo`, the `data` field of `getwork` after the
 * per-uint32 un-swap, etc.). This helper makes our verbose logs comparable
 * line-for-line with `yacoin-cli getblockhash` / `getblock` output. */
static std::string hashHexBigEndian(const uint8_t *data32)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        const uint8_t b = data32[31 - i];
        out[2 * i + 0] = kHex[(b >> 4) & 0x0f];
        out[2 * i + 1] = kHex[b & 0x0f];
    }
    return out;
}


/* Format a Unix epoch second as an ISO-8601 UTC timestamp, used alongside the
 * raw int64 nTime when logging a parsed block header. */
static std::string formatUtcTime(int64_t epochSec)
{
    const auto t = static_cast<std::time_t>(epochSec);
    std::tm tm{};
#   ifdef _WIN32
    gmtime_s(&tm, &t);
#   else
    gmtime_r(&t, &tm);
#   endif
    char buf[32] = { 0 };
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}


/* Minimal RFC 4648 base64 encoder for the HTTP Basic auth header. yacoind requires
 * Authorization: Basic <base64(rpcuser:rpcpassword)>; rather than pull in a third-
 * party dependency for ~25 lines of code, encode inline. */
static std::string base64Encode(const std::string &in)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const auto b0 = static_cast<uint8_t>(in[i]);
        const auto b1 = static_cast<uint8_t>(in[i + 1]);
        const auto b2 = static_cast<uint8_t>(in[i + 2]);
        out.push_back(kAlphabet[(b0 >> 2) & 0x3f]);
        out.push_back(kAlphabet[((b0 << 4) | (b1 >> 4)) & 0x3f]);
        out.push_back(kAlphabet[((b1 << 2) | (b2 >> 6)) & 0x3f]);
        out.push_back(kAlphabet[b2 & 0x3f]);
    }

    const size_t rem = in.size() - i;
    if (rem == 1) {
        const auto b0 = static_cast<uint8_t>(in[i]);
        out.push_back(kAlphabet[(b0 >> 2) & 0x3f]);
        out.push_back(kAlphabet[(b0 << 4) & 0x3f]);
        out.append("==");
    }
    else if (rem == 2) {
        const auto b0 = static_cast<uint8_t>(in[i]);
        const auto b1 = static_cast<uint8_t>(in[i + 1]);
        out.push_back(kAlphabet[(b0 >> 2) & 0x3f]);
        out.push_back(kAlphabet[((b0 << 4) | (b1 >> 4)) & 0x3f]);
        out.push_back(kAlphabet[(b1 << 2) & 0x3f]);
        out.push_back('=');
    }

    return out;
}


} // namespace xmrig


xmrig::YacGetworkClient::YacGetworkClient(int id, IClientListener *listener) :
    BaseClient(id, listener)
{
    m_httpListener = std::make_shared<HttpListener>(this);
    m_timer        = new Timer(this);
}


xmrig::YacGetworkClient::~YacGetworkClient()
{
    delete m_timer;
}


void xmrig::YacGetworkClient::deleteLater()
{
    delete this;
}


bool xmrig::YacGetworkClient::disconnect()
{
    if (m_state != UnconnectedState) {
        setState(UnconnectedState);
    }
    return true;
}


bool xmrig::YacGetworkClient::isTLS() const
{
#   ifdef XMRIG_FEATURE_TLS
    return m_pool.isTLS();
#   else
    return false;
#   endif
}


void xmrig::YacGetworkClient::setPool(const Pool &pool)
{
    BaseClient::setPool(pool);

    // --coin=yac alone (no --algo) leaves m_pool.algorithm() invalid. Synthesise
    // it from the coin so workers see the right algo on their first setJob.
    if (!m_pool.algorithm().isValid() && m_pool.coin() == Coin::YAC) {
        m_pool.setAlgo(Algorithm::SCRYPT_CHACHA_YAC);
    }
}


void xmrig::YacGetworkClient::connect()
{
    setState(ConnectingState);

    if (!m_pool.algorithm().isValid() && m_pool.coin() != Coin::YAC) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("connect error: ") RED_BOLD("invalid algorithm"), tag());
        }
        retry();
        return;
    }

    // Get the chain info to get the first job and arm the polling timer.
    requestChainInfo();
}


void xmrig::YacGetworkClient::connect(const Pool &pool)
{
    setPool(pool);
    connect();
}


void xmrig::YacGetworkClient::onTimer(const Timer *)
{
    if (m_state == ConnectingState) {
        // First getwork RPC never came back (HTTP error, timeout). Restart from
        // scratch so we get a fresh m_failures count and a clean state machine.
        connect();
        return;
    }

    if (m_state == ConnectedState) {
        // Cheap "did the tip move?" probe; getwork fires only when the tip moves.
        requestChainInfo();
    }
}


int64_t xmrig::YacGetworkClient::requestWork()
{
    LOG_DEBUG("%s yac: requestWork() seq=%" PRId64, tag(), m_sequence);
    LOG_VERBOSE("%s " CYAN("-> getwork") " (seq=%" PRId64 ")", tag(), m_sequence);

    using namespace rapidjson;
    Document doc(kObjectType);
    Value params(kArrayType);
    JsonRequest::create(doc, m_sequence, "getwork", params);
    return rpcSend(doc, REQ_GETWORK);
}


int64_t xmrig::YacGetworkClient::requestChainInfo()
{
    LOG_DEBUG("%s yac: requestChainInfo() seq=%" PRId64
              " currentHeight=%" PRId64 " currentTip=%s",
              tag(), m_sequence, m_chainHeight,
              m_chainTipHash.isNull() ? "<none>" : m_chainTipHash.data());
    LOG_VERBOSE("%s " CYAN("-> gettimechaininfo") " (seq=%" PRId64 ")", tag(), m_sequence);

    using namespace rapidjson;
    Document doc(kObjectType);
    Value params(kArrayType);
    JsonRequest::create(doc, m_sequence, "gettimechaininfo", params);
    return rpcSend(doc, REQ_CHAININFO);
}


int64_t xmrig::YacGetworkClient::rpcSend(const rapidjson::Document &doc, RequestType type)
{
    FetchRequest req(HTTP_POST, m_pool.host(), m_pool.port(), "/", doc, m_pool.isTLS(), isQuiet());

    // yacoind uses HTTP Basic auth derived from -rpcuser / -rpcpassword.
    if (!m_user.isEmpty()) {
        std::string creds = std::string(m_user.data());
        creds += ":";
        if (!m_password.isNull()) {
            creds += m_password.data();
        }
        req.headers.insert({"Authorization", "Basic " + base64Encode(creds)});
    }

    LOG_DEBUG("%s yac: rpcSend() type=%d host=%s:%u seq=%" PRId64,
              tag(), static_cast<int>(type), m_pool.host().data(), m_pool.port(), m_sequence);

    fetch(tag(), std::move(req), m_httpListener, static_cast<int>(type));
    return m_sequence++;
}


bool xmrig::YacGetworkClient::parseGetworkResponse(const rapidjson::Value &result)
{
    LOG_DEBUG("%s yac: parseGetworkResponse() entry", tag());

    if (!result.IsObject()) {
        LOG_DEBUG("%s yac: parseGetworkResponse() rejected: result not an object", tag());
        return false;
    }

    const char *dataHex   = Json::getString(result, "data");
    const char *targetHex = Json::getString(result, "target");
    if (!dataHex || !targetHex) {
        LOG_DEBUG("%s yac: parseGetworkResponse() rejected: missing data or target fields", tag());
        return false;
    }
    if (std::strlen(dataHex) != 256 || std::strlen(targetHex) != 64) {
        LOG_DEBUG("%s yac: parseGetworkResponse() rejected: bad lengths data=%zu target=%zu",
                  tag(), std::strlen(dataHex), std::strlen(targetHex));
        return false;
    }

    uint8_t data[128];
    if (!Cvt::fromHex(data, sizeof(data), dataHex, 256)) {
        LOG_DEBUG("%s yac: parseGetworkResponse() rejected: data hex decode failed", tag());
        return false;
    }

    // yacoind emits getwork with ByteReverse applied to every uint32_t word
    // (legacy Bitcoin getwork). Recover the on-disk header by reversing again.
    for (size_t i = 0; i < 128; i += 4) {
        std::swap(data[i + 0], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
    }

    uint32_t version = 0;
    std::memcpy(&version, data, sizeof(version));
    if (version < 7) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("YAC v7+ block header required (got version %u)"), tag(), version);
        }
        return false;
    }

    // Dedupe: hash the 84-byte header into a fingerprint. The 84-byte slice
    // excludes the legacy SHA-256 padding tail (bytes 84..127), which yacoind
    // echoes verbatim and which therefore does not change when the chain tip
    // moves. Any change in version/prev_block/merkle_root/timestamp/bits/nonce
    // shows up in the hex; equal hex strings ⇒ same template, drop it.
    const String newTemplate = Cvt::toHex(data, 84);
    if (newTemplate == m_currentTemplate) {
        LOG_DEBUG("%s yac: parseGetworkResponse() duplicate template, template=%s",
                  tag(), newTemplate.data());
        return false;    // same template; nothing to do
    }

    // The job_id we expose to workers/logs is a short random 8-hex tag — the
    // 168-char template hex is unreadable in a debug log and tells the worker
    // nothing useful. Workers stamp this id onto their JobResult; submit()
    // verifies result.jobId == m_currentJobId to drop stale shares, so the
    // tag's only requirement is uniqueness across overlapping in-flight jobs
    // (which 32 random bits comfortably provides).
    const String newJobId = Cvt::toHex(Cvt::randomBytes(4));

    // Verbose dump of the parsed block header. The dedupe check above
    // ensures this fires at most once per new template (not on every
    // gettimechaininfo-driven re-poll), so the noise stays bounded.
    if (Log::isVerbose()) {
        int64_t  ntime    = 0;
        uint32_t nbits    = 0;
        uint32_t initNonce = 0;
        std::memcpy(&ntime,     data + 68, 8);
        std::memcpy(&nbits,     data + 76, 4);
        std::memcpy(&initNonce, data + 80, 4);

        LOG_VERBOSE("%s " CYAN_BOLD("new template parsed:"), tag());
        LOG_VERBOSE("%s   version       %u", tag(), version);
        LOG_VERBOSE("%s   prev_block    %s", tag(), hashHexBigEndian(data + 4).c_str());
        LOG_VERBOSE("%s   merkle_root   %s", tag(), hashHexBigEndian(data + 36).c_str());
        LOG_VERBOSE("%s   timestamp     %llu (%s UTC)",
                    tag(),
                    static_cast<unsigned long long>(ntime),
                    formatUtcTime(ntime).c_str());
        LOG_VERBOSE("%s   bits          0x%08x", tag(), nbits);
        LOG_VERBOSE("%s   nonce (seed)  0x%08x", tag(), initNonce);
        LOG_VERBOSE("%s   raw template  %s", tag(), newTemplate.data());
        LOG_VERBOSE("%s   job_id        %s", tag(), newJobId.data());
    }

    uint8_t target[32];
    if (!Cvt::fromHex(target, sizeof(target), targetHex, 64)) {
        LOG_DEBUG("%s yac: parseGetworkResponse() rejected: target hex decode failed", tag());
        return false;
    }

    Job job(false, m_pool.algorithm(), m_pool.url());
    job.setAlgorithm(Algorithm::SCRYPT_CHACHA_YAC);
    if (!job.setBlob(Cvt::toHex(data, 84).data())) {
        LOG_DEBUG("%s yac: parseGetworkResponse() rejected: setBlob failed", tag());
        return false;
    }
    job.setRawTarget32(target);

    // Fast prefilter: the top 8 bytes of the 32-byte target as little-endian
    // uint64 = numerically most-significant 64 bits of the 256-bit target.
    // Job::setTarget(const char*) interprets a 16-hex-char string this way; pass
    // the last 16 chars of the 64-char hex. If those bytes happen to be all
    // zero (extremely tight difficulty), the prefilter setter rejects; fall
    // back to setDiff(1) so the uint64 prefilter is effectively disabled
    // (m_target == max uint64) and the worker still gates submission via the
    // full 32-byte rawTarget32() compare.
    if (!job.setTarget(targetHex + 48)) {
        job.setDiff(1);
    }

    job.setHeight(m_chainHeight >= 0 ? static_cast<uint64_t>(m_chainHeight) : 0);
    job.setId(newJobId.data());

    std::memcpy(m_currentData, data, 128);
    m_currentJobId    = newJobId;
    m_currentTemplate = newTemplate;
    m_job             = std::move(job);

    LOG_DEBUG("%s yac: parseGetworkResponse() accepted: job_id=%s height=%" PRIu64
              " diff=%" PRIu64 " version=%u",
              tag(), newJobId.data(), m_job.height(), m_job.diff(), version);
    LOG_INFO("%s " GREEN_BOLD("received new job:") " job_id=%s height=%" PRIu64 " diff=%" PRIu64,
                tag(), newJobId.data(), m_job.height(), m_job.diff());

    return true;
}


int64_t xmrig::YacGetworkClient::submit(const JobResult &result)
{
    LOG_DEBUG("%s yac: submit() entry job_id=%s result_diff=%" PRIu64 " nonce=0x%08x",
              tag(), result.jobId.data(), result.diff,
              static_cast<uint32_t>(result.nonce));

    if (result.jobId != m_currentJobId) {
        // Stale share — the chain has moved on since the worker started this batch.
        LOG_DEBUG("%s yac: submit() dropped stale share: result_job=%s current_job=%s",
                  tag(), result.jobId.data(),
                  m_currentJobId.isNull() ? "<none>" : m_currentJobId.data());
        return -1;
    }

    // Patch the nonce into bytes 80..83 of the cached unswapped buffer.
    // m_currentData is post-byte-swap (on-disk layout), so the patched buffer
    // also matches on-disk layout. Re-applying the per-uint32 swap restores
    // the wire format yacoind expects.
    uint8_t wire[128];
    std::memcpy(wire, m_currentData, 128);
    std::memcpy(wire + 80, &result.nonce, 4);    // little-endian uint32

    for (size_t i = 0; i < 128; i += 4) {
        std::swap(wire[i + 0], wire[i + 3]);
        std::swap(wire[i + 1], wire[i + 2]);
    }

    // Verbose dump of the submitted share: the winning nonce, the
    // scrypt-chacha hash the worker computed (displayed in yacoind's
    // big-endian `GetHex()` form), and the full 256-hex-char wire payload
    // exactly as it will land in the `getwork [data]` RPC call.
    if (Log::isVerbose()) {
        // Reconstruct the 84-byte block header in on-disk layout (the canonical
        // yacoind `block_header` struct: version | prev_block | merkle_root |
        // timestamp | bits | nonce). m_currentData is already on-disk layout;
        // patch the winning nonce into bytes 80..83 of a local copy so the
        // logged header matches `yacoin-cli getblock <hash>` byte-for-byte.
        uint8_t header[84];
        std::memcpy(header, m_currentData, 80);
        std::memcpy(header + 80, &result.nonce, 4);

        const uint32_t nonce32 = static_cast<uint32_t>(result.nonce);
        LOG_VERBOSE("%s " MAGENTA_BOLD("submitting share:"), tag());
        LOG_VERBOSE("%s   nonce         0x%08x (%u)", tag(), nonce32, nonce32);
        LOG_VERBOSE("%s   hash          %s", tag(), hashHexBigEndian(result.result()).c_str());
        LOG_VERBOSE("%s   header (84B)  %s", tag(), Cvt::toHex(header, 84).data());
        LOG_VERBOSE("%s   job_id        %s", tag(), result.jobId.data());
    }

    using namespace rapidjson;
    Document doc(kObjectType);
    auto &allocator = doc.GetAllocator();

    Value params(kArrayType);
    params.PushBack(Cvt::toHex(wire, 128, doc), allocator);

    JsonRequest::create(doc, m_sequence, "getwork", params);
    m_results[m_sequence] = SubmitResult(m_sequence, result.diff, result.actualDiff(), 0, result.backend);

    LOG_DEBUG("%s yac: submit() dispatched seq=%" PRId64 " actual_diff=%" PRIu64,
              tag(), m_sequence, result.actualDiff());

    return rpcSend(doc, REQ_SUBMIT);
}


void xmrig::YacGetworkClient::onHttpData(const HttpData &data)
{
    LOG_DEBUG("%s yac: onHttpData() type=%d status=%d body_size=%zu",
              tag(), data.userType, data.status, data.body.size());

    if (data.status != 200) {
        LOG_DEBUG("%s yac: onHttpData() HTTP error status=%d -> retry()", tag(), data.status);
        return retry();
    }

    m_ip = data.ip().c_str();

    rapidjson::Document doc;
    if (doc.Parse(data.body.c_str()).HasParseError()) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("JSON decode failed: ") RED_BOLD("\"%s\""),
                    tag(), rapidjson::GetParseError_En(doc.GetParseError()));
        }
        return retry();
    }

    const int64_t id              = Json::getInt64(doc, "id", -1);
    const rapidjson::Value &result = Json::getValue(doc, "result");
    const rapidjson::Value &error  = Json::getValue(doc, "error");

    switch (static_cast<RequestType>(data.userType)) {

    case REQ_GETWORK: {
        if (error.IsObject()) {
            if (!isQuiet()) {
                LOG_ERR("%s " RED("getwork RPC error: %s"), tag(),
                        Json::getString(error, "message", "unknown"));
            }
            return retry();
        }
        if (!parseGetworkResponse(result)) {
            // Either bad JSON, version<7, or a duplicate template. Only retry on
            // the first two; a duplicate during steady-state polling is normal.
            if (m_state == ConnectingState) {
                return retry();
            }
            return;
        }
        if (m_state == ConnectingState) {
            setState(ConnectedState);    // fires onLoginSuccess via the ConnectedState branch
        }
        m_listener->onJobReceived(this, m_job, rapidjson::Value{});
        break;
    }

    case REQ_CHAININFO: {
        if (error.IsObject() || !result.IsObject()) {
            LOG_DEBUG("%s yac: gettimechaininfo malformed reply -> retry()", tag());
            return retry();
        }
        const int64_t blocks      = Json::getInt64(result, "blocks", -1);
        const char *bestblockHash = Json::getString(result, "bestblockhash");
        if (blocks < 0 || bestblockHash == nullptr) {
            // gettimechaininfo response missing required fields — treat as transient
            // RPC failure; the next tick will try again.
            LOG_DEBUG("%s yac: gettimechaininfo missing fields blocks=%" PRId64
                      " bestblockhash=%s -> retry()",
                      tag(), blocks, bestblockHash ? bestblockHash : "<null>");
            return retry();
        }
        const String newTipHash(bestblockHash);
        if (blocks != m_chainHeight || newTipHash != m_chainTipHash) {
            LOG_DEBUG("%s yac: chain tip moved: blocks %" PRId64 "->%" PRId64
                      " tip %s->%s",
                      tag(),
                      m_chainHeight, blocks,
                      m_chainTipHash.isNull() ? "<none>" : m_chainTipHash.data(),
                      newTipHash.data());
            LOG_INFO("%s " CYAN("chain tip moved") " height=%" PRId64
                        " bestblockhash=%s", tag(), blocks, newTipHash.data());
            m_chainHeight  = blocks;
            m_chainTipHash = newTipHash;
            requestWork();    // chain tip moved -> fetch the new template
        }
        else {
            LOG_DEBUG("%s yac: chain tip unchanged blocks=%" PRId64, tag(), blocks);
        }
        break;
    }

    case REQ_SUBMIT: {
        const char *err = nullptr;
        if (error.IsObject()) {
            err = Json::getString(error, "message", "rejected");
        }
        else if (result.IsBool() && !result.GetBool()) {
            err = "stale or invalid share";
        }
        LOG_DEBUG("%s yac: submit response id=%" PRId64 " err=%s",
                  tag(), id, err ? err : "<none>");
        handleSubmitResponse(id, err);    // BaseClient method — fires onResultAccepted
        // Immediately trigger a chain info poll to see if the chain tip has moved and get the new template if it has moved.
        requestChainInfo();
        break;
    }
    }
}


void xmrig::YacGetworkClient::retry()
{
    m_failures++;
    m_listener->onClose(this, static_cast<int>(m_failures));

    if (m_failures == -1) {
        return;
    }

    if (m_state == ConnectedState) {
        setState(ConnectingState);
    }

    // Invalidate the cached chain tip on every disconnect. The reconnect
    // happy-path relies on REQ_CHAININFO observing "tip moved" to trigger
    // requestWork() -> REQ_GETWORK success -> setState(ConnectedState) ->
    // periodic timer rearm. If the daemon went down and came back up
    // without the chain progressing (common in low-difficulty test setups,
    // or any brief outage where no other miner produces a block), the
    // post-reconnect gettimechaininfo would return the same blocks /
    // bestblockhash we had cached, the inequality check would be false,
    // requestWork() would not fire, and the one-shot retry timer would
    // never get rearmed -- the client would freeze in ConnectingState
    // with no further polling. Forcing the cache to its initial sentinel
    // values guarantees the inequality fires on the next success.
    m_chainHeight    = -1;
    m_chainTipHash   = String();

    m_timer->stop();
    m_timer->start(m_retryPause, 0);
}


void xmrig::YacGetworkClient::setState(SocketState state)
{
    if (m_state == state) {
        return;
    }

    m_state = state;

    switch (state) {
    case ConnectedState: {
        m_failures = 0;
        m_listener->onLoginSuccess(this);

        const uint64_t interval = std::max<uint64_t>(20, m_pool.pollInterval());
        m_timer->start(interval, interval);
        break;
    }

    case UnconnectedState:
        m_failures        = -1;
        m_timer->stop();
        m_chainHeight     = -1;
        m_chainTipHash    = String();
        m_currentJobId    = String();
        m_currentTemplate = String();
        break;

    default:
        break;
    }
}


#endif // XMRIG_FEATURE_HTTP
