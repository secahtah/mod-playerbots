# Design Spec — Objective-Aware In-Battleground Bot Chat

**Date:** 2026-06-28
**Status:** Approved design (pre-implementation)
**Builds on:** the LLM bot-chat feature (`2026-06-28-llm-bot-chat-design.md`, PR #3). This MR stacks on it.

## 1. Goal

Bots in battlegrounds **announce topical objective lines** — *"Ramming the gate!"*, *"Blue Gate is down — push Red!"*, *"They capped West graveyard!"*, *"Capping the flag!"* — and can chat/reply in BGs (today they're silent there except whispers). Full dynamic treatment for **Strand of the Ancients, Warsong Gulch, Arathi Basin, Alterac Valley**; other BGs get lighter generic callouts.

Lines are **hybrid**: each situation has a canned line; `LlmBgCannedChance` % of the time it's posted verbatim (free), otherwise the LLM **reflavors that same canned line** in the bot's persona (topical + cheap, since the seed keeps it short and on-objective).

## 2. Two mechanisms (both run inside BGs; reuse the engine — no ProcessBot path)

The world-ambient path is short-circuited for BG bots (`ProcessBot` `InBattleground()` early-return), so chatter originates from the engine instead:

1. **Per-bot action callouts** — a new low-relevance `"bg announce"` `BGTactics` action, wired into each BG strategy via a throttle trigger (`timer bg`/`random`), mirroring the existing `"bg check flag"`. It reads the bot's *own* state and fires the matching situation:
   - `IsInVehicle()` + near an intact gate → `ramming`
   - `PlayerHasFlag::IsCapturingFlag(bot)` → `flag_carry`
   - `atFlag()` / node proximity → `node_capping`
2. **BG event detection** — add `OnBattlegroundUpdate(bg, diff)` to the existing `PlayerBotsBGScript` (`AllBattlegroundScript`, already registered). Cache a prev-objective-state snapshot per `bg->GetInstanceID()` (same pattern as the existing `bgStrategies` map); diff each tick; on a change, pick one bot on the relevant team and fire the situation. Throttled per (instance, situation).

## 3. Objective state — accessible getters (read from core + bot AI)

| BG (zone) | State source | Situations |
|---|---|---|
| **SA** (4384) | gate GOs `GetBGObject(idx)->GetDestructibleState()`, relic GO, graveyard flags (as in the SA bot AI) | `ramming`, `gate_down` (`<loc>`=gate), `relic_exposed`, `demolisher_inc`, `gy_capped` (`<loc>`=East/West), `boarding`, `match_start` |
| **WSG** (3277) | `GetFlagState(team)` (1 base/2 player/3 ground), `GetFlagPickerGUID(team)`, `GetTeamScore(team)` (0–3) | `flag_grab`, `flag_carry`, `flag_capped`, `flag_dropped`, `flag_returned`, `enemy_has_flag` |
| **AB** (3358) | `GetCapturePointInfo(node)._state` (neutral/occ/contested per team), `GetTeamScore` (≤1600), 5 named nodes | `node_assault`, `node_captured` (`<loc>`=Stables/Blacksmith/Farm/Lumber Mill/Gold Mine), `node_lost`, `winning`, `losing` |
| **AV** (2597) | `GetAVNodeInfo(node)` (state/owner/`Tower`), `IsTower(node)`, `GetTeamScore` (reinforcements) | `tower_assault`, `tower_captured` (`<loc>`), `gy_captured` (`<loc>`), `reinforcements_low`, `boss_down` |

`<loc>` is a placeholder substituted with the specific gate/graveyard/node name from the event → the "dynamic location."

## 4. Callout flow — `LlmChatMgr::BgCallout(bot, bgZoneId, situation, locName)`

1. Weighted-pick a canned row for `(bgZoneId, situation)`; substitute `<loc>` → `locName`. (No row → no-op.)
2. Roll `LlmBgCannedChance`:
   - **hit** → emit the canned line verbatim on the row's channel (`/say` via `bot->Say`, or `/bg` via `SayToRaid`). Synchronous, on the world/BG thread (no LLM, free).
   - **miss** → if LLM enabled and the BG gates pass, dispatch async, seeded with the canned line: system = persona + BG `zone_flavor`; user = `Rephrase this battleground shout in character, one short line: "<canned>"`. Result posts via a new `LlmChatOperation` Say/Raid target. If gated/disabled → emit the canned line verbatim (graceful fallback).
3. Throttle: BG-specific per-bot cooldown + per-(instance,situation) cooldown (separate config below).

Channel per situation lives in the table (`channel` column: `say` | `bg`). Coordination → `bg`, taunts → `say`.

## 5. Storage (one new table; static tone reuses zone tables)

```sql
playerbot_llm_bg_callout(
  id INT AUTO_INCREMENT PK,
  bg_zone_id INT,            -- 4384 SA, 3277 WSG, 3358 AB, 2597 AV (0 = any BG fallback)
  situation  VARCHAR(32),   -- 'ramming','gate_down',...
  locale     VARCHAR(4) DEFAULT 'enUS',
  line       VARCHAR(255),  -- may contain <loc>; doubles as the LLM seed
  channel    ENUM('say','bg') DEFAULT 'say',
  weight     INT DEFAULT 1
)
```
Loaded by `LlmPersonaMgr` (mirrors its existing loaders). **Per-BG static tone reuses `playerbot_llm_zone_flavor` keyed by the BG zone id — no new table.** Seeded richly for SA + sensible defaults for WSG/AB/AV.

## 6. Reactive replies in BG

In `LlmChatMgr::RequestReply`, when the bot is in a BG, also route `SRC_SAY` (→ Say target) and `SRC_RAID`/BG chat (→ Raid target) to the LLM, so bots answer players in BGs. Subject to the BG cooldowns + shared budget. (Outside BGs, behavior is unchanged: only World/General.)

## 7. Config — separate BG knobs (shared daily budget)

| Key | Default | Notes |
|---|---|---|
| `AiPlayerbot.LlmBgEnabled` | `1` | BG callouts on; works even with `LlmEnabled=0` (canned-only) |
| `AiPlayerbot.LlmBgCannedChance` | `50` | % verbatim vs LLM-reflavored (the seed) |
| `AiPlayerbot.LlmBgBotCooldownSec` | `30` | per-bot say cooldown in BG (vs global 120) |
| `AiPlayerbot.LlmBgEventCooldownSec` | `15` | per (instance, situation) — stops 10 bots shouting the same |
| `AiPlayerbot.LlmBgAnnounceChance` | `25` | per-tick chance for the per-bot action callouts |

The daily token budget / rate limiter remain the **shared global wallet** — BG only gets its own frequency/cooldowns, so "more BG chat" doesn't mean "more spend" (canned-heavy).

## 8. New surface (minimal)
- 1 table + seed SQL; loader + `BgCallout`/`BgZoneFlavor` accessors in `LlmPersonaMgr`.
- `LlmChatMgr::BgCallout` entrypoint + BG cooldown maps + BG config; reactive BG routing.
- `LlmChatOperation` gains `Say` and `Raid` targets.
- `BGTactics` `"bg announce"` action + `ActionContext` creator + trigger wiring in BG strategies.
- `OnBattlegroundUpdate` override in `PlayerBotsBGScript` + a per-instance prev-state cache.

## 9. Guardrails / safety (inherited)
All existing LLM guardrails apply: shared budget/rate, output `|`-strip + blocklist + 255 clamp, mutex/atomic state, worker snapshotting. BG callouts add only frequency knobs. Canned-verbatim emits are plain strings (sanitized). Reactive BG replies inherit the whisper/channel safety.

## 10. Sequence
implement → **code + security review** → apply fixes → **single build** → commit + push + MR stacked on #3. All docs updated + a "how to add BG callouts/flavor" guide.

## 11. Out of scope (YAGNI)
Per-BG separate budgets, EY/IC full dynamic (generic only for now), voice/emote callouts, cross-team reading of enemy comms.
