# LLM-Driven Bot Chat — Setup Guide

Bots can hold in-character conversations using an LLM: ambient zone/global chatter, replies to
players, capped bot-to-bot banter, and whisper answers — with personalities driven by race, class,
and faction attitudes. It is **off by default** and works with **OpenAI** (cloud) or **Ollama** (local).

> Full design + rationale: [`docs/superpowers/specs/2026-06-28-llm-bot-chat-design.md`](superpowers/specs/2026-06-28-llm-bot-chat-design.md).

## Quick start

All settings live in `playerbots.conf` (prefix `AiPlayerbot.Llm*`). Three DB migrations are applied
automatically:

- `…/2026_06_28_00_ai_playerbot_llm_chat.sql` — race/class personas, faction attitudes, and a **Barrens
  (zone 17)** era-meme pool.
- `…/2026_06_28_01_ai_playerbot_llm_bg_callout.sql` — objective-aware **battleground callouts** (SotA, WSG, AB, AV).
- `…/2026_06_28_02_ai_playerbot_llm_zone_corpus.sql` — ambient **overworld chatter** for 26 zones (capitals,
  DK start, Outland, Northrend, classic-nostalgia zones, Wintergrasp) from the curated WotLK chat corpus.

### Option A — OpenAI (cloud, cheap)

```ini
AiPlayerbot.LlmEnabled = 1
AiPlayerbot.LlmProvider = "openai"
AiPlayerbot.LlmApiBase = "https://api.openai.com/v1"
AiPlayerbot.LlmApiKey = "sk-...your key..."
AiPlayerbot.LlmModel = "gpt-4o-mini"
AiPlayerbot.LlmDailyTokenBudget = 200000   # hard daily cap; resets at server midnight
AiPlayerbot.LlmRequestsPerMin = 20
```

### Option B — Ollama (local, free)

