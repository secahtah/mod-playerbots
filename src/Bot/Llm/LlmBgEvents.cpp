/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LlmBgEvents.h"

#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundAV.h"
#include "BattlegroundSA.h"
#include "BattlegroundWS.h"
#include "LlmChatMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct Snap
    {
        bool init = false;
        uint32 accum = 0;
        bool saGate[6] = {false, false, false, false, false, false};  // destroyed?
        bool saRelic = false;
        uint8 wsFlag[2] = {1, 1};  // BG_WS_FLAG_STATE_ON_BASE
        uint32 wsScore[2] = {0, 0};
        TeamId abOwner[5] = {TEAM_NEUTRAL, TEAM_NEUTRAL, TEAM_NEUTRAL, TEAM_NEUTRAL, TEAM_NEUTRAL};
        uint8 avState[16] = {0};
        bool reinfLow[2] = {false, false};
    };

    std::mutex g_mutex;
    std::unordered_map<uint32, Snap> g_snaps;

    char const* SA_GATE[6] = {"Green Gate", "Yellow Gate", "Blue Gate", "Red Gate", "Purple Gate", "Ancient Gate"};
    char const* AB_NODE[5] = {"Stables", "Blacksmith", "Farm", "Lumber Mill", "Gold Mine"};

    struct BgEvent
    {
        TeamId team;
        std::string situation;
        std::string loc;
    };

    Player* PickBot(Battleground* bg, TeamId team)
    {
        std::vector<Player*> bots;
        for (auto const& pair : bg->GetPlayers())
        {
            Player* p = ObjectAccessor::FindPlayer(pair.first);
            if (p && p->IsInWorld() && p->IsAlive() && p->GetTeamId() == team && GET_PLAYERBOT_AI(p))
                bots.push_back(p);
        }
        if (bots.empty())
            return nullptr;
        return bots[urand(0, bots.size() - 1)];
    }

    bool GateDown(Battleground* bg, uint32 idx)
    {
        GameObject* g = bg->GetBGObject(idx);
        return !g || g->GetDestructibleState() == GO_DESTRUCTIBLE_DESTROYED;
    }
}  // namespace

void LlmBgEvents::OnEnd(uint32 instanceId)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_snaps.erase(instanceId);
}

