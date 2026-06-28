# Design Spec — LLM-Driven Bot Chat

**Date:** 2026-06-28
**Status:** Approved design (pre-implementation)
**Module:** `mod-playerbots`

## 1. Goal & Scope

Give bots LLM-generated chat so a populated server feels alive:

- **Zone/global channel chatter** — reactive (replies to players), proactive ambient lines, and **capped bot-to-bot** conversation.
- **Whisper replies** — a bot answers a player's whisper in-character.
- **Pluggable provider** — OpenAI (cheap cloud model) or **Ollama** (local, free).
- **Personality from DB-authored prompts** — a bot responds based on its own race/class *and* the speaker's race/class/faction (Horde↔Alliance hostility, elves aloof toward humans, ~everyone revolted by undead, etc.). Prompts kept **token-minimal**.
- **Zone flavor / era memes** — optional per-zone chatter style, with a free verbatim "canned" pool for iconic running gags. Flagship example: **Barrens General ~2005** (Mankrik's wife, Chuck Norris jokes, Tauren puns, Thunderfury spam, WC LFG).
- **Hard cost/perf guardrails** — daily token budget, provider-aware rate/concurrency limits, per-zone audience + cooldown gates, bot-to-bot loop caps, and a "no audience → no spend" master switch.

**Non-goals (YAGNI):** tool/function calling, long-term memory, party/raid/guild LLM chat, streaming, generated translation, moderation beyond blocklist + length clamp.

**Key constraint satisfied:** **zero core-fork edits.** Config, HTTP (boost::beast + OpenSSL, already linked), threading (existing task queue), and singleton patterns all exist in-module.

## 2. Architecture (Option A — manager-centric + minimal hook swaps)

A new singleton `LlmChatMgr` owns everything cross-cutting; the two existing chat-generation points call into it; results are applied back on the world thread via the existing task queue. When LLM is unavailable for any reason, behavior **degrades silently to today's canned chatter**.

### 2.1 Components (each: one responsibility)

| Component | Location | Responsibility |
|---|---|---|
| **`LlmChatMgr`** (singleton `sLlmChatMgr`) | `src/Bot/Llm/LlmChatMgr.{h,cpp}` | Orchestrator + all gates (budget, rate, concurrency, zone cooldown, audience, loop) + conversation memory + dispatch. Mutex-guarded global state. |
| **`LlmClient`** | `src/Bot/Llm/LlmClient.{h,cpp}` | Provider abstraction. Async HTTP(S) via boost::beast/OpenSSL. One code path using the **OpenAI-compatible `/chat/completions`** for both OpenAI and Ollama. Returns `{text, promptTokens, completionTokens, latencyMs, ok}`. |
| **`LlmPersonaMgr`** | `src/Bot/Llm/LlmPersonaMgr.{h,cpp}` | Loads DB persona/attitude tables (boot + `/reload`, locale-aware). Assembles the compact prompt for a (bot, speaker) pair. |
| **`LlmChatOperation`** | `src/Bot/Llm/LlmChatOperation.{h,cpp}` | `PlayerbotOperation` subclass. Applies the result on the **world thread** (emits via `SayToChannel`/`SayToWorld`/`Whisper`). Stores **copies only**. |
| **Hook swaps** | existing files | `ChatReplyAction::GenerateReplyMessage` → `LlmChatMgr::RequestReply`; new ambient trigger/action → `LlmChatMgr::MaybeAmbient`; `OnPlayerLogout` + `OnUpdate` one-liners for GC. |

### 2.2 Hook points (verified)

- **Replies:** `ChatReplyAction::GenerateReplyMessage` (SayAction.cpp:574) — currently returns canned string-table text; route through `LlmChatMgr` first, fall back to canned. `SendGeneralResponse` (SayAction.cpp:499) already routes the returned string to the right channel/whisper.
- **Proactive ambient:** a new trigger in the chat strategy (alongside `SayAction::Execute`, SayAction.cpp:134) calls `MaybeAmbient`.
- **Inbound context:** OnChat hooks (Playerbots.cpp:198–262) → `PlayerbotAI::HandleCommand` → `QueueChatResponse(ChatQueuedReply{...})` → `chatReplies` queue (PlayerbotAI.cpp:490) → `ChatReplyDo`. The async LLM result re-uses this deferred queue, so latency is a non-issue.
- **Emit:** `PlayerbotAI::SayToChannel(msg, ChatChannelId::GENERAL)` / `SayToWorld(msg)` / `Whisper(msg, name)` (PlayerbotAI.cpp:2790–2946). Output **clamped to 255 chars** + newline-stripped + sanitized.
- **GC:** `PlayerbotsPlayerScript::OnPlayerLogout` (Playerbots.cpp:472) and `OnUpdate` (Playerbots.cpp:381) each get one added line.

## 3. Data Flow (async, never blocks the world tick)

```
WORLD THREAD (bot tick or OnChat)
  LlmChatMgr::Gate(...)  -> audience(zone), zone cooldown, bot cooldown,
                            rate/concurrency (provider-aware), daily budget,
                            bot-to-bot loop depth/chance
  build LlmRequest{ botGuid, target(channel|whisperName), messages[] }   // copies only
  reserve estimated tokens; mark in-flight slot
  enqueue to LlmClient worker pool
WORKER THREAD (boost::asio io_context)
  POST {base}/chat/completions  (timeout LlmTimeoutMs)
  parse text + usage.total_tokens + latency
  LlmChatMgr::RecordUsage(actualTokens, latency); release slot
  new LlmChatOperation{ botGuid, target, clamp(text) }
  PlayerbotWorldThreadProcessor::QueueOperation(...)
WORLD THREAD (drain @ Playerbots.cpp:381)
  Operation.Execute(): ObjectAccessor bot by guid (skip if gone)
                       -> SayToChannel / SayToWorld / Whisper
```

**Bot-to-bot:** a bot's emitted zone line is heard via the normal inbound path; `LlmChatMgr` may schedule a reply from another bot in that zone, subject to the loop cap (below).

## 4. Guardrails (all enforced in `LlmChatMgr` before any API call)

| Gate | Rule | Applies to |
|---|---|---|
| **Master audience switch** | `LlmEnabledOnlyWithPlayers=1`: if zero non-bot players online, no ambient/bot-to-bot at all | ambient, bot-to-bot |
| **Per-zone audience** | zone chatter only if ≥1 real PC shares the bot's `GetZoneId()` (zone channels are already zone-scoped) | ambient, bot-to-bot |
| **Per-zone cooldown** | a zone chats at most every `LlmZoneCooldownSec` (+jitter) — "not often" | ambient, bot-to-bot |
| **Per-bot cooldown** | a bot speaks at most every `LlmBotCooldownSec` | ambient, bot-to-bot, replies |
| **OpenAI rate** | global `LlmRequestsPerMin` cap (sliding window) | OpenAI only |
| **Ollama concurrency** | `LlmOllamaMaxConcurrent` in-flight (default 1); excess **dropped, not queued** | Ollama only |
| **Ollama adaptive backoff** | rolling avg latency > `LlmOllamaTargetLatencyMs` → suppress ambient/bot-to-bot (whispers only) until recovered | Ollama only |
| **Daily token budget** | skip once `LlmDailyTokenBudget` consumed; resets at server midnight | OpenAI (Ollama = unlimited) |
| **Bot-to-bot loop cap** | thread dies after `LlmBotToBotMaxTurns`; each hop rolls `LlmBotToBotChance` (decaying); bot lines tagged so bot→bot is capped separately from bot→player | bot-to-bot |
| **Whisper exemption** | whisper replies bypass audience/zone/ambient gates (a real player is waiting) | replies |
| **Output clamp** | ≤255 chars, strip newlines, blocklist filter | all |

A request proceeds only if **every** applicable gate passes; otherwise it silently falls back to canned chatter (replies) or is skipped (ambient).

## 5. Provider Abstraction

Single client path via the OpenAI-compatible chat-completions API (Ollama implements it at `/v1/chat/completions`).

| Config | OpenAI | Ollama (RTX 3090) |
|---|---|---|
| `LlmProvider` | `openai` | `ollama` |
| `LlmApiBase` | `https://api.openai.com/v1` | `http://localhost:11434/v1` |
| `LlmApiKey` | required (secret) | ignored |
| `LlmModel` | `gpt-4o-mini` | `llama3.1:8b-instruct-q4_K_M` (default) |
| budget | enforced (daily tokens) | unlimited |
| throttle | requests/min | max-concurrent + adaptive latency |

**Ollama model recommendation (RTX 3090, 24 GB; short in-character replies):**

- **Default — `llama3.1:8b-instruct-q4_K_M`** (~5 GB, ~70–90 tok/s, sub-second). Best balance; huge VRAM headroom.
- **Quality — `qwen2.5:14b-instruct-q4_K_M`** (~9 GB, ~35–45 tok/s, ~1–1.5 s). Richer persona nuance, still comfortable.
- **Throughput — `qwen2.5:7b-instruct-q4_K_M` / `llama3.2:3b`** for heavy ambient volume.
- Ollama options: `num_ctx 2048`, `num_predict 64`, `temperature ~0.8`, `keep_alive` to keep the model resident.
- **Rate scoping:** at concurrency 1 the 8B model yields ≥1 reply/sec single-stream; zone/bot cooldowns keep ambient infrequent, so the card is never saturated. Optionally raise `LlmOllamaMaxConcurrent` to 2 with `OLLAMA_NUM_PARALLEL=2`.

## 6. Persona / Prompt Model (token-minimal)

### 6.1 DB schema (mirrors `PlayerbotTextMgr` loading)

- **`playerbot_llm_persona`** — `(kind ENUM('race','class'), id TINYINT, locale VARCHAR, trait_text VARCHAR)`. Short trait tags, e.g. orc(race)→"gruff, honorable, blunt"; mage(class)→"arrogant, bookish".
- **`playerbot_llm_attitude`** — `(subject_race TINYINT, object_race TINYINT, attitude_tag VARCHAR)`. Compact matrix; race-pair rows override, with **faction fallback** (any-Horde↔any-Alliance → "hostile") and a default "neutral". Examples: nightelf→human "aloof"; *→undead "revolted"; same-race "kinship".
- **`playerbot_llm_zone_flavor`** — `(zone_id INT, locale VARCHAR, flavor_text VARCHAR)`. Optional per-zone "vibe" snippet injected into the **LLM ambient** prompt. e.g. Barrens (zone 17) → *"Barrens General chat ~2005: Chuck-Norris-style jokes, 'where is Mankrik's wife?', bad cow/Tauren puns, LFG Wailing Caverns. One short line, meme energy."*
- **`playerbot_llm_zone_canned`** — `(zone_id INT, locale VARCHAR, line VARCHAR(255), weight INT)`. Pool of **verbatim** era lines posted for **zero tokens** (mirrors `PlayerbotTextMgr`). Holds the pixel-perfect classics (Mankrik's wife, Chuck Norris one-liners, `[Thunderfury, Blessed Blade of the Windseeker]` link spam, "LF tank WC at Crossroads", Tauren puns). Item links carried verbatim.

Loaded at boot + `/reload`; locale-aware. Missing rows → safe neutral defaults so the feature never errors on incomplete data. Zone tables are optional — a zone with no rows just uses generic ambient.

### 6.2 Prompt assembly (kept tiny)

```
system: "{LlmSystemPrompt base} You are a {race} {class}. Traits: {raceTrait}; {classTrait}.
         Reply in-character in <=1 short sentence."
[history: last up-to-4 exchanges for this (bot,player), each trimmed]  // sec 7
user:   "[{speakerRace} {speakerClass}; you feel {attitude} toward them] {their message}"
```

`LlmSystemPrompt` (config) sets global style without DB edits. No history for ambient or first-contact; history only for ongoing whisper/zone exchanges. Hard total-token cap on the assembled prompt.

### 6.3 Zone flavor & era memes (hybrid: free canned + LLM)

When a bot does **ambient** chatter in a zone that has flavor data (`LlmZoneFlavorEnabled=1`):

1. Roll `LlmZoneCannedChance` (default 60%). On hit → post a **weighted random verbatim line** from `playerbot_llm_zone_canned` for that zone. **Zero tokens**, pixel-perfect memes, item links intact.
2. Otherwise → call the LLM with the zone's `flavor_text` appended to the system prompt, so the generated line matches the era/zone style (fresh variety).

Both paths still pass every guardrail (PC-in-zone, zone/bot cooldown, budget, rate, clamp). Net effect: a flavored zone like the **Barrens (zone 17)** feels alive while costing almost nothing, because the iconic running gags are canned. Ships with a default Barrens seed SQL; admins add other zones (Gadgetzan, Stormwind Trade, Goldshire…) by inserting rows — no code change. Reuses the existing Thunderfury item-link handling for verbatim `[item]` links.

## 7. Conversation Memory (in-memory, bounded, GC'd)

- Structure: `LlmChatMgr` holds `map<(botGuid,playerGuid), RingBuffer>`; each buffer keeps the **last `LlmHistoryMaxExchanges` (4) exchanges** (8 lines), each line capped at `LlmHistoryMaxCharsPerMsg` (~160), with a hard total-token cap.
- "Has this PC messaged before?" = buffer exists for the pair → prepend trimmed history; else single-turn.
- **Provider-scoped:** primarily for the free Ollama path; on OpenAI it adds tokens, so depth is configurable (set lower or 0 for the paid path).
- **Garbage collection (3 layers, leak-proof):**
  1. **Immediate on logout** — `LlmChatMgr::OnPlayerLogout(guid)` clears every entry where guid is the player *or* the bot (bots are `Player`s, so the same hook covers both). Added at Playerbots.cpp:472.
  2. **TTL sweep** — `LlmChatMgr::Update(diff)` (hooked at OnUpdate, Playerbots.cpp:381) expires conversations idle past `LlmHistoryTtlSec` (~600).
  3. **Hard cap + LRU** — `LlmHistoryMaxConversations` backstop evicts the oldest if exceeded.

## 8. Config Keys (`PlayerbotAIConfig.h/.cpp` + `playerbots.conf.dist`)

Add fields + `sConfigMgr->GetOption<T>(...)` reads in `PlayerbotAIConfig::Initialize()` (re-invoked on `/reload`), plus documented `.conf.dist` entries.

| Key | Default | Notes |
|---|---|---|
| `AiPlayerbot.LlmEnabled` | `0` | master on/off |
| `AiPlayerbot.LlmProvider` | `openai` | `openai`\|`ollama`\|`mock` |
| `AiPlayerbot.LlmApiBase` | `https://api.openai.com/v1` | |
| `AiPlayerbot.LlmApiKey` | `""` | secret; OpenAI only |
| `AiPlayerbot.LlmModel` | `gpt-4o-mini` | |
| `AiPlayerbot.LlmMaxTokens` | `60` | completion cap |
| `AiPlayerbot.LlmTimeoutMs` | `8000` | |
| `AiPlayerbot.LlmDailyTokenBudget` | `200000` | OpenAI; resets server-midnight |
| `AiPlayerbot.LlmRequestsPerMin` | `20` | **OpenAI only** |
| `AiPlayerbot.LlmOllamaMaxConcurrent` | `1` | **Ollama only** |
| `AiPlayerbot.LlmOllamaTargetLatencyMs` | `3000` | **Ollama** adaptive backoff |
| `AiPlayerbot.LlmBotCooldownSec` | `120` | |
| `AiPlayerbot.LlmZoneCooldownSec` | `90` | |
| `AiPlayerbot.LlmEnabledOnlyWithPlayers` | `1` | no-audience cutoff |
| `AiPlayerbot.LlmReplyToWhispers` | `1` | |
| `AiPlayerbot.LlmAmbientChance` | `5` | per eligible tick (%) |
| `AiPlayerbot.LlmZoneFlavorEnabled` | `1` | enable per-zone era flavor/canned memes |
| `AiPlayerbot.LlmZoneCannedChance` | `60` | % of flavored-zone ambient lines drawn free from the canned pool vs LLM-generated |
| `AiPlayerbot.LlmBotToBotMaxTurns` | `3` | |
| `AiPlayerbot.LlmBotToBotChance` | `15` | decaying (%) |
| `AiPlayerbot.LlmSystemPrompt` | `"You are a character in World of Warcraft. Stay terse, in-character, and never break the fourth wall or mention being an AI."` | base style template |
| `AiPlayerbot.LlmHistoryEnabled` | `1` | |
| `AiPlayerbot.LlmHistoryMaxExchanges` | `4` | |
| `AiPlayerbot.LlmHistoryMaxCharsPerMsg` | `160` | |
| `AiPlayerbot.LlmHistoryTtlSec` | `600` | |
| `AiPlayerbot.LlmHistoryMaxConversations` | `500` | LRU backstop |
| `AiPlayerbot.LlmDebug` | `0` | log prompts/usage/decisions |

## 9. Threading & Safety

- Worker: a small `boost::asio::io_context` + thread pool (precedent: `PlayerbotCommandServer.cpp:97` detaches a thread; reuse the pattern). Blocking HTTPS happens off the world thread.
- All game-object access (resolve bot, emit chat) happens **only** on the world thread inside `LlmChatOperation::Execute()`. Requests/operations carry **copies** (`ObjectGuid`, strings) — never raw `Player*`.
- `LlmChatMgr` counters (budget, rate window, in-flight, cooldown maps, conversation map) guarded by a `std::mutex`.
- Clean shutdown: stop the io_context and join workers (improve on the command server's detach-and-leak).

## 10. Error Handling & Fallback

Missing key / `LlmEnabled=0` / over-budget / no-audience / rate-or-concurrency-limited / HTTP error / timeout / empty or unsafe response → **silently fall back** to the existing canned reply tables (replies) or skip (ambient). API 429 → back off + skip. All worker exceptions caught. `LlmDebug` records the decision/prompt/usage without spamming chat.

## 11. Testing / Verification

- **Build** clean against the Playerbot fork (toolchain available).
- **`LlmProvider=mock`** echo backend → exercise gates/flow offline with zero spend.
- **Manual in-game:** whisper a bot (reply + memory across 4 turns); populated zone (ambient + bot-to-bot under caps); cross-faction flavor; verify budget/rate/concurrency cutoffs and GC via `LlmDebug` logs; verify Ollama path with `llama3.1:8b`; verify graceful fallback at `LlmEnabled=0` and when no PCs are online.

## 12. Open Questions (deferred to implementation)

- Exact race/class trait + attitude seed rows (content authoring) — ship a sensible default SQL set; admins extend.
- Whether ambient lines should occasionally reference nearby context (zone, recent kills) — start without; revisit.
- Worker pool size vs single detached thread — start with a 1–2 thread io_context.
