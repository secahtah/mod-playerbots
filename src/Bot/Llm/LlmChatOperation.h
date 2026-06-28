/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef PLAYERBOTS_LLMCHATOPERATION_H
#define PLAYERBOTS_LLMCHATOPERATION_H

#include "ObjectGuid.h"
#include "PlayerbotOperation.h"

#include <string>

/**
 * Applies an LLM-generated chat line on the WORLD THREAD. Constructed on a worker thread once the LLM
 * responds, then handed to PlayerbotWorldThreadProcessor::QueueOperation. Stores only copies (guid + strings),
 * never raw game pointers, per the PlayerbotOperation contract.
 */
class LlmChatOperation : public PlayerbotOperation
{
public:
    enum class Target : uint8
    {
        ZoneChannel,  // bot's current-zone General channel (SayToChannel GENERAL)
        World,        // custom World channel (SayToWorld)
        Whisper       // whisper back to whisperTarget
    };

    LlmChatOperation(ObjectGuid botGuid, Target target, std::string text, std::string whisperTarget = "")
        : _botGuid(botGuid), _target(target), _text(std::move(text)), _whisperTarget(std::move(whisperTarget))
    {
    }

    bool Execute() override;  // resolve bot by guid; emit via SayToChannel / SayToWorld / Whisper
    ObjectGuid GetBotGuid() const override { return _botGuid; }
    uint32 GetPriority() const override { return 50; }  // player-facing
    std::string GetName() const override { return "LlmChat"; }

private:
    ObjectGuid _botGuid;
    Target _target;
    std::string _text;
    std::string _whisperTarget;
};

#endif