void LlmBgEvents::Update(Battleground* bg, uint32 diff)
{
    if (!bg || !sPlayerbotAIConfig.llmBgEnabled || bg->GetStatus() != STATUS_IN_PROGRESS)
        return;

    BattlegroundTypeId bgType = bg->GetBgTypeID(true);
    if (bgType != BATTLEGROUND_SA && bgType != BATTLEGROUND_WS && bgType != BATTLEGROUND_AB &&
        bgType != BATTLEGROUND_AV)
        return;

    std::vector<BgEvent> events;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        Snap& s = g_snaps[bg->GetInstanceID()];
        s.accum += diff;
        if (s.accum < 1000)
            return;  // diff at ~1s cadence
        s.accum = 0;
        bool first = !s.init;
        s.init = true;

        switch (bgType)
        {
            case BATTLEGROUND_SA:
            {
                GameObject* relicGo = bg->GetBGObject(BG_SA_TITAN_RELIC);
                if (!relicGo)
                    break;  // can't attribute the attacking side yet; detect next tick
                TeamId atk = relicGo->GetUInt32Value(GAMEOBJECT_FACTION) == BG_SA_Factions[TEAM_ALLIANCE]
                                 ? TEAM_ALLIANCE
                                 : TEAM_HORDE;
                for (uint32 i = 0; i < 6; ++i)
                {
                    bool down = GateDown(bg, i);
                    if (down && !s.saGate[i] && !first)
                        events.push_back({atk, "gate_down", SA_GATE[i]});
                    s.saGate[i] = down;
                }
                bool relic = GateDown(bg, BG_SA_YELLOW_GATE) && GateDown(bg, BG_SA_ANCIENT_GATE);
                if (relic && !s.saRelic && !first)
                    events.push_back({atk, "relic_exposed", ""});
                s.saRelic = relic;
                break;
            }
            case BATTLEGROUND_WS:
            {
                BattlegroundWS* ws = static_cast<BattlegroundWS*>(bg);
                // Score first: a capture also resets the captured flag to base, which would otherwise look
                // like a defensive "flag_returned". Detect the cap, then suppress the coincident return.
                bool cappedThisTick = false;
                for (uint8 t = 0; t < 2; ++t)
                {
                    uint32 score = bg->GetTeamScore(TeamId(t));
                    if (!first && score > s.wsScore[t])
                    {
                        events.push_back({TeamId(t), "flag_capped", ""});
                        cappedThisTick = true;
                    }
                    s.wsScore[t] = score;
                }
                for (uint8 t = 0; t < 2; ++t)
                {
                    TeamId team = TeamId(t);
                    uint8 fs = ws->GetFlagState(team);
                    if (!first && fs != s.wsFlag[t])
                    {
                        // team t's flag changed state. ON_PLAYER(2)=taken by enemy, ON_BASE(1)=returned.
                        if (fs == 2)
                            events.push_back({team, "enemy_has_flag", ""});
                        else if (fs == 1 && !cappedThisTick)
                            events.push_back({team, "flag_returned", ""});
                        else if (fs == 3)
                            events.push_back({team, "flag_dropped", ""});
                    }
                    s.wsFlag[t] = fs;
                }
                break;
            }
            case BATTLEGROUND_AB:
            {
                BattlegroundAB* ab = static_cast<BattlegroundAB*>(bg);
                for (uint32 n = 0; n < 5; ++n)
                {
                    CaptureABPointInfo const& info = ab->GetCapturePointInfo(n);
                    uint8 st = info._state;
                    TeamId owner = info._ownerTeamId;
                    bool occupied = (st == BG_AB_NODE_STATE_ALLY_OCCUPIED || st == BG_AB_NODE_STATE_HORDE_OCCUPIED);
                    if (!first && occupied && owner != TEAM_NEUTRAL && owner != s.abOwner[n])
                    {
                        events.push_back({owner, "node_captured", AB_NODE[n]});
                        if (s.abOwner[n] != TEAM_NEUTRAL)
                            events.push_back({s.abOwner[n], "node_lost", AB_NODE[n]});
                    }
                    if (occupied)
                        s.abOwner[n] = owner;
                    else if (st == BG_AB_NODE_STATE_NEUTRAL)
                        s.abOwner[n] = TEAM_NEUTRAL;
                }
                break;
            }
            case BATTLEGROUND_AV:
            {
                BattlegroundAV* av = static_cast<BattlegroundAV*>(bg);
                for (uint32 n = 0; n < BG_AV_NODES_MAX && n < 16; ++n)
                {
                    BG_AV_NodeInfo const& info = av->GetAVNodeInfo(n);
                    uint8 st = (uint8)info.State;
                    if (!first && st == POINT_CONTROLLED && s.avState[n] != POINT_CONTROLLED &&
                        info.OwnerId != TEAM_NEUTRAL)
                    {
                        events.push_back({info.OwnerId, info.Tower ? "tower_captured" : "gy_captured",
                                          info.Tower ? "a tower" : "a graveyard"});
                    }
                    s.avState[n] = st;
                }
                for (uint8 t = 0; t < 2; ++t)
                {
                    TeamId team = TeamId(t);
                    bool low = bg->GetTeamScore(team) < 100;
                    if (!first && low && !s.reinfLow[t])
                        events.push_back({team, "reinforcements_low", ""});
                    s.reinfLow[t] = low;
                }
                break;
            }
            default:
                break;
        }
    }

    // Fire outside the snapshot lock (BgCallout takes its own locks + throttles per instance/situation).
    for (BgEvent const& e : events)
    {
        if (Player* bot = PickBot(bg, e.team))
            sLlmChatMgr.BgCallout(bot, bot->GetZoneId(), e.situation, e.loc);
    }
}
