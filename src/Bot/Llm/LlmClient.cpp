/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LlmClient.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/json/src.hpp>  // header-only boost::json impl (this is the ONLY TU that pulls it)

#include <chrono>
#include <cstdint>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

namespace
{
    struct ParsedUrl
    {
        bool https = true;
        std::string host;
        std::string port;
        std::string pathPrefix;  // e.g. "/v1"
        bool ok = false;
    };

    ParsedUrl ParseBase(std::string const& base)
    {
        ParsedUrl u;
        std::string s = base;
        if (s.rfind("https://", 0) == 0)
        {
            u.https = true;
            s = s.substr(8);
        }
        else if (s.rfind("http://", 0) == 0)
        {
            u.https = false;
            s = s.substr(7);
        }
        else
            return u;  // unknown scheme

        // split host[:port] / path
        std::string hostport = s;
        std::string path;
        size_t slash = s.find('/');
        if (slash != std::string::npos)
        {
            hostport = s.substr(0, slash);
            path = s.substr(slash);
        }
        // strip trailing slash from path prefix
        while (!path.empty() && path.back() == '/')
            path.pop_back();
        u.pathPrefix = path;

        size_t colon = hostport.find(':');
        if (colon != std::string::npos)
        {
            u.host = hostport.substr(0, colon);
            u.port = hostport.substr(colon + 1);
        }
        else
        {
            u.host = hostport;
            u.port = u.https ? "443" : "80";
        }
        u.ok = !u.host.empty();
        return u;
    }

    std::string Trim(std::string const& in)
    {
        size_t b = in.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        size_t e = in.find_last_not_of(" \t\r\n");
        return in.substr(b, e - b + 1);
    }

    // Build the OpenAI-compatible chat-completions request body.
    std::string BuildBody(LlmRequest const& req)
    {
        json::array messages;
        for (LlmMessage const& m : req.messages)
        {
            json::object o;
            o["role"] = m.role;
            o["content"] = m.content;
            messages.push_back(std::move(o));
        }
        json::object body;
        body["model"] = req.model;
        body["messages"] = std::move(messages);
        body["max_tokens"] = static_cast<std::int64_t>(req.maxTokens);
        body["temperature"] = 0.8;
        body["stream"] = false;
        return json::serialize(body);
    }

    // Parse choices[0].message.content + usage tokens out of an OpenAI-compatible response.
    void ParseResponse(std::string const& payload, LlmResult& out)
    {
        json::value v = json::parse(payload);
        if (!v.is_object())
            return;
        json::object const& root = v.as_object();

        if (auto const* choices = root.if_contains("choices"); choices && choices->is_array() &&
                                                                !choices->as_array().empty())
        {
            json::value const& first = choices->as_array().front();
            if (first.is_object())
            {
                if (auto const* msg = first.as_object().if_contains("message"); msg && msg->is_object())
                {
                    if (auto const* content = msg->as_object().if_contains("content"); content && content->is_string())
                        out.text = Trim(std::string(content->as_string().c_str()));
                }
            }
        }

        if (auto const* usage = root.if_contains("usage"); usage && usage->is_object())
        {
            json::object const& u = usage->as_object();
            auto getInt = [&](char const* k) -> uint32
            {
                if (auto const* p = u.if_contains(k); p && p->is_int64())
                    return static_cast<uint32>(p->as_int64());
                return 0;
            };
            out.promptTokens = getInt("prompt_tokens");
            out.completionTokens = getInt("completion_tokens");
            out.totalTokens = getInt("total_tokens");
        }

        out.ok = !out.text.empty();
        if (!out.ok)
            out.error = "empty completion";
    }

    template <class Stream>
    void DoRequest(Stream& stream, ParsedUrl const& url, std::string const& body, std::string const& apiKey,
                   LlmResult& out)
    {
        std::string target = url.pathPrefix + "/chat/completions";
        http::request<http::string_body> req{http::verb::post, target, 11};
        req.set(http::field::host, url.host);
        req.set(http::field::user_agent, "mod-playerbots-llm/1.0");
        req.set(http::field::content_type, "application/json");
        req.set(http::field::accept, "application/json");
        if (!apiKey.empty())
            req.set(http::field::authorization, "Bearer " + apiKey);
        req.body() = body;
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        unsigned status = res.result_int();
        if (status < 200 || status >= 300)
        {
            out.ok = false;
            out.error = "http " + std::to_string(status);
            return;
        }
        ParseResponse(res.body(), out);
    }
}  // namespace

LlmResult LlmClient::Chat(LlmRequest const& request)
{
    LlmResult out;
    auto t0 = std::chrono::steady_clock::now();

    // Everything comes from the request snapshot - never touch sPlayerbotAIConfig on this worker thread.
    std::string const& provider = request.provider;
    std::string const& base = request.apiBase;
    std::string const& apiKey = request.apiKey;
    uint32 timeoutMs = request.timeoutMs;

    // Offline echo backend for testing without spend.
    if (provider == "mock")
    {
        std::string last;
        for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it)
        {
            if (it->role == "user")
            {
                last = it->content;
                break;
            }
        }
        out.text = last.empty() ? "..." : ("(mock) " + last.substr(0, 80));
        out.promptTokens = 10;
        out.completionTokens = 10;
        out.totalTokens = 20;
        out.ok = true;
        out.latencyMs = 0;
        return out;
    }

    ParsedUrl url = ParseBase(base);
    if (!url.ok)
    {
        out.error = "bad LlmApiBase: " + base;
        return out;
    }

    try
    {
        std::string body = BuildBody(request);
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        auto const results = resolver.resolve(url.host, url.port);

        if (url.https)
        {
            ssl::context ctx(ssl::context::tls_client);
            ctx.set_default_verify_paths();
            ctx.set_verify_mode(ssl::verify_peer);

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
            // SNI + hostname verification (transmits the API key, so verify the peer).
            if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()))
            {
                out.error = "SNI failed";
                return out;
            }
            stream.set_verify_callback(ssl::host_name_verification(url.host));

            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeoutMs));
            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(ssl::stream_base::client);

            DoRequest(stream, url, body, apiKey, out);

            beast::error_code ec;
            stream.shutdown(ec);  // ignore shutdown errors (servers often close abruptly)
        }
        else
        {
            beast::tcp_stream stream(ioc);
            stream.expires_after(std::chrono::milliseconds(timeoutMs));
            stream.connect(results);

            // Never transmit the bearer secret over a plaintext (non-TLS) connection.
            DoRequest(stream, url, body, std::string(), out);

            beast::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        }
    }
    catch (std::exception const& e)
    {
        out.ok = false;
        out.error = std::string("exception: ") + e.what();
    }
    catch (...)
    {
        out.ok = false;
        out.error = "unknown exception";
    }

    auto t1 = std::chrono::steady_clock::now();
    out.latencyMs = static_cast<uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}
