#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/DirectiveActions.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotFactory.h"
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

    // validate kill order against live game state — the attackable set
    // (not-evading), same reason as DirectiveValues::GetDirectiveKillTarget
    std::list<ObjectGuid> possible = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
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
        if (!unit || unit == bot ||
            (unit->IsPlayer() && bot->GetGroup() && ((Player*)unit)->GetGroup() == bot->GetGroup()) ||
            sServerFacade.UnitIsDead(unit) ||
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

    // cc assignments: same resolution + attackable-set rules as kill order
    for (Directive::CcAssignment assignment : incoming.requestedCc)
    {
        if (!assignment.guid && !assignment.name.empty())
            for (const ObjectGuid& candidate : possible)
            {
                Unit* unit = ai->GetUnit(candidate);
                if (unit && SameNameNoCase(unit->GetName(), assignment.name.c_str()))
                {
                    assignment.guid = candidate;
                    break;
                }
            }
        Unit* unit = assignment.guid ? ai->GetUnit(assignment.guid) : nullptr;
        if (!unit || unit == bot || sServerFacade.UnitIsDead(unit) ||
            std::find(possible.begin(), possible.end(), assignment.guid) == possible.end())
        {
            ++dropped;
            continue;
        }
        incoming.cc.push_back(assignment);
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

    // "give_healthstone_to": the warlock opens trade with the named member;
    // the executor's trade hook stuffs the stone and accepts from there
    if (!incoming.giveHealthstoneTo.empty() && bot->getClass() == CLASS_WARLOCK && bot->GetGroup())
        for (GroupReference* itr = bot->GetGroup()->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || member == bot || !member->IsInWorld())
                continue;
            if (!SameNameNoCase(member->GetName(), incoming.giveHealthstoneTo.c_str()))
                continue;
            WorldPacket data(CMSG_INITIATE_TRADE, 8);
            data << member->GetObjectGuid();
            bot->GetSession()->HandleInitiateTradeOpcode(data);
            break;
        }

    Report(incoming, true, note);
    return true;
}

bool MaintainAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    int spec = AiFactory::GetPlayerSpecTab(bot);   // keep the tree he's in
    PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_EPIC);

    bot->resetTalents(true);
    factory.InitTalents(uint32(spec));
    if (bot->GetFreeTalentPoints())
        factory.InitTalents(uint32(2 - spec));     // dump leftovers off-tree

    factory.EnchantEquipment();                    // per-spec enchant template
    factory.InitGems();                            // fill empty sockets

    // rogues run dry: keep a stack of each poison in the bags
    // (Instant VII 21927 / Deadly VII 22054 — the executor applies them)
    if (bot->getClass() == CLASS_ROGUE)
    {
        if (!bot->HasItemCount(21927, 5))
            bot->StoreNewItemInBestSlots(21927, 20);
        if (!bot->HasItemCount(22054, 5))
            bot->StoreNewItemInBestSlots(22054, 20);
    }

    // warlocks need shards for summons and healthstones (6265, non-stacking
    // — each shard eats a bag slot, so top up to 3, never hoard)
    if (bot->getClass() == CLASS_WARLOCK)
    {
        uint32 shards = bot->GetItemCount(6265);
        if (shards < 3)
            bot->StoreNewItemInBestSlots(6265, 3 - shards);
    }

    bot->SaveToDB();

    std::ostringstream out;
    out << "maintained: talents tab " << spec << " refilled, gear enchanted + gemmed, saved";
    ai->TellPlayer(requester, out.str());
    return true;
}

bool StubBrainEmitAction::Execute(Event& event)
{
    // mirror mode (retired debug scaffolding, StubMode=1 only): re-issue the
    // bot's current target. NB it PINS target selection while active.
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || target == bot || sServerFacade.UnitIsDead(target))
        return false;   // live bug: a self-buff moment snapshotted self as target

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
