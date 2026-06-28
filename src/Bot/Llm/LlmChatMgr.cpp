/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LlmChatMgr.h"

#include "LlmPersonaMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Random.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>
#include <sstream>

namespace
{
    constexpr uint32 LLM_REFRESH_MS = 3000;  // audience/blocklist cache + bot-to-bot decay cadence
    uint32 g_refreshAccum = 0;               // world-thread only

    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // Strip chat control codes (newlines AND the WoW pipe '|' used for colour/hyperlink escapes), collapse
    // whitespace, trim, drop a single enclosing quote pair, and clamp to the 255-char chat limit. Neutralising
    // '|' prevents the model from emitting forged item links / colour codes into channels.
    std::string Sanitize(std::string const& in, uint32 maxLen = 255)
    {
        std::string out;
        out.reserve(in.size());
        bool prevSpace = false;
        for (char c : in)
        {
            if (c == '\n' || c == '\r' || c == '\t' || c == '|')
                c = ' ';
            if (c == ' ')
            {
                if (prevSpace)
                    continue;
                prevSpace = true;
            }
            else
                prevSpace = false;
            out.push_back(c);
        }
        size_t b = out.find_first_not_of(" ");
        size_t e = out.find_last_not_of(" ");
        if (b == std::string::npos)
            return "";
        out = out.substr(b, e - b + 1);
        if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
            out = out.substr(1, out.size() - 2);
        if (out.size() > maxLen)
            out = out.substr(0, maxLen);
        return out;
    }

    time_t StartOfDay(time_t now)
    {
        tm t{};
        localtime_r(&now, &t);
        t.tm_hour = 0;
        t.tm_min = 0;
        t.tm_sec = 0;
        return mktime(&t);
    }
}  // namespace

LlmChatMgr::~LlmChatMgr() { Stop(); }

bool LlmChatMgr::Enabled() const
{
    if (!sPlayerbotAIConfig.llmEnabled)
        return false;
    std::string const& provider = sPlayerbotAIConfig.llmProvider;
    if (provider == "openai" && sPlayerbotAIConfig.llmApiKey.empty())
        return false;  // refuse to "work" without a key on the paid path
    return true;
}

void LlmChatMgr::EnsureWorkers()
{
    // Caller MUST hold _jobMutex. Reachable from map threads (reply path) + world thread (ambient).
    if (_started)
        return;
    _started = true;
    _stop = false;
    uint32 n = std::max<uint32>(2, sPlayerbotAIConfig.llmOllamaMaxConcurrent + 1);
    for (uint32 i = 0; i < n; ++i)
        _workers.emplace_back([this]() { WorkerLoop(); });
}

void LlmChatMgr::Stop()
{
    {
        std::lock_guard<std::mutex> lk(_jobMutex);
        if (!_started)
            return;
        _stop = true;
    }
    _jobCv.notify_all();
    for (std::thread& t : _workers)
        if (t.joinable())
            t.join();
    _workers.clear();
    _started = false;
}

void LlmChatMgr::Submit(Job job)
{
    {
        std::lock_guard<std::mutex> lk(_jobMutex);
        EnsureWorkers();  // happens-once under the queue lock
        _jobs.push(std::move(job));
    }
    _jobCv.notify_one();
}

void LlmChatMgr::WorkerLoop()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lk(_jobMutex);
            _jobCv.wait(lk, [this]() { return _stop.load() || !_jobs.empty(); });
            if (_stop.load() && _jobs.empty())
                return;
            job = std::move(_jobs.front());
            _jobs.pop();
        }

        LlmResult res = LlmClient::Chat(job.req);

        RecordUsage(job.isOllama ? 0 : job.estTokens, res.totalTokens, res.latencyMs);
        if (job.isOllama)
            ReleaseOllamaSlot();

        if (!res.ok)
        {
            if (sPlayerbotAIConfig.llmDebug)
                LOG_INFO("playerbots", "LLM request failed: {}", res.error);
            continue;
        }

        std::string text = Sanitize(res.text);
        if (text.empty() || Blocked(text))
        {
            if (!text.empty() && sPlayerbotAIConfig.llmDebug)
                LOG_INFO("playerbots", "LLM reply dropped by blocklist");
            continue;
        }

        if (sPlayerbotAIConfig.llmDebug)
            LOG_INFO("playerbots", "LLM reply ({} tok, {}ms): {}", res.totalTokens, res.latencyMs, text);

        if (job.convoPlayer)
            Remember(job.botGuid, job.convoPlayer, "assistant", text);

        auto op = std::make_unique<LlmChatOperation>(job.botGuid, job.target, std::move(text), job.whisperTarget);
        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(op));
    }
}

