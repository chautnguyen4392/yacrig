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

#ifndef XMRIG_YACGETWORKCLIENT_H
#define XMRIG_YACGETWORKCLIENT_H


#ifdef XMRIG_FEATURE_HTTP


#include "base/kernel/interfaces/IHttpListener.h"
#include "base/kernel/interfaces/ITimerListener.h"
#include "base/net/stratum/BaseClient.h"


#include <memory>


namespace xmrig {


class HttpData;
class Timer;


class YacGetworkClient : public BaseClient, public ITimerListener, public IHttpListener
{
public:
    XMRIG_DISABLE_COPY_MOVE_DEFAULT(YacGetworkClient)

    YacGetworkClient(int id, IClientListener *listener);
    ~YacGetworkClient() override;

protected:
    // IClient
    bool disconnect() override;
    bool isTLS() const override;
    int64_t submit(const JobResult &result) override;
    void connect() override;
    void connect(const Pool &pool) override;
    void deleteLater() override;
    void setPool(const Pool &pool) override;

    inline bool hasExtension(Extension) const noexcept override        { return false; }
    inline const char *mode() const override                           { return "yac-getwork"; }
    inline const char *tlsFingerprint() const override                 { return nullptr; }
    inline const char *tlsVersion() const override                     { return nullptr; }
    inline int64_t send(const rapidjson::Value &, Callback) override   { return -1; }
    inline int64_t send(const rapidjson::Value &) override             { return -1; }
    inline void tick(uint64_t) override                                {}

    // ITimerListener
    void onTimer(const Timer *timer) override;

    // IHttpListener
    void onHttpData(const HttpData &data) override;

private:
    enum RequestType : int {
        REQ_GETWORK    = 1,    // userType for getwork (no params) — fetch a job
        REQ_CHAININFO  = 2,    // userType for gettimechaininfo — cheap "did the chain tip move?"
        REQ_SUBMIT     = 3,    // userType for getwork [hex_blob] — submit a share
    };

    int64_t requestWork();
    int64_t requestChainInfo();
    int64_t rpcSend(const rapidjson::Document &doc, RequestType type);
    bool parseGetworkResponse(const rapidjson::Value &result);
    void retry();
    void setState(SocketState state);

    std::shared_ptr<IHttpListener> m_httpListener;
    Timer *m_timer = nullptr;

    uint8_t  m_currentData[128]{};    // last-decoded (post-swap) getwork buffer
    String   m_currentJobId;          // short hex(randomBytes(4)) — human-readable Job::id() for logs / share-stamping
    String   m_currentTemplate;       // hex(blob[0..83]) — fingerprint used to dedupe poll-on-same-template
    int64_t  m_chainHeight  = -1;     // last "blocks" from gettimechaininfo; -1 = never queried
    String   m_chainTipHash;          // last "bestblockhash" from gettimechaininfo; empty = never queried
};


} // namespace xmrig


#endif // XMRIG_FEATURE_HTTP


#endif // XMRIG_YACGETWORKCLIENT_H
