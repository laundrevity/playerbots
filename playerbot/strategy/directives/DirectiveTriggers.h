#pragma once

#include "playerbot/strategy/Trigger.h"

namespace ai
{
    // Fires inside the bot's own engine tick when a brain has mailed a
    // directive — consumption happens at executor cadence, never mid-action.
    class DirectivePendingTrigger : public Trigger
    {
    public:
        DirectivePendingTrigger(PlayerbotAI* ai) : Trigger(ai, "directive pending", 1) {}
        virtual bool IsActive() override;
    };

    // Mirror-mode stub heartbeat: while a real player's party bot is in
    // combat with no active directive, re-emit "keep doing what you're
    // doing" so the whole seam (emit -> mailbox -> validate -> slot -> log)
    // runs constantly with zero behavior change.
    class StubBrainReplanTrigger : public Trigger
    {
    public:
        StubBrainReplanTrigger(PlayerbotAI* ai) : Trigger(ai, "stub brain replan", 3) {}
        virtual bool IsActive() override;
    };
}