void LlmChatMgr::Update(uint32 diff)
{
    if (!Enabled())
        return;

    g_refreshAccum += diff;
    if (g_refreshAccum < LLM_REFRESH_MS)
        return;
    g_refreshAccum = 0;

    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_dayStart == 0 || now >= _dayStart + 86400)
        {
            _dayStart = StartOfDay(now);
            _tokensToday = 0;
        }
        for (auto it = _zoneBotTurns.begin(); it != _zoneBotTurns.end();)
        {
            if (it->second <= 1)
                it = _zoneBotTurns.erase(it);
            else
            {
                it->second -= 1;
                ++it;
            }
        }
        SweepConversations(now);
    }

    RefreshAudienceAndBlocklist();
}

void LlmChatMgr::RefreshAudienceAndBlocklist()
{
    // World thread (called from Update). Scans online players once per LLM_REFRESH_MS.
    bool anyReal = false;
    std::unordered_set<uint32> zones;
    auto const& players = ObjectAccessor::GetPlayers();
    for (auto const& pair : players)
    {
        Player* p = pair.second;
        if (p && p->IsInWorld() && !GET_PLAYERBOT_AI(p))
        {
            anyReal = true;
            zones.insert(p->GetZoneId());
        }
    }
    _anyRealPlayer.store(anyReal, std::memory_order_relaxed);

    // Parse the denylist (safe: config is only reassigned on this same world thread during /reload).
    std::vector<std::string> bl;
    std::stringstream ss(sPlayerbotAIConfig.llmBlocklist);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        size_t b = tok.find_first_not_of(" \t");
        size_t e = tok.find_last_not_of(" \t");
        if (b != std::string::npos)
            bl.push_back(ToLower(tok.substr(b, e - b + 1)));
    }

    std::lock_guard<std::mutex> guard(_mutex);
    _zonesWithPlayers = std::move(zones);
    _blocklist = std::move(bl);
}

void LlmChatMgr::OnPlayerLogout(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(_mutex);
    for (auto it = _convos.begin(); it != _convos.end();)
    {
        if (it->first.first == guid || it->first.second == guid)
            it = _convos.erase(it);
        else
            ++it;
    }
    _botCooldown.erase(guid);
    _whisperWindow.erase(guid);
}

bool LlmChatMgr::AnyRealPlayerOnline() const { return _anyRealPlayer.load(std::memory_order_relaxed); }

bool LlmChatMgr::RealPlayerInZone(uint32 zoneId)
{
    std::lock_guard<std::mutex> guard(_mutex);
    return _zonesWithPlayers.count(zoneId) != 0;
}

bool LlmChatMgr::Blocked(std::string const& text) const
{
    std::string lower = ToLower(text);
    std::lock_guard<std::mutex> guard(_mutex);
    for (std::string const& bad : _blocklist)
        if (!bad.empty() && lower.find(bad) != std::string::npos)
            return true;
    return false;
}

bool LlmChatMgr::BotOffCooldownStamp(ObjectGuid botGuid, time_t now)
{
    auto it = _botCooldown.find(botGuid);
    if (it != _botCooldown.end() && now < it->second + (time_t)sPlayerbotAIConfig.llmBotCooldownSec)
        return false;
    _botCooldown[botGuid] = now;
    return true;
}

bool LlmChatMgr::ZoneOffCooldownStamp(uint32 zoneId, time_t now)
{
    auto it = _zoneCooldown.find(zoneId);
    if (it != _zoneCooldown.end() && now < it->second + (time_t)sPlayerbotAIConfig.llmZoneCooldownSec)
        return false;
    _zoneCooldown[zoneId] = now;
    return true;
}

