/* Copyright (c) 2014-2017, Fengping Bao <jamol@live.com>
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

#ifdef KUMA_HAS_OPENSSL

#include "SslHandler.h"
#include <openssl/x509v3.h>

using namespace kuma;

KMError SslHandler::setAlpnProtocols(const AlpnProtos &protocols)
{
#if OPENSSL_VERSION_NUMBER >= 0x1000200fL && !defined(OPENSSL_NO_TLSEXT)
    if (ssl_ && SSL_set_alpn_protos(ssl_, &protocols[0], (unsigned int)protocols.size()) == 0) {
        return KMError::NOERR;
    }
    return KMError::SSL_FAILED;
#else
    return KMError::UNSUPPORT;
#endif
}

KMError SslHandler::getAlpnSelected(std::string &proto)
{
    if (!ssl_) {
        return KMError::INVALID_STATE;
    }
    const uint8_t *buf = nullptr;
    uint32_t len = 0;
    SSL_get0_alpn_selected(ssl_, &buf, &len);
    if (buf && len > 0) {
        proto.assign((const char*)buf, len);
    } else {
        proto.clear();
    }
    return KMError::NOERR;
}

KMError SslHandler::setServerName(const std::string &serverName)
{
#if OPENSSL_VERSION_NUMBER >= 0x1000105fL && !defined(OPENSSL_NO_TLSEXT)
    if (ssl_ && SSL_set_tlsext_host_name(ssl_, serverName.c_str())) {
        return KMError::NOERR;
    }
    return KMError::SSL_FAILED;
#else
    return KMError::UNSUPPORT;
#endif
}

KMError SslHandler::setHostName(const std::string &hostName)
{
    if (ssl_) {
        auto param = SSL_get0_param(ssl_);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS);
        X509_VERIFY_PARAM_set1_host(param, hostName.c_str(), hostName.size());
        return KMError::NOERR;
    }
    return KMError::SSL_FAILED;
}

#endif