Install [Ollama](https://ollama.com), pull a model, and point the module at its OpenAI-compatible API.

```ini
AiPlayerbot.LlmEnabled = 1
AiPlayerbot.LlmProvider = "ollama"
AiPlayerbot.LlmApiBase = "http://localhost:11434/v1"
AiPlayerbot.LlmModel = "llama3.1:8b-instruct-q4_K_M"
AiPlayerbot.LlmOllamaMaxConcurrent = 1       # a single GPU serializes; excess is dropped, not queued
AiPlayerbot.LlmOllamaTargetLatencyMs = 3000  # if avg latency exceeds this, ambient is suppressed
```

The daily token budget is ignored for Ollama (it's free); only the concurrency + latency throttles apply.

#### Recommended Ollama models for an RTX 3090 (24 GB)

| Model | VRAM | Speed | Notes |
|---|---|---|---|
| `llama3.1:8b-instruct-q4_K_M` ✅ | ~5 GB | ~70–90 tok/s | Default: fast, great instruction-following, huge headroom |
| `qwen2.5:14b-instruct-q4_K_M` | ~9 GB | ~35–45 tok/s | Richer persona nuance, still comfortable |
| `qwen2.5:7b-instruct-q4_K_M` / `llama3.2:3b` | ~2–5 GB | very high | Max throughput for heavy ambient |

Tune Ollama for short, low-latency replies (e.g. `num_ctx 2048`, `num_predict 64`, `keep_alive` to keep
the model resident). At concurrency 1 the 8B model returns short lines in well under a second.

### Option C — Mock (offline test, no spend)

`AiPlayerbot.LlmProvider = "mock"` echoes a canned reply so you can exercise the gates/flow with zero cost.

## How bots behave

- **Whispers** — a bot answers in-character (with short conversation memory), throttled per player.
- **Reactive** — bots may reply in **General**/**World** chat to messages there.
- **Ambient** — bots occasionally post a line in their zone's General channel, **only when a real player is in
  that zone** and not too often (zone/bot cooldowns). 26 zones ship with curated chatter; see below.
- **Bot-to-bot** — bots banter with each other, but only with a player present, depth-capped and decaying.
- **Battleground callouts** — inside SotA/WSG/AB/AV, bots react to live objectives ("Ramming the gate!",
  "Capped Lumber Mill!", "EFC at mid!") and can chat in BGs. See [In-battleground callouts](#in-battleground-callouts).

### Personalities

A bot's prompt is assembled from its **race + class traits** and its **attitude toward the speaker**
(Horde↔Alliance hostility, elves aloof toward humans, ~everyone revolted by the Forsaken, etc.). Edit the
DB tables and `.reload config` (or restart) to change them — no recompile:

- `playerbot_llm_persona` — `(kind 'race'|'class', id, locale, trait_text)`
- `playerbot_llm_attitude` — `(subject_race, object_race, attitude_tag)` overrides; faction/undead/same-race
  fallbacks are computed in code, so you only seed the nuances.

### Zone flavor & era memes (e.g. Barrens ~2005)

Two optional tables give a zone its own vibe, with most lines posted **free** (no tokens) from a verbatim pool:

- `playerbot_llm_zone_flavor` — `(zone_id, locale, flavor_text)`: a short style hint for LLM ambient lines.
- `playerbot_llm_zone_canned` — `(zone_id, locale, line, weight)`: verbatim lines (Mankrik's wife, Chuck
  Norris, Tauren puns, Thunderfury, WC LFG…). `LlmZoneCannedChance` controls the free-vs-LLM mix.

`…_02_ai_playerbot_llm_zone_corpus.sql` ships ~180 such lines across 26 zones (Dalaran, Stormwind, Orgrimmar,
Ironforge, Shattrath, Undercity, the DK start, Hellfire/Zangarmarsh/Nagrand/Terokkar, every Northrend leveling
zone, classic-nostalgia alt zones, and Wintergrasp). Add a zone by inserting rows with its `zone_id` — no code
change. The pool is **social-only**: no commerce/gold spam, no slurs or targeted harassment, nothing sexual —
keep additions to that standard, since bot lines are indistinguishable from real players.

## In-battleground callouts

Bots in **Strand of the Ancients, Warsong Gulch, Arathi Basin, and Alterac Valley** announce what's happening
on the objectives. Two paths feed it:

- **Proactive** — a low-priority `bg announce` action (relevance `0.5`, never preempts real BG play) fires on a
  timer and picks a situation from the bot's state (driving a demolisher → `ramming`; carrying a flag →
  `flag_carry`; otherwise a taunt/incoming/defend).
- **Event-driven** — `LlmBgEvents` diffs each BG's objective state every server tick and emits the matching
  callout the moment something changes: SotA `gate_down`/`relic_exposed`, WSG `enemy_has_flag`/`flag_capped`/
  `flag_returned`, AB `node_captured`/`node_lost`, AV `tower_captured`/`gy_captured`/`reinforcements_low`.

Each callout is a row in `playerbot_llm_bg_callout` `(bg_zone_id, situation, locale, line, channel, weight)`:

- `bg_zone_id` — `4384` SotA, `3277` WSG, `3358` AB, `2597` AV. **`0` = any BG** (a fallback used when no
  BG-specific row matches).
- `situation` — the event/state key (e.g. `gate_down`, `enemy_has_flag`, `node_captured`, `taunt`).
- `channel` — `bg` posts to **team raid chat** (coordination); `say` posts to local **/say** (taunts, public).
- `line` — the text. `<loc>` is substituted at runtime with the specific gate/node/graveyard name.

Like ambient chatter, each callout is posted **verbatim** part of the time and used as the **LLM seed** the
rest (so the model reflavors the same topical line in the bot's persona). The BG's own voice comes from a
`playerbot_llm_zone_flavor` row keyed by the BG zone id.

**To add or retune a callout**, insert/edit a `playerbot_llm_bg_callout` row and `.reload config` (or restart) —
no recompile. To add an *entirely new* situation, also emit it from `LlmBgEvents::Update` (event-driven) or from
`BattleGroundTactics::announce` (proactive).

### BG callout tuning

```ini
AiPlayerbot.LlmBgEnabled        = 1    # master switch for BG callouts
AiPlayerbot.LlmBgCannedChance   = 50   # % posted verbatim vs LLM-reflavored (rest)
AiPlayerbot.LlmBgBotCooldownSec = 30   # min seconds between callouts from the same bot
AiPlayerbot.LlmBgEventCooldownSec = 15 # min seconds between callouts for the same (BG, situation)
AiPlayerbot.LlmBgAnnounceChance = 25   # % chance the proactive timer actually speaks
```

These BG rate limits are **separate from** the global ambient/whisper limits, so BG chatter can be tuned
independently. LLM-reflavored BG replies always go to **team-only** chat (never public `/say`).

## Guardrails & cost control

Every LLM call passes a layered gate: **player present** (global + per-zone) → **cooldowns** (per-zone, per-bot,
per-player whisper) → **rate** (OpenAI requests/min, or Ollama concurrency + latency backoff) → **daily token
budget** (hard, pre-charged). `AiPlayerbot.LlmEnabledOnlyWithPlayers = 1` means **zero spend when no real
players are online**. See `playerbots.conf.dist` for every knob.

## Safety notes

- The API key is read only from config, never logged, and sent **only over HTTPS** (peer-verified).
- LLM output is sanitized (newlines + the `|` chat-escape stripped, clamped to 255 chars) and screened
  against `AiPlayerbot.LlmBlocklist`. This is best-effort moderation on top of the model's own safety — review
  it before enabling on a public realm, since bot lines are indistinguishable from real players.
- If an LLM request fails (timeout/error/blocked), the bot stays silent for that line rather than falling back.