bool LlmChatMgr::WhisperThrottleOk(ObjectGuid speaker, time_t now)
{
    uint32 perMin = sPlayerbotAIConfig.llmWhisperPerPlayerPerMin;
    if (perMin == 0)
        return true;  // unlimited
    std::deque<time_t>& w = _whisperWindow[speaker];
    while (!w.empty() && w.front() + 60 <= now)
        w.pop_front();
    if (w.size() >= perMin)
        return false;
    w.push_back(now);
    return true;
}

bool LlmChatMgr::OllamaSuppressed() const
{
    return sPlayerbotAIConfig.llmProvider == "ollama" &&
           _avgLatencyMs.load(std::memory_order_relaxed) > (double)sPlayerbotAIConfig.llmOllamaTargetLatencyMs;
}

bool LlmChatMgr::ReserveSlot(uint32 estTokens, bool isOllama)
{
    time_t now = time(nullptr);
    if (isOllama)
    {
        if (_inFlight >= (int)sPlayerbotAIConfig.llmOllamaMaxConcurrent)
            return false;  // drop, do not queue
        ++_inFlight;
        return true;
    }

    // OpenAI: daily token budget (hard, pre-charged) + requests-per-minute.
    uint32 budget = sPlayerbotAIConfig.llmDailyTokenBudget;
    if (budget > 0 && _tokensToday + estTokens > budget)
        return false;

    while (!_reqWindow.empty() && _reqWindow.front() + 60 <= now)
        _reqWindow.pop_front();
    if (_reqWindow.size() >= sPlayerbotAIConfig.llmRequestsPerMin)
        return false;
    _reqWindow.push_back(now);
    _tokensToday += estTokens;  // pre-charge so the cap can't be overshot by in-flight requests
    return true;
}

void LlmChatMgr::ReleaseOllamaSlot()
{
    std::lock_guard<std::mutex> guard(_mutex);
    if (_inFlight > 0)
        --_inFlight;
}

void LlmChatMgr::RecordUsage(uint32 estTokens, uint32 totalTokens, uint32 latencyMs)
{
    std::lock_guard<std::mutex> guard(_mutex);
    // reconcile the pre-charged estimate with actual usage
    if (_tokensToday >= estTokens)
        _tokensToday -= estTokens;
    else
        _tokensToday = 0;
    _tokensToday += totalTokens;

    double cur = _avgLatencyMs.load(std::memory_order_relaxed);
    double nv = cur <= 0.0 ? (double)latencyMs : (cur * 0.7 + (double)latencyMs * 0.3);
    _avgLatencyMs.store(nv, std::memory_order_relaxed);
}

void LlmChatMgr::Remember(ObjectGuid bot, ObjectGuid player, std::string role, std::string text)
{
    if (!sPlayerbotAIConfig.llmHistoryEnabled || !player)
        return;
    uint32 cap = sPlayerbotAIConfig.llmHistoryMaxCharsPerMsg;
    if (text.size() > cap)
        text = text.substr(0, cap);

    std::lock_guard<std::mutex> guard(_mutex);
    Convo& c = _convos[{bot, player}];
    c.lines.push_back(LlmMessage{std::move(role), std::move(text)});
    size_t maxLines = (size_t)sPlayerbotAIConfig.llmHistoryMaxExchanges * 2;
    while (c.lines.size() > maxLines)
        c.lines.pop_front();
    c.lastSeen = time(nullptr);

    uint32 maxConvos = sPlayerbotAIConfig.llmHistoryMaxConversations;
    if (maxConvos > 0 && _convos.size() > maxConvos)
    {
        auto oldest = _convos.begin();
        for (auto it = _convos.begin(); it != _convos.end(); ++it)
            if (it->second.lastSeen < oldest->second.lastSeen)
                oldest = it;
        if (oldest->first != std::make_pair(bot, player))
            _convos.erase(oldest);
    }
}

