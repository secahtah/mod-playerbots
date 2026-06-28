/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef PLAYERBOTS_LLMCLIENT_H
#define PLAYERBOTS_LLMCLIENT_H

#include "Define.h"

#include <cstdint>
#include <string>
#include <vector>

// One chat message in the OpenAI-compatible schema. role = "system" | "user" | "assistant".
struct LlmMessage
{
    std::string role;
    std::string content;
};

// A single completion request. The originating (world/map) thread snapshots all provider settings into
// the request so the worker thread never dereferences sPlayerbotAIConfig (which the world thread may
// reassign during /reload).
struct LlmRequest
{
    std::string model;
    std::vector<LlmMessage> messages;
    uint32 maxTokens = 60;
    std::string provider;  // "openai" | "ollama" | "mock"
    std::string apiBase;
    std::string apiKey;
    uint32 timeoutMs = 8000;
};

// Result of a completion. Always check ok before using text.
struct LlmResult
{
    bool ok = false;
    std::string text;          // trimmed assistant content
    uint32 promptTokens = 0;
    uint32 completionTokens = 0;
    uint32 totalTokens = 0;
    uint32 latencyMs = 0;
    std::string error;         // populated when !ok
};

/**
 * Stateless provider abstraction. Chat() performs a BLOCKING HTTP(S) round trip and MUST be called
 * from a worker thread (never the world thread). Supports provider = "openai" | "ollama" (both via the
 * OpenAI-compatible /chat/completions endpoint) and "mock" (offline echo for testing). Uses boost::beast
 * + boost::asio::ssl for HTTPS and boost::json for parsing. No game-object access; pure I/O.
 */
class LlmClient
{
public:
    // Reads provider, apiBase, apiKey, timeoutMs from sPlayerbotAIConfig. Never throws (errors -> result.ok=false).
    static LlmResult Chat(LlmRequest const& request);
};

#endif
