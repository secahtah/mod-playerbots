/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LlmPersonaMgr.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"

namespace
{
    char const* RaceName(uint8 race)
    {
        switch (race)
        {
            case RACE_HUMAN: return "Human";
            case RACE_ORC: return "Orc";
            case RACE_DWARF: return "Dwarf";
            case RACE_NIGHTELF: return "Night Elf";
            case RACE_UNDEAD_PLAYER: return "Forsaken";
            case RACE_TAUREN: return "Tauren";
            case RACE_GNOME: return "Gnome";
            case RACE_TROLL: return "Troll";
            case RACE_BLOODELF: return "Blood Elf";
            case RACE_DRAENEI: return "Draenei";
            default: return "adventurer";
        }
    }

    char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR: return "Warrior";
            case CLASS_PALADIN: return "Paladin";
            case CLASS_HUNTER: return "Hunter";
            case CLASS_ROGUE: return "Rogue";
            case CLASS_PRIEST: return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN: return "Shaman";
            case CLASS_MAGE: return "Mage";
            case CLASS_WARLOCK: return "Warlock";
            case CLASS_DRUID: return "Druid";
            default: return "adventurer";
        }
    }
}  // namespace

void LlmPersonaMgr::Load()
{
    std::lock_guard<std::mutex> guard(_mutex);
    _raceTraits.clear();
    _classTraits.clear();
    _attitude.clear();
    _zoneFlavor.clear();
    _zoneCanned.clear();

    if (QueryResult r =
            PlayerbotsDatabase.Query("SELECT kind, id, trait_text FROM playerbot_llm_persona WHERE locale = 'enUS'"))
    {
        do
        {
            Field* f = r->Fetch();
            std::string kind = f[0].Get<std::string>();
            uint8 id = f[1].Get<uint8>();
            std::string text = f[2].Get<std::string>();
            if (kind == "race")
                _raceTraits[id] = text;
            else if (kind == "class")
                _classTraits[id] = text;
        } while (r->NextRow());
    }

    if (QueryResult r =
            PlayerbotsDatabase.Query("SELECT subject_race, object_race, attitude_tag FROM playerbot_llm_attitude"))
    {
        do
        {
            Field* f = r->Fetch();
            uint8 subj = f[0].Get<uint8>();
            uint8 obj = f[1].Get<uint8>();
            _attitude[{subj, obj}] = f[2].Get<std::string>();
        } while (r->NextRow());
    }

    if (QueryResult r =
            PlayerbotsDatabase.Query("SELECT zone_id, flavor_text FROM playerbot_llm_zone_flavor WHERE locale = 'enUS'"))
    {
        do
        {
            Field* f = r->Fetch();
            _zoneFlavor[f[0].Get<uint32>()] = f[1].Get<std::string>();
        } while (r->NextRow());
    }

    if (QueryResult r = PlayerbotsDatabase.Query(
            "SELECT zone_id, line, weight FROM playerbot_llm_zone_canned WHERE locale = 'enUS'"))
    {
        do
        {
            Field* f = r->Fetch();
            uint32 zone = f[0].Get<uint32>();
            std::string line = f[1].Get<std::string>();
            uint32 weight = f[2].Get<uint32>();
            if (weight == 0)
                weight = 1;
            _zoneCanned[zone].emplace_back(std::move(line), weight);
        } while (r->NextRow());
    }

    LOG_INFO("playerbots", "LLM persona data loaded: {} race, {} class, {} attitude, {} zone-flavor, {} canned-zones",
             _raceTraits.size(), _classTraits.size(), _attitude.size(), _zoneFlavor.size(), _zoneCanned.size());
}

std::string LlmPersonaMgr::BuildSystemPrompt(Player* bot) const
{
    if (!bot)
        return sPlayerbotAIConfig.llmSystemPrompt;

    uint8 race = bot->getRace();
    uint8 cls = bot->getClass();

    std::string raceTrait;
    std::string classTrait;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (auto it = _raceTraits.find(race); it != _raceTraits.end())
            raceTrait = it->second;
        if (auto it = _classTraits.find(cls); it != _classTraits.end())
            classTrait = it->second;
    }

    std::string prompt = sPlayerbotAIConfig.llmSystemPrompt;
    prompt += " You are a ";
    prompt += RaceName(race);
    prompt += " ";
    prompt += ClassName(cls);
    prompt += ".";
    if (!raceTrait.empty() || !classTrait.empty())
    {
        prompt += " Traits: ";
        prompt += raceTrait;
        if (!raceTrait.empty() && !classTrait.empty())
            prompt += "; ";
        prompt += classTrait;
        prompt += ".";
    }
    prompt += " Reply in-character in one short sentence.";
    return prompt;
}

std::string LlmPersonaMgr::AttitudeTag(uint8 subjectRace, uint8 objectRace) const
{
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (auto it = _attitude.find({subjectRace, objectRace}); it != _attitude.end())
            return it->second;
    }

    // Code fallbacks (keep the table tiny).
    if (subjectRace == objectRace)
        return "kinship";
    if (objectRace == RACE_UNDEAD_PLAYER)
        return "revolted";
    if (IsAlliance(subjectRace) != IsAlliance(objectRace))
        return "hostile";
    return "neutral";
}

std::string LlmPersonaMgr::ZoneFlavor(uint32 zoneId) const
{
    std::lock_guard<std::mutex> guard(_mutex);
    if (auto it = _zoneFlavor.find(zoneId); it != _zoneFlavor.end())
        return it->second;
    return "";
}

bool LlmPersonaMgr::HasZoneFlavor(uint32 zoneId) const
{
    std::lock_guard<std::mutex> guard(_mutex);
    return _zoneFlavor.count(zoneId) != 0 || _zoneCanned.count(zoneId) != 0;
}

std::string LlmPersonaMgr::RandomCannedLine(uint32 zoneId) const
{
    std::lock_guard<std::mutex> guard(_mutex);
    auto it = _zoneCanned.find(zoneId);
    if (it == _zoneCanned.end() || it->second.empty())
        return "";

    uint32 total = 0;
    for (auto const& p : it->second)
        total += p.second;
    if (total == 0)
        return "";

    uint32 roll = urand(0, total - 1);
    for (auto const& p : it->second)
    {
        if (roll < p.second)
            return p.first;
        roll -= p.second;
    }
    return it->second.front().first;
}