std::vector<LlmMessage> LlmChatMgr::HistoryFor(ObjectGuid bot, ObjectGuid player) const
{
    std::vector<LlmMessage> out;
    if (!sPlayerbotAIConfig.llmHistoryEnabled || !player)
        return out;
    std::lock_guard<std::mutex> guard(_mutex);
    auto it = _convos.find({bot, player});
    if (it == _convos.end())
        return out;
    out.assign(it->second.lines.begin(), it->second.lines.end());
    return out;
}

void LlmChatMgr::SweepConversations(time_t now)
{
    time_t ttl = (time_t)sPlayerbotAIConfig.llmHistoryTtlSec;
    for (auto it = _convos.begin(); it != _convos.end();)
    {
        if (now - it->second.lastSeen > ttl)
            it = _convos.erase(it);
        else
            ++it;
    }
}

uint32 LlmChatMgr::EstimateTokens(std::vector<LlmMessage> const& messages, uint32 maxTokens)
{
    size_t chars = 0;
    for (LlmMessage const& m : messages)
        chars += m.content.size() + m.role.size() + 4;
    return (uint32)(chars / 4) + maxTokens;
}

bool LlmChatMgr::RequestReply(Player* bot, uint32 chatType, std::string const& message, ObjectGuid speakerGuid,
                              std::string const& speakerName, std::string const& channelName, bool speakerIsBot)
{
    if (!Enabled() || !bot)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    bool isWhisper = (chatType == CHAT_MSG_WHISPER);
    if (isWhisper && !sPlayerbotAIConfig.llmReplyToWhispers)
        return false;

    // Route the reply back to the source channel; skip channels we don't speak in (canned handles those).
    LlmChatOperation::Target target = LlmChatOperation::Target::ZoneChannel;
    if (!isWhisper)
    {
        ChatChannelSource src = botAI->GetChatChannelSource(bot, chatType, channelName);
        if (src == SRC_WORLD)
            target = LlmChatOperation::Target::World;
        else if (src == SRC_GENERAL)
            target = LlmChatOperation::Target::ZoneChannel;
        else
            return false;  // whisper handled above; other sources fall back to canned
    }

    bool const isOllama = sPlayerbotAIConfig.llmProvider == "ollama";
    uint32 zoneId = bot->GetZoneId();
    time_t now = time(nullptr);

    if (isOllama && OllamaSuppressed())
        return false;

    // --- gates ---
    if (isWhisper)
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (!WhisperThrottleOk(speakerGuid, now))
            return false;
    }
    else
    {
        if (sPlayerbotAIConfig.llmEnabledOnlyWithPlayers && !AnyRealPlayerOnline())
            return false;

        if (speakerIsBot)
        {
            // bot-to-bot: only with a PC in the zone, capped depth + decaying chance.
            if (!RealPlayerInZone(zoneId))
                return false;
            std::lock_guard<std::mutex> guard(_mutex);
            auto it = _zoneBotTurns.find(zoneId);
            uint32 turns = it == _zoneBotTurns.end() ? 0 : it->second;
            uint32 maxTurns = std::max<uint32>(1, sPlayerbotAIConfig.llmBotToBotMaxTurns);
            if (turns >= maxTurns)
                return false;
            uint32 chance = sPlayerbotAIConfig.llmBotToBotChance * (maxTurns - turns) / maxTurns;
            if (urand(0, 99) >= chance)
                return false;
            if (!BotOffCooldownStamp(bot->GetGUID(), now))
                return false;
            _zoneBotTurns[zoneId] = turns + 1;  // consume a turn only once all gates pass
        }
        else
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (!BotOffCooldownStamp(bot->GetGUID(), now))
                return false;
        }
    }

    // --- build prompt ---
    std::vector<LlmMessage> messages;
    messages.push_back({"system", sLlmPersonaMgr.BuildSystemPrompt(bot)});
    if (isWhisper)
        for (LlmMessage const& h : HistoryFor(bot->GetGUID(), speakerGuid))
            messages.push_back(h);

    std::string attitude = "neutral";
    if (Player* speaker = ObjectAccessor::FindPlayer(speakerGuid))
        attitude = sLlmPersonaMgr.AttitudeTag(bot->getRace(), speaker->getRace());

    std::string user = "[" + (speakerName.empty() ? std::string("someone") : speakerName) + "; you feel " + attitude +
                       " toward them] " + message;
    messages.push_back({"user", user});

    // --- reserve + dispatch ---
    uint32 est = EstimateTokens(messages, sPlayerbotAIConfig.llmMaxTokens);
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (!ReserveSlot(est, isOllama))
            return false;
    }

    if (isWhisper)
        Remember(bot->GetGUID(), speakerGuid, "user", message);

    Job job;
    job.req.model = sPlayerbotAIConfig.llmModel;
    job.req.maxTokens = sPlayerbotAIConfig.llmMaxTokens;
    job.req.messages = std::move(messages);
    job.req.provider = sPlayerbotAIConfig.llmProvider;
    job.req.apiBase = sPlayerbotAIConfig.llmApiBase;
    job.req.apiKey = sPlayerbotAIConfig.llmApiKey;
    job.req.timeoutMs = sPlayerbotAIConfig.llmTimeoutMs;
    job.botGuid = bot->GetGUID();
    job.target = isWhisper ? LlmChatOperation::Target::Whisper : target;
    job.whisperTarget = isWhisper ? speakerName : "";
    job.convoPlayer = isWhisper ? speakerGuid : ObjectGuid::Empty;
    job.isOllama = isOllama;
    job.estTokens = est;
    Submit(std::move(job));
    return true;
}

