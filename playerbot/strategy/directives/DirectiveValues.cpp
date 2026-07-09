#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/DirectiveValues.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "Util/Timer.h"

#include <algorithm>
#include <sstream>

using namespace ai;

std::string DirectiveValue::Format()
{
    Directive directive = Get();
    if (!directive.valid)
        return "<none>";

    std::ostringstream out;
    out << directive.id << " (" << DirectiveSourceTag(directive.src) << ")"
        << " targets:" << directive.killOrder.size();
    uint32 now = WorldTimer::getMSTime();
    if (directive.IsActiveNow(now))
        out << " ttl:" << (directive.expiresAtMs - now) << "ms";
    else
        out << " EXPIRED";
    return out.str();
}

Unit* ai::GetDirectiveKillTarget(PlayerbotAI* ai, AiObjectContext* context)
{
    if (!sPlayerbotAIConfig.directiveEnabled)
        return nullptr;

    Directive directive = context->GetValue<Directive>("directive")->Get();
    if (!directive.IsActiveNow(WorldTimer::getMSTime()) || directive.killOrder.empty())
        return nullptr;

    Player* bot = ai->GetBot();
    std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
    for (const ObjectGuid& guid : directive.killOrder)
    {
        if (std::find(possible.begin(), possible.end(), guid) == possible.end())
            continue;

        Unit* unit = ai->GetUnit(guid);
        if (!unit || sServerFacade.UnitIsDead(unit))
            continue;

        if (!bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.sightDistance, false))
            continue;

        return unit;
    }
    return nullptr;
}
