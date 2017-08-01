/* Copyright (c) 2016, Fengping Bao <jamol@live.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Http2Request.h"
#include "http/Uri.h"
#include "H2ConnectionMgr.h"
#include "util/kmtrace.h"
#include "util/util.h"

#include <sstream>
#include <algorithm>
#include <string>

using namespace kuma;

Http2Request::Http2Request(const EventLoopPtr &loop, std::string ver)
: HttpRequest::Impl(std::move(ver)), loop_(loop)
{
    KM_SetObjKey("Http2Request");
}

Http2Request::~Http2Request()
{
    iovec iov;
    while (data_queue_.dequeue(iov)) {
        delete [] (uint8_t*)iov.iov_base;
    }
    loop_token_.reset();
}

KMError Http2Request::setSslFlags(uint32_t ssl_flags)
{
    ssl_flags_ = ssl_flags;
    return KMError::NOERR;
}

void Http2Request::addHeader(std::string name, std::string value)
{
    transform(name.begin(), name.end(), name.begin(), ::tolower);
    if(!name.empty()) {
        if (is_equal("Transfer-Encoding", name) && is_equal("chunked", value)) {
            is_chunked_ = true;
            return; // omit chunked
        }
        HttpHeader::addHeader(std::move(name), std::move(value));
    }
}

KMError Http2Request::sendRequest()
{
    std::string str_port = uri_.getPort();
    uint16_t port = 80;
    if (is_equal("https", uri_.getScheme())) {
        ssl_flags_ |= SSL_ENABLE;
        port = 443;
    } else {
        ssl_flags_ = SSL_NONE;
    }
    if(!str_port.empty()) {
        port = std::stoi(str_port);
    }
    
    setState(State::CONNECTING);
    auto loop = loop_.lock();
    if (!loop) {
        return KMError::INVALID_STATE;
    }
    auto &conn_mgr = H2ConnectionMgr::getRequestConnMgr(ssl_flags_ != SSL_NONE);
    conn_ = conn_mgr.getConnection(uri_.getHost(), port, ssl_flags_, loop);
    if (!conn_ || !conn_->eventLoop()) {
        KUMA_ERRXTRACE("sendRequest, failed to get H2Connection");
        return KMError::INVALID_PARAM;
    } else {
        loop_token_.eventLoop(conn_->eventLoop());
        if (conn_->isInSameThread()) {
            return sendRequest_i();
        } else if (!conn_->async([this] {
            auto err = sendRequest_i();
            if (err != KMError::NOERR) {
                onError(err);
            }
        }, &loop_token_)) {
            KUMA_ERRXTRACE("sendRequest, failed to run in H2Connection, key="<<conn_->getConnectionKey());
            return KMError::INVALID_STATE;
        }
    }
    return KMError::NOERR;
}

KMError Http2Request::sendRequest_i()
{
    if (!conn_) {
        return KMError::INVALID_STATE;
    }
    if (!conn_->isReady()) {
        conn_->addConnectListener(getObjId(), [this] (KMError err) { onConnect(err); });
        return KMError::NOERR;
    } else {
        return sendHeaders();
    }
}

const std::string& Http2Request::getHeaderValue(std::string name) const
{
    auto it = rsp_headers_.find(name);
    return it != rsp_headers_.end() ? it->second : EmptyString;
}

void Http2Request::forEachHeader(EnumrateCallback cb)
{
    for (auto &kv : rsp_headers_) {
        cb(kv.first, kv.second);
    }
}

void Http2Request::checkHeaders()
{
    if(!hasHeader("accept")) {
        addHeader("accept", "*/*");
    }
    if(!hasHeader("content-type")) {
        addHeader("content-type", "application/octet-stream");
    }
    if(!hasHeader("user-agent")) {
        addHeader("user-agent", UserAgent);
    }
    if(!hasHeader("cache-control")) {
        addHeader("cache-control", "no-cache");
    }
    if(!hasHeader("pragma")) {
        addHeader("pragma", "no-cache");
    }
}

size_t Http2Request::buildHeaders(HeaderVector &headers)
{
    HttpHeader::processHeader();
    size_t headers_size = 0;
    headers.emplace_back(std::make_pair(H2HeaderMethod, method_));
    headers_size += H2HeaderMethod.size() + method_.size();
    headers.emplace_back(std::make_pair(H2HeaderScheme, uri_.getScheme()));
    headers_size += H2HeaderScheme.size() + uri_.getScheme().size();
    std::string path = uri_.getPath();
    if(!uri_.getQuery().empty()) {
        path = "?" + uri_.getQuery();
    }
    if(!uri_.getFragment().empty()) {
        path = "#" + uri_.getFragment();
    }
    headers.emplace_back(std::make_pair(H2HeaderPath, path));
    headers_size += H2HeaderPath.size() + path.size();
    headers.emplace_back(std::make_pair(H2HeaderAuthority, uri_.getHost()));
    headers_size += H2HeaderAuthority.size() + uri_.getHost().size();
    for (auto it : header_map_) {
        headers.emplace_back(std::make_pair(it.first, it.second));
        headers_size += it.first.size() + it.second.size();
    }
    return headers_size;
}

