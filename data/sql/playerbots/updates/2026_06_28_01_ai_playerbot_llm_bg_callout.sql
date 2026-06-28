-- Objective-aware in-battleground bot callouts. Each row is a canned line for a (bg_zone_id, situation):
-- it is posted verbatim part of the time (AiPlayerbot.LlmBgCannedChance) and used as the LLM seed the rest,
-- so the LLM reflavors the same topical line in the bot's persona. <loc> is substituted at runtime with the
-- specific gate / node / graveyard name. channel: 'bg' = team raid chat (coordination), 'say' = local /say
-- (taunts, public). bg_zone_id: 4384 SotA, 3277 Warsong Gulch, 3358 Arathi Basin, 2597 Alterac Valley.
-- See docs/superpowers/specs/2026-06-28-bg-bot-chat-design.md and docs/llm-bot-chat.md.

CREATE TABLE IF NOT EXISTS `playerbot_llm_bg_callout` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bg_zone_id` INT UNSIGNED NOT NULL,
  `situation` VARCHAR(32) NOT NULL,
  `locale` VARCHAR(4) NOT NULL DEFAULT 'enUS',
  `line` VARCHAR(255) NOT NULL,
  `channel` ENUM('say','bg') NOT NULL DEFAULT 'say',
  `weight` INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`id`),
  KEY `idx_lookup` (`bg_zone_id`,`situation`,`locale`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `playerbot_llm_bg_callout` WHERE `locale`='enUS';
INSERT INTO `playerbot_llm_bg_callout` (`bg_zone_id`,`situation`,`locale`,`line`,`channel`,`weight`) VALUES
-- ===================== Strand of the Ancients (4384) =====================
(4384,'match_start','enUS','Disembark! Take the beach!','bg',2),
(4384,'boarding','enUS','Got a demolisher, rolling out!','bg',2),
(4384,'ramming','enUS','Ramming the gate!','say',3),
(4384,'ramming','enUS','Bring this wall down!','say',2),
(4384,'gate_down','enUS','<loc> is down! Push!','bg',3),
(4384,'gate_down','enUS','<loc> breached, move up!','bg',2),
(4384,'demolisher_inc','enUS','Demolisher incoming on <loc>!','bg',3),
(4384,'gy_capped','enUS','We took the <loc> graveyard!','bg',2),
(4384,'relic_exposed','enUS','Relic is open — rush it!','bg',3),
(4384,'taunt','enUS','Your gates won''t hold!','say',1),
-- ===================== Warsong Gulch (3277) =====================
(3277,'flag_grab','enUS','Got their flag, heading home!','bg',3),
(3277,'flag_carry','enUS','FC needs an escort!','bg',3),
(3277,'flag_carry','enUS','FC low, peel for me!','bg',2),
(3277,'flag_capped','enUS','Flag capped! That''s one!','bg',3),
(3277,'flag_dropped','enUS','Our flag dropped — get it back!','bg',2),
(3277,'flag_returned','enUS','Flag returned!','bg',2),
(3277,'enemy_has_flag','enUS','They have our flag! EFC at <loc>!','bg',3),
(3277,'enemy_has_flag','enUS','EFC dropped, return it!','bg',2),
(3277,'incoming','enUS','INC mid!','bg',2),
(3277,'incoming','enUS','INC tunnel!','bg',2),
(3277,'incoming','enUS','INC roof!','bg',1),
(3277,'taunt','enUS','Your FC is a turtle, come fight!','say',2),
(3277,'taunt','enUS','Stop zerging mid and grab the flag!','say',2),
(3277,'taunt','enUS','Nice peel... not.','say',1),
-- ===================== Arathi Basin (3358) =====================
(3358,'node_assault','enUS','Inc <loc>!','bg',3),
(3358,'node_assault','enUS','Inc <loc>, many!','bg',2),
(3358,'node_captured','enUS','Capped <loc>!','bg',3),
(3358,'node_lost','enUS','Lost <loc> — take it back!','bg',2),
(3358,'defend','enUS','Def <loc>!','bg',3),
(3358,'oom','enUS','OOM, need to drink!','bg',1),
(3358,'winning','enUS','We''re ahead — hold the bases!','bg',2),
(3358,'losing','enUS','We''re behind, stop zerging and cap!','bg',2),
(3358,'taunt','enUS','Lumber Mill is a deathtrap, lol.','say',2),
(3358,'taunt','enUS','Lost a base to ONE of them, really?','say',1),
-- ===================== Alterac Valley (2597) =====================
(2597,'tower_captured','enUS','Took <loc>!','bg',2),
(2597,'gy_captured','enUS','Capped <loc> graveyard!','bg',2),
(2597,'defend','enUS','Def the <loc>!','bg',3),
(2597,'incoming','enUS','Inc the bridge!','bg',2),
(2597,'incoming','enUS','Inc <loc>!','bg',2),
(2597,'kill_lts','enUS','Kill the Lieutenants!','bg',2),
(2597,'pull_boss','enUS','Pull Galv!','bg',2),
(2597,'pull_boss','enUS','On Drek, let''s go!','bg',2),
(2597,'reinforcements_low','enUS','Reinforcements low — defend!','bg',3),
(2597,'taunt','enUS','PvE''rs, turn around and fight!','say',2),
(2597,'taunt','enUS','Tunnel vision much?','say',1),
(2597,'taunt','enUS','Stop AFKing in the cave!','say',2);

-- Per-battleground LLM tone, reusing the existing zone-flavor table keyed by the BG zone id. Used to
-- reflavor a callout when AiPlayerbot.LlmBgCannedChance misses (the canned line is the seed, this is the voice).
DELETE FROM `playerbot_llm_zone_flavor` WHERE `zone_id` IN (4384,3277,3358,2597) AND `locale`='enUS';
INSERT INTO `playerbot_llm_zone_flavor` (`zone_id`,`locale`,`flavor_text`) VALUES
(4384,'enUS','You are a soldier mid-siege in the Strand of the Ancients battleground. Terse, urgent, aggressive.'),
(3277,'enUS','You are a tryhard flag-runner in Warsong Gulch. Fast, cocky, lots of callouts and light trash talk.'),
(3358,'enUS','You are a Arathi Basin node-rotator. Bark map calls and get salty when a base is lost.'),
(2597,'enUS','You are a grizzled veteran of the endless Alterac Valley war. Gruff, faction-proud, impatient with leechers.');

