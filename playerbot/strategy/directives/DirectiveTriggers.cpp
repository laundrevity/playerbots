#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/DirectiveTriggers.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/directives/DirectiveMgr.h"
#include "playerbot/strategy/directives/DirectiveValues.h"
#include "Util/Timer.h"

using namespace ai;

bool DirectivePendingTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.directiveEnabled)
        return false;
    return sDirectiveMgr.HasPending(bot->GetObjectGuid());
}

bool StubBrainReplanTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.directiveEnabled || sPlayerbotAIConfig.directiveStubMode != 1)
        return false;

    if (!ai->HasRealPlayerMaster())
        return false;

    if (!bot->IsInCombat())
        return false;

    Directive directive = AI_VALUE(Directive, "directive");
    return !directive.IsActiveNow(WorldTimer::getMSTime());
}
