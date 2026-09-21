// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "WowLegendsAiHttp.h"
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <string>
#include <utility>

namespace WowLegendsAiHttp
{
// Same synchronous transport entry point for production sockets and offline mocks.
// Requests use Connection: close. Only EOF, or a fully framed TLS response whose
// peer omitted close_notify, may terminate a successful response.
template <typename Stream>
std::string ReadHttpBody(Stream& stream)
{
    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    std::string response;
    boost::system::error_code readError;
    char buffer[8192];
    for (;;)
    {
        std::size_t n = stream.read_some(asio::buffer(buffer), readError);
        if (n > sizeof(buffer) || !CanAppend(response.size(), n))
            return {};
        if (n) response.append(buffer, n);
        if (readError) break;
        if (!n) return {}; // No-progress stream must not spin indefinitely.
    }
    bool tlsTruncated = readError == asio::ssl::error::stream_truncated;
    if (readError != asio::error::eof && !tlsTruncated)
        return {};
    if (!IsSuccess(response)) return {};

    http::response_parser<http::string_body> parser;
    parser.body_limit(MaxResponseBytes);
    parser.header_limit(64 * 1024);
    parser.eager(true);
    boost::system::error_code parseError;
    std::size_t consumed = parser.put(asio::buffer(response), parseError);
    if (parseError && parseError != http::error::need_more) return {};
    if (consumed != response.size()) return {};
    if (!parser.is_header_done()) return {};
    // A close-delimited TLS body cannot be authenticated as complete when the
    // TLS session ended without close_notify. Content-Length/chunked can be.
    if (tlsTruncated && (!parser.is_done() || parser.need_eof())) return {};
    if (!parser.is_done())
    {
        parseError.clear();
        parser.put_eof(parseError);
        if (parseError || !parser.is_done()) return {};
    }
    if (parser.get().result_int() < 200 || parser.get().result_int() >= 300)
        return {};
    return std::move(parser.get().body());
}
}
