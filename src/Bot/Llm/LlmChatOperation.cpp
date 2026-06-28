/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LlmChatOperation.h"

#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"

bool LlmChatOperation::Execute()
{
    if (_text.empty())
        return false;

    Player* bot = ObjectAccessor::FindPlayer(_botGuid);
    if (!bot || !bot->IsInWorld())
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    switch (_target)
    {
        case Target::Whisper:
            if (_whisperTarget.empty())
                return false;
            return botAI->Whisper(_text, _whisperTarget);
        case Target::World:
            return botAI->SayToWorld(_text);
        case Target::Say:
            return botAI->Say(_text);
        case Target::Raid:
            return botAI->SayToRaid(_text);
        case Target::ZoneChannel:
        default:
            return botAI->SayToChannel(_text, ChatChannelId::GENERAL);
    }
}
