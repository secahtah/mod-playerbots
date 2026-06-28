/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef PLAYERBOTS_LLMBGEVENTS_H
#define PLAYERBOTS_LLMBGEVENTS_H

#include "Define.h"

class Battleground;

// Detects battleground objective CHANGES (gate destroyed, flag captured, node taken, reinforcements low, ...)
// by diffing a per-instance snapshot, and fires an objective callout from one team bot via LlmChatMgr::BgCallout.
// Called from PlayerBotsBGScript::OnBattlegroundUpdate (world/map thread). State is mutex-guarded.
namespace LlmBgEvents
{
    void Update(Battleground* bg, uint32 diff);
    void OnEnd(uint32 instanceId);  // drop the cached snapshot when a BG ends
}

#endif
