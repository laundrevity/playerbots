#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/DirectiveActions.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/directives/DirectiveMgr.h"
#include "playerbot/strategy/directives/DirectiveValues.h"
#include "playerbot/thirdparty/nlohmann/json.hpp"

#include "Combat/CombatEventLog.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

using namespace ai;

namespace
{
    bool SameNameNoCase(const char* a, const char* b)
    {
        while (*a && *b)
        {
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
            ++a;
            ++b;
        }
        return !*a && !*b;
    }
}

bool BrainCommandAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string text = event.getParam();
    if (text.empty())
    {
        ai->TellPlayer(requester, "usage: brain {json directive} — see docs/design/directive-seam.md");
        return false;
    }

    sDirectiveMgr.Push(bot->GetObjectGuid(), text, DirectiveSource::Script,
                       requester ? requester->GetObjectGuid() : ObjectGuid());
    return true;
}

void ApplyDirectiveAction::Report(const Directive& directive, bool accepted, const std::string& note)
{
    sCombatEventLog.LogDirective(bot, directive.id.c_str(), DirectiveSourceTag(directive.src),
                                 accepted, note.c_str());

    // humans get whispered feedback; the stub stays silent (log only)
    if (directive.src == DirectiveSource::Script)
    {
        Player* master = GetMaster();
        if (master)
        {
            std::ostringstream out;
            out << "brain: " << (directive.id.empty() ? "directive" : directive.id)
                << (accepted ? " accepted" : " REJECTED");
            if (!note.empty())
                out << " (" << note << ")";
            ai->TellPlayer(master, out.str());
        }
    }
}

bool ApplyDirectiveAction::Execute(Event& event)
{
    PendingDirective pending;
    if (!sDirectiveMgr.Pop(bot->GetObjectGuid(), pending))
        return false;

    Directive incoming;
    std::string error;
    if (!ParseDirective(pending.json, incoming, error))
    {
        incoming.src = pending.source;
        Report(incoming, false, error);
        return false;
    }

    // wire "src" is advisory; the transport knows where it really came from
    incoming.src = pending.source;

    // TTL is clamped, never trusted
    incoming.ttlMs = std::max(500u, std::min(incoming.ttlMs, sPlayerbotAIConfig.directiveMaxTtlMs));

    // validate kill order against live game state (same legality shape as
    // rti/skull targeting: possible target, alive, within sight)
    std::list<ObjectGuid> possible = AI_VALUE(std::list<ObjectGuid>, "possible targets");
    uint32 dropped = 0;
    for (const DirectiveTargetRef& ref : incoming.requestedKillOrder)
    {
        ObjectGuid guid = ref.guid;
        if (!guid && !ref.name.empty())
        {
            // name fallback: first possible target with a case-insensitive match
            for (const ObjectGuid& candidate : possible)
            {
                Unit* unit = ai->GetUnit(candidate);
                if (unit && SameNameNoCase(unit->GetName(), ref.name.c_str()))
                {
                    guid = candidate;
                    break;
                }
            }
        }

        Unit* unit = guid ? ai->GetUnit(guid) : nullptr;
        if (!unit || sServerFacade.UnitIsDead(unit) ||
            std::find(possible.begin(), possible.end(), guid) == possible.end() ||
            !bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.sightDistance, false))
        {
            ++dropped;
            continue;
        }
        incoming.killOrder.push_back(guid);
    }

    if (!incoming.requestedKillOrder.empty() && incoming.killOrder.empty())
    {
        Report(incoming, false, "no valid kill-order targets");
        return false;
    }

    // anchors must be on this map; a bad anchor degrades, it doesn't reject
    std::string note;
    if (incoming.anchor.hasCoords && incoming.anchor.mapId != bot->GetMapId())
    {
        incoming.anchor = DirectiveAnchor();
        note = "anchor dropped (wrong map)";
    }
    if (dropped)
    {
        std::ostringstream d;
        d << (note.empty() ? "" : "; ") << dropped << " target(s) dropped";
        note += d.str();
    }

    incoming.expiresAtMs = WorldTimer::getMSTime() + incoming.ttlMs;
    incoming.valid = true;
    SET_AI_VALUE(Directive, "directive", incoming);

    // the shot-caller's party reply rides on a directive; speak it here on
    // the bot's own thread (party channel when grouped)
    if (!incoming.chatSay.empty() && bot->GetGroup())
        ai->TellPlayerNoFacing(GetMaster(), incoming.chatSay,
                               PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL,
                               /*isPrivate=*/false);

    Report(incoming, true, note);
    return true;
}

bool StubBrainEmitAction::Execute(Event& event)
{
    // mirror mode: re-issue the bot's own current target as the kill order —
    // the architecture runs end to end, behavior stays baseline
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || sServerFacade.UnitIsDead(target))
        return false;

    char guidHex[32];
    snprintf(guidHex, sizeof(guidHex), "0x%llx",
             (unsigned long long)target->GetObjectGuid().GetRawValue());

    nlohmann::json j;
    j["v"] = 0;
    j["id"] = "stub-" + std::to_string(bot->GetGUIDLow()) + "-" + std::to_string(WorldTimer::getMSTime());
    j["src"] = "stub";
    j["ttl_ms"] = 4000;
    j["kill_order"] = nlohmann::json::array({ nlohmann::json{{"guid", guidHex}} });

    sDirectiveMgr.Push(bot->GetObjectGuid(), j.dump(), DirectiveSource::Stub, ObjectGuid());
    return true;
}