KMError Http2Request::sendHeaders()
{
    stream_ = conn_->createStream();
    stream_->setHeadersCallback([this] (const HeaderVector &headers, bool endSteam) {
        onHeaders(headers, endSteam);
    });
    stream_->setDataCallback([this] (void *data, size_t len, bool endSteam) {
        onData(data, len, endSteam);
    });
    stream_->setRSTStreamCallback([this] (int err) {
        onRSTStream(err);
    });
    stream_->setWriteCallback([this] {
        onWrite();
    });
    setState(State::SENDING_HEADER);
    
    HeaderVector headers;
    size_t headers_size = buildHeaders(headers);
    bool end_stream = !has_content_length_ && !is_chunked_;
    auto ret = stream_->sendHeaders(headers, headers_size, end_stream);
    if (ret == KMError::NOERR) {
        if (end_stream) {
            setState(State::RECVING_RESPONSE);
        } else {
            setState(State::SENDING_BODY);
            auto loop = conn_->eventLoop();
            if (loop) {
                loop->post([this] { onWrite(); }, &loop_token_);
            }
        }
    }
    return ret;
}

void Http2Request::onConnect(KMError err)
{
    if(err != KMError::NOERR) {
        onError(err);
        return ;
    }
    sendHeaders();
}

void Http2Request::onError(KMError err)
{
    if(error_cb_) error_cb_(err);
}

int Http2Request::sendData(const void* data, size_t len)
{
    if (!conn_) {
        return -1;
    }
    if (getState() != State::SENDING_BODY) {
        return 0;
    }
    if (write_blocked_) {
        return 0;
    }
    if (conn_->isInSameThread() && data_queue_.empty()) {
        return sendData_i(data, len); // return the bytes sent directly
    } else {
        uint8_t *d = nullptr;
        if (data && len) {
            d = new uint8_t[len];
            memcpy(d, data, len);
        }
        iovec iov;
        iov.iov_base = (char*)d;
        iov.iov_len = len;
        data_queue_.enqueue(iov);
        if (data_queue_.size() <= 1) {
            conn_->async([=] { sendData_i(); }, &loop_token_);
        }
        return int(len);
    }
}

int Http2Request::sendData_i(const void* data, size_t len)
{
    if (getState() != State::SENDING_BODY) {
        return 0;
    }
    int ret = 0;
    size_t send_len = len;
    if (data && len) {
        if (has_content_length_ && body_bytes_sent_ + send_len > content_length_) {
            send_len = content_length_ - body_bytes_sent_;
        }
        ret = stream_->sendData(data, send_len, false);
        if (ret > 0) {
            body_bytes_sent_ += ret;
        }
    }
    bool end_stream = (!data && !len) || (has_content_length_ && body_bytes_sent_ >= content_length_);
    if (end_stream) {
        stream_->sendData(nullptr, 0, true);
        setState(State::RECVING_RESPONSE);
    }
    if (ret == 0) {
        write_blocked_ = true;
    }
    return ret;
}

int Http2Request::sendData_i()
{
    int bytes_sent = 0;
    while (!data_queue_.empty()) {
        auto &iov = data_queue_.front();
        int ret = sendData_i(iov.iov_base, iov.iov_len);
        if (ret > 0) {
            bytes_sent += ret;
            data_queue_.pop_front();
            delete [] (uint8_t*)iov.iov_base;
        } else if (ret == 0) {
            break;
        } else {
            onError(KMError::FAILED);
            return -1;
        }
    }
    return bytes_sent;
}

void Http2Request::onHeaders(const HeaderVector &headers, bool end_stream)
{
    if (headers.empty()) {
        return;
    }
    if (!is_equal(headers[0].first, H2HeaderStatus)) {
        return;
    }
    status_code_ = std::stoi(headers[0].second);
    std::string str_cookie;
    for (size_t i = 1; i < headers.size(); ++i) {
        if (is_equal(headers[i].first, H2HeaderCookie)) {
            if (!str_cookie.empty()) {
                str_cookie += "; ";
            }
            str_cookie += headers[i].second;
        } else {
            rsp_headers_.emplace(headers[i].first, headers[i].second);
        }
    }
    if (!str_cookie.empty()) {
        rsp_headers_.emplace("Cookie", str_cookie);
    }
    DESTROY_DETECTOR_SETUP();
    if (header_cb_) header_cb_();
    DESTROY_DETECTOR_CHECK_VOID();
    if (end_stream) {
        setState(State::COMPLETE);
        if (response_cb_) response_cb_();
    }
}

void Http2Request::onData(void *data, size_t len, bool end_stream)
{
    DESTROY_DETECTOR_SETUP();
    if (data_cb_ && len > 0) data_cb_(data, len);
    DESTROY_DETECTOR_CHECK_VOID();
    
    if (end_stream && response_cb_) {
        setState(State::COMPLETE);
        response_cb_();
    }
}

void Http2Request::onRSTStream(int err)
{
    onError(KMError::FAILED);
}

void Http2Request::onWrite()
{
    if (sendData_i() < 0 || !data_queue_.empty()) {
        return;
    }
    write_blocked_ = false;
    if(write_cb_) write_cb_(KMError::NOERR);
}

KMError Http2Request::close()
{
    if (conn_) {
        conn_->sync([this] { close_i(); });
    }
    conn_.reset();
    loop_token_.reset();
    return KMError::NOERR;
}

void Http2Request::close_i()
{
    if (getState() == State::CONNECTING && conn_) {
        conn_->removeConnectListener(getObjId());
    }
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
}
