/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef PLAYERBOTS_LLMCHATMGR_H
#define PLAYERBOTS_LLMCHATMGR_H

#include "LlmChatOperation.h"
#include "LlmClient.h"
#include "ObjectGuid.h"

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

/**
 * Central orchestrator for LLM bot chat. Owns every cross-cutting concern: the async worker pool, all
 * cost/perf/audience guardrails, bot-to-bot loop control, and bounded conversation memory. The two chat
 * hook points (reply + ambient) and the world-update/logout hooks call into it; results are applied on the
 * world thread via PlayerbotWorldThreadProcessor. All shared state is mutex-guarded.
 */
class LlmChatMgr
{
public:
    static LlmChatMgr& instance()
    {
        static LlmChatMgr instance;
        return instance;
    }

    // --- lifecycle (world thread) ---
    void Update(uint32 diff);              // TTL sweep + daily budget rollover (called from WorldScript::OnUpdate)
    void Stop();                           // join workers on shutdown
    void OnPlayerLogout(ObjectGuid guid);  // drop conversation memory involving guid (player OR bot)

    bool Enabled() const;  // LlmEnabled and provider/key sane

    // Reactive reply. Returns true if an async request was dispatched (caller suppresses canned fallback);
    // false if gated/disabled (caller emits canned text). speakerIsBot drives bot-to-bot loop caps.
    bool RequestReply(Player* bot, uint32 chatType, std::string const& message, ObjectGuid speakerGuid,
                      std::string const& speakerName, std::string const& channelName, bool speakerIsBot);

    // Proactive ambient line for a bot in its current zone. Returns true if it emitted (canned) or dispatched.
    bool MaybeAmbient(Player* bot);

private:
    LlmChatMgr() = default;
    ~LlmChatMgr();
    LlmChatMgr(LlmChatMgr const&) = delete;
    LlmChatMgr& operator=(LlmChatMgr const&) = delete;

    struct Job
    {
        LlmRequest req;
        ObjectGuid botGuid;
        LlmChatOperation::Target target = LlmChatOperation::Target::ZoneChannel;
        std::string whisperTarget;
        ObjectGuid convoPlayer;  // Empty when no conversation memory applies
        bool isOllama = false;
        uint32 estTokens = 0;    // pre-charged budget estimate (for reconciliation)
    };

    struct Convo
    {
        std::deque<LlmMessage> lines;  // alternating user/assistant, trimmed
        time_t lastSeen = 0;
    };

    void EnsureWorkers();
    void Submit(Job job);
    void WorkerLoop();

    // gating helpers (under _mutex unless noted)
    bool AnyRealPlayerOnline() const;        // reads cached atomic (refreshed in Update)
    bool RealPlayerInZone(uint32 zoneId);    // reads cached set under _mutex
    bool BotOffCooldownStamp(ObjectGuid botGuid, time_t now);
    bool ZoneOffCooldownStamp(uint32 zoneId, time_t now);
    bool WhisperThrottleOk(ObjectGuid speaker, time_t now);  // per-player whisper rate limit
    bool ReserveSlot(uint32 estTokens, bool isOllama);  // budget + RPM (OpenAI) / concurrency (Ollama)
    void ReleaseOllamaSlot();
    void RecordUsage(uint32 estTokens, uint32 totalTokens, uint32 latencyMs);
    bool OllamaSuppressed() const;
    void RefreshAudienceAndBlocklist();      // world thread (Update)
    bool Blocked(std::string const& text) const;  // output denylist check

    // conversation memory (under _mutex)
    void Remember(ObjectGuid bot, ObjectGuid player, std::string role, std::string text);
    std::vector<LlmMessage> HistoryFor(ObjectGuid bot, ObjectGuid player) const;
    void SweepConversations(time_t now);

    static uint32 EstimateTokens(std::vector<LlmMessage> const& messages, uint32 maxTokens);

    std::atomic<bool> _anyRealPlayer{false};   // cached audience (write world thread, read anywhere)
    std::atomic<double> _avgLatencyMs{0.0};    // Ollama rolling latency (write worker, read anywhere)

    mutable std::mutex _mutex;  // guards all state below
    std::map<std::pair<ObjectGuid, ObjectGuid>, Convo> _convos;  // (botGuid, playerGuid)
    std::unordered_map<ObjectGuid, time_t> _botCooldown;
    std::unordered_map<uint32, time_t> _zoneCooldown;
    std::unordered_map<uint32, uint32> _zoneBotTurns;  // bot-to-bot depth per zone (decays)
    std::unordered_set<uint32> _zonesWithPlayers;      // cached real-player zones (refreshed in Update)
    std::unordered_map<ObjectGuid, std::deque<time_t>> _whisperWindow;  // per-player whisper timestamps
    std::vector<std::string> _blocklist;               // lowercase output denylist (refreshed in Update)
    std::deque<time_t> _reqWindow;                     // request timestamps for RPM
    uint32 _tokensToday = 0;
    time_t _dayStart = 0;
    int _inFlight = 0;        // Ollama in-flight count

    // worker pool
    std::vector<std::thread> _workers;
    std::queue<Job> _jobs;
    std::mutex _jobMutex;
    std::condition_variable _jobCv;
    std::atomic<bool> _stop{false};
    bool _started = false;
};

#define sLlmChatMgr LlmChatMgr::instance()

#endif
