-- LLM-driven bot chat: persona traits, attitude overrides, and per-zone flavor + canned meme pool.
-- See docs/superpowers/specs/2026-06-28-llm-bot-chat-design.md
-- Race ids: Human 1, Orc 2, Dwarf 3, NightElf 4, Undead 5, Tauren 6, Gnome 7, Troll 8, BloodElf 10, Draenei 11.
-- Class ids: Warrior 1, Paladin 2, Hunter 3, Rogue 4, Priest 5, DeathKnight 6, Shaman 7, Mage 8, Warlock 9, Druid 11.

-- ---------------------------------------------------------------------------------------------------
-- Race/class trait tags (token-minimal). kind = 'race' or 'class'.
CREATE TABLE IF NOT EXISTS `playerbot_llm_persona` (
  `kind` ENUM('race','class') NOT NULL,
  `id` TINYINT UNSIGNED NOT NULL,
  `locale` VARCHAR(4) NOT NULL DEFAULT 'enUS',
  `trait_text` VARCHAR(255) NOT NULL,
  PRIMARY KEY (`kind`,`id`,`locale`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `playerbot_llm_persona` WHERE `locale`='enUS';
INSERT INTO `playerbot_llm_persona` (`kind`,`id`,`locale`,`trait_text`) VALUES
('race',1,'enUS','ambitious, diplomatic, a touch smug'),
('race',2,'enUS','gruff, honorable, blunt'),
('race',3,'enUS','jolly, stubborn, loves ale'),
('race',4,'enUS','ancient, aloof, nature-bound'),
('race',5,'enUS','morbid, sardonic, spiteful'),
('race',6,'enUS','calm, wise, peace-loving'),
('race',7,'enUS','hyper, inventive, chatty'),
('race',8,'enUS','laid-back, mystic, says "mon"'),
('race',10,'enUS','vain, haughty, magic-craving'),
('race',11,'enUS','serene, devout, formal'),
('class',1,'enUS','brash and fearless'),
('class',2,'enUS','righteous and a bit preachy'),
('class',3,'enUS','a loner who dotes on their pet'),
('class',4,'enUS','sly and sarcastic'),
('class',5,'enUS','devout, with a shadowy streak'),
('class',6,'enUS','grim and brooding'),
('class',7,'enUS','spiritual and elemental'),
('class',8,'enUS','arrogant and bookish'),
('class',9,'enUS','sinister and smug'),
('class',11,'enUS','easygoing and nature-loving');

-- ---------------------------------------------------------------------------------------------------
-- Attitude OVERRIDES only. The loader computes sensible fallbacks in code:
--   same race -> "kinship"; object is Undead(5) -> "revolted"; cross-faction -> "hostile"; else "neutral".
-- Seed only the notable race-specific nuances here.
CREATE TABLE IF NOT EXISTS `playerbot_llm_attitude` (
  `subject_race` TINYINT UNSIGNED NOT NULL,
  `object_race` TINYINT UNSIGNED NOT NULL,
  `attitude_tag` VARCHAR(64) NOT NULL,
  PRIMARY KEY (`subject_race`,`object_race`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `playerbot_llm_attitude`;
INSERT INTO `playerbot_llm_attitude` (`subject_race`,`object_race`,`attitude_tag`) VALUES
(4,1,'aloof and unimpressed'),     -- night elf -> human
(1,4,'wary but curious'),          -- human -> night elf
(10,4,'old disdain'),              -- blood elf -> night elf
(4,10,'old disdain'),              -- night elf -> blood elf
(3,8,'friendly rivalry'),          -- dwarf -> troll (cross-faction but jokey)
(6,5,'pitying'),                   -- tauren -> undead (allies, but uneasy)
(2,5,'uneasy ally');               -- orc -> undead

-- ---------------------------------------------------------------------------------------------------
-- Per-zone LLM "vibe" snippet appended to the ambient system prompt.
CREATE TABLE IF NOT EXISTS `playerbot_llm_zone_flavor` (
  `zone_id` INT UNSIGNED NOT NULL,
  `locale` VARCHAR(4) NOT NULL DEFAULT 'enUS',
  `flavor_text` VARCHAR(512) NOT NULL,
  PRIMARY KEY (`zone_id`,`locale`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `playerbot_llm_zone_flavor` WHERE `locale`='enUS';
INSERT INTO `playerbot_llm_zone_flavor` (`zone_id`,`locale`,`flavor_text`) VALUES
(17,'enUS','This is Barrens General chat, circa 2005. Channel that era: Chuck-Norris-style jokes, asking where Mankrik''s wife is, bad cow/Tauren puns, and LFG for Wailing Caverns. Keep it to one short, silly line.');

-- ---------------------------------------------------------------------------------------------------
-- Per-zone pool of VERBATIM canned lines (zero tokens). Weighted random pick.
CREATE TABLE IF NOT EXISTS `playerbot_llm_zone_canned` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `zone_id` INT UNSIGNED NOT NULL,
  `locale` VARCHAR(4) NOT NULL DEFAULT 'enUS',
  `line` VARCHAR(255) NOT NULL,
  `weight` INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`id`),
  KEY `idx_zone` (`zone_id`,`locale`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `playerbot_llm_zone_canned` WHERE `zone_id`=17 AND `locale`='enUS';
INSERT INTO `playerbot_llm_zone_canned` (`zone_id`,`locale`,`line`,`weight`) VALUES
(17,'enUS','Where the heck is Mankrik''s wife???',3),
(17,'enUS','anyone know where mankrik''s wife is??',2),
(17,'enUS','Chuck Norris knows where Mankrik''s wife is, because he let her go.',2),
(17,'enUS','Chuck Norris doesn''t search for Mankrik''s wife. Mankrik''s wife searches for him.',2),
(17,'enUS','Did you know that Chuck Norris can hear sign language?',2),
(17,'enUS','Chuck Norris doesn''t sleep. He waits.',2),
(17,'enUS','Chuck Norris can kill two stones with one bird.',2),
(17,'enUS','Need a tank for Wailing Caverns, I am at the Crossroads!',3),
(17,'enUS','LFG WC, need a healer and tank, summons at the zep towers',2),
(17,'enUS','Is there any better class than Rogue?',1),
(17,'enUS','Chuck Norris is the best class. He has no class, he just roundhouse kicks you.',1),
(17,'enUS','Why did the Tauren cross the road? To get to the udder side.',2),
(17,'enUS','That''s an udderly terrible joke.',2),
(17,'enUS','Did someone say [Thunderfury, Blessed Blade of the Windseeker]?',2);
