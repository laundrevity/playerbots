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
    // "possible attack targets" (attackable, not evading), not the looser
    // "possible targets": a pinned evading mob rubber-banded the tank live
    std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
    for (const ObjectGuid& guid : directive.killOrder)
    {
        if (guid == bot->GetObjectGuid())
            continue;   // live bug: a self-target snapshot became "kill yourself"
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

Unit* ai::FindBlessingOverrideMismatch(PlayerbotAI* ai, AiObjectContext* context, Player* bot)
{
    if (bot->getClass() != CLASS_PALADIN || !bot->GetGroup())
        return nullptr;

    std::string overrides = context->GetValue<std::string>("blessing overrides")->Get();
    if (overrides.empty())
        return nullptr;

    for (GroupReference* itr = bot->GetGroup()->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsInWorld() || !member->IsAlive())
            continue;
        if (member->GetMapId() != bot->GetMapId() || !bot->IsWithinDistInMap(member, 30.0f, false))
            continue;

        std::string forced = GetBlessingOverride(overrides, member->GetName());
        if (forced.empty())
            continue;

        std::string want = "blessing of " + forced;
        if (!ai->HasMyAura(want, member) && !ai->HasMyAura("greater " + want, member))
            return member;
    }
    return nullptr;
}