bool LlmChatMgr::MaybeAmbient(Player* bot)
{
    if (!Enabled() || !bot)
        return false;
    if (sPlayerbotAIConfig.llmEnabledOnlyWithPlayers && !AnyRealPlayerOnline())
        return false;

    uint32 zoneId = bot->GetZoneId();
    if (!RealPlayerInZone(zoneId))
        return false;

    bool const isOllama = sPlayerbotAIConfig.llmProvider == "ollama";
    if (isOllama && OllamaSuppressed())
        return false;

    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (!ZoneOffCooldownStamp(zoneId, now))
            return false;
        if (!BotOffCooldownStamp(bot->GetGUID(), now))
            return false;
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    bool flavored = sPlayerbotAIConfig.llmZoneFlavorEnabled && sLlmPersonaMgr.HasZoneFlavor(zoneId);

    // Free canned meme path (no LLM call) - emit directly on this (world) thread.
    if (flavored && urand(0, 99) < sPlayerbotAIConfig.llmZoneCannedChance)
    {
        std::string line = Sanitize(sLlmPersonaMgr.RandomCannedLine(zoneId));
        if (!line.empty())
        {
            botAI->SayToChannel(line, ChatChannelId::GENERAL);
            return true;
        }
    }

    // LLM-generated ambient line.
    std::vector<LlmMessage> messages;
    std::string sys = sLlmPersonaMgr.BuildSystemPrompt(bot);
    if (flavored)
    {
        std::string flavor = sLlmPersonaMgr.ZoneFlavor(zoneId);
        if (!flavor.empty())
            sys += " " + flavor;
    }
    messages.push_back({"system", sys});
    messages.push_back({"user", "Say one short, in-character line for General chat. No quotes."});

    uint32 est = EstimateTokens(messages, sPlayerbotAIConfig.llmMaxTokens);
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (!ReserveSlot(est, isOllama))
            return false;
    }

    Job job;
    job.req.model = sPlayerbotAIConfig.llmModel;
    job.req.maxTokens = sPlayerbotAIConfig.llmMaxTokens;
    job.req.messages = std::move(messages);
    job.req.provider = sPlayerbotAIConfig.llmProvider;
    job.req.apiBase = sPlayerbotAIConfig.llmApiBase;
    job.req.apiKey = sPlayerbotAIConfig.llmApiKey;
    job.req.timeoutMs = sPlayerbotAIConfig.llmTimeoutMs;
    job.botGuid = bot->GetGUID();
    job.target = LlmChatOperation::Target::ZoneChannel;
    job.convoPlayer = ObjectGuid::Empty;
    job.isOllama = isOllama;
    job.estTokens = est;
    Submit(std::move(job));
    return true;
}
