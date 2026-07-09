#pragma once

#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // The directive seam's strategy ("brain"): consumes mailed directives at
    // executor cadence and runs the mirror-mode stub heartbeat. Enabled by
    // default for every bot; both triggers are inert unless the bot serves a
    // real player's party (the ambient 1000 stay untouched).
    class BrainStrategy : public Strategy
    {
    public:
        BrainStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "brain"; }
        int GetType() override { return STRATEGY_TYPE_GENERIC; }

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };
}
