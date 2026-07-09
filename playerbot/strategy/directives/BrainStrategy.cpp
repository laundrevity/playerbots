#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/BrainStrategy.h"

using namespace ai;

void BrainStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "directive pending",
        NextAction::array(0, new NextAction("apply directive", 89.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "stub brain replan",
        NextAction::array(0, new NextAction("stub brain emit", 88.0f), NULL)));
}

void BrainStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "directive pending",
        NextAction::array(0, new NextAction("apply directive", 89.0f), NULL)));
}
