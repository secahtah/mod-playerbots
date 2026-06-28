/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef PLAYERBOTS_LLMPERSONAMGR_H
#define PLAYERBOTS_LLMPERSONAMGR_H

#include "Define.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Player;

/**
 * Loads the LLM persona/attitude/zone tables from the playerbots DB (boot + /reload) and assembles
 * token-minimal prompts. Mirrors PlayerbotTextMgr's DB-load pattern. Thread-safe reads (mutex) because
 * prompt assembly happens on the world thread while Load() may run on reload.
 */
class LlmPersonaMgr
{
public:
    static LlmPersonaMgr& instance()
    {
        static LlmPersonaMgr instance;
        return instance;
    }

    // Load all four tables from PlayerbotsDatabase. Safe to call again on /reload.
    void Load();

    // Compact system prompt: base template + this bot's race/class trait tags + faction.
    std::string BuildSystemPrompt(Player* bot) const;

    // Short attitude tag of a bot (subjectRace) toward a speaker (objectRace): "hostile"/"aloof"/"neutral"/...
    std::string AttitudeTag(uint8 subjectRace, uint8 objectRace) const;

    // Per-zone LLM "vibe" snippet appended to the ambient system prompt; "" if the zone has none.
    std::string ZoneFlavor(uint32 zoneId) const;

    // A weighted-random verbatim canned line for a zone; "" if the zone has no canned pool.
    std::string RandomCannedLine(uint32 zoneId) const;

    bool HasZoneFlavor(uint32 zoneId) const;

    // A weighted-random battleground callout line for (bgZoneId, situation); sets channelOut to "say"/"bg".
    // Falls back to bg_zone_id 0 (any-BG). Returns "" if nothing is seeded. The line may contain "<loc>".
    std::string BgCallout(uint32 bgZoneId, std::string const& situation, std::string& channelOut) const;

private:
    LlmPersonaMgr() = default;

    mutable std::mutex _mutex;
    std::unordered_map<uint8, std::string> _raceTraits;   // race id -> trait text
    std::unordered_map<uint8, std::string> _classTraits;  // class id -> trait text
    std::map<std::pair<uint8, uint8>, std::string> _attitude;  // (subjectRace, objectRace) -> tag
    std::unordered_map<uint32, std::string> _zoneFlavor;  // zoneId -> flavor text
    std::unordered_map<uint32, std::vector<std::pair<std::string, uint32>>> _zoneCanned;  // zoneId -> [(line, weight)]

    struct BgCalloutLine
    {
        std::string line;
        std::string channel;  // "say" | "bg"
        uint32 weight = 1;
    };
    std::map<std::pair<uint32, std::string>, std::vector<BgCalloutLine>> _bgCallouts;  // (bgZoneId, situation)
};

#define sLlmPersonaMgr LlmPersonaMgr::instance()

#endif
