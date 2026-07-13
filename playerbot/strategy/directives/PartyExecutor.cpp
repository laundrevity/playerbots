#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/PartyExecutor.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/directives/DirectiveMgr.h"
#include "playerbot/strategy/directives/DirectiveValues.h"
#include "playerbot/strategy/directives/ShotCaller.h"

#include "playerbot/thirdparty/nlohmann/json.hpp"

#include "Entities/Bag.h"
#include "Entities/Pet.h"
#include "Groups/Group.h"
#include "Maps/Map.h"
#include "MotionGenerators/MotionMaster.h"
#include "MotionGenerators/PathFinder.h"
#include "Movement/MoveSpline.h"
#include "Movement/MoveSplineInit.h"
#include "Util/Timer.h"

#include <cmath>
#include <fstream>
#include <map>
#include <vector>

using namespace ai;

namespace
{
    constexpr uint32 AFTER_CAST_DELAY_MS = 500;
    constexpr uint32 IDLE_DELAY_MS = 250;
    constexpr uint32 NONCOMBAT_DELAY_MS = 500;

    // dps hold their specials above this fraction of the tank's threat
    // (TBC pulls aggro at 110% melee / 130% ranged; 0.9 leaves margin)
    constexpr float THREAT_CEILING = 0.9f;

    // route chunks and leashes (yards)
    constexpr float ROUTE_LEASH = 35.0f;
    constexpr float ROUTE_CHUNK = 80.0f;    // one smooth spline per chunk
    constexpr float ROUTE_PULL_RANGE = 35.0f;

    // the human tank's pacing checklist: nobody fighting, nobody low,
    // mana users watered — shared by every pull-on-your-own mode
    bool PartyReadyToPull(Group* group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->IsInWorld())
                continue;
            if (member->IsInCombat())
                return false;
            if (member->GetHealth() * 10 < member->GetMaxHealth() * 6)
                return false;   // someone below 60% hp: wait
            if (member->GetPowerType() == POWER_MANA &&
                member->GetPower(POWER_MANA) * 2 < member->GetMaxPower(POWER_MANA))
                return false;   // a mana user below 50%: let them drink
        }
        return true;
    }

    // rogue poison upkeep (DB-verified 2.4.3 top ranks)
    constexpr uint32 INSTANT_POISON_VII = 21927;    // -> enchant 2641 (MH)
    constexpr uint32 DEADLY_POISON_VII = 22054;     // -> enchant 2643 (OH)
    constexpr uint32 INSTANT_POISON_ENCHANT = 2641;
    constexpr uint32 DEADLY_POISON_ENCHANT = 2643;

    // consume a bag poison and put it on the blade, like a player would
    bool ApplyPoisonTo(Player* bot, uint8 slot, uint32 itemId, uint32 enchantId, uint32 charges)
    {
        Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!weapon || weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return false;
        if (!bot->HasItemCount(itemId, 1))
            return false;
        bot->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, false);
        weapon->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchantId, 3600 * IN_MILLISECONDS, charges);
        bot->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, true);
        bot->DestroyItemCount(itemId, 1, true);
        return true;
    }

    // healthstones a warlock might carry (master + lvl-60 ranks, DB-verified)
    const uint32 HEALTHSTONE_IDS[] = { 22105, 22104, 22103, 19013, 19012, 19011 };
    constexpr uint32 SOUL_SHARD = 6265;

    Item* FindBagItem(Player* bot, const uint32* ids, size_t count)
    {
        auto matches = [&](Item* item)
        {
            for (size_t i = 0; i < count; ++i)
                if (item->GetEntry() == ids[i])
                    return true;
            return false;
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (matches(item))
                    return item;
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                    if (Item* item = pBag->GetItemByPos(j))
                        if (matches(item))
                            return item;
        return nullptr;
    }

    // nearest enemy player classified healer by his talents (works for
    // opponents: role detection reads the player's own spec)
    Unit* NearestEnemyHealer(PlayerbotAI* ai, Player* bot)
    {
        AiObjectContext* context = ai->GetAiObjectContext();
        std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
        Unit* best = nullptr;
        float bestDistance = 40.0f;
        for (const ObjectGuid& guid : possible)
        {
            Unit* candidate = ai->GetUnit(guid);
            if (!candidate || !candidate->IsPlayer() || !candidate->IsAlive())
                continue;
            if (!PlayerbotAI::IsHeal((Player*)candidate))
                continue;
            float distance = sServerFacade.GetDistance2d(bot, candidate);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = candidate;
            }
        }
        return best;
    }

    // ---- tank routes (encounters/party_routes.json, installed to etc/) ----
    // The offline DSL: map id -> ordered boss creature_template entries.
    // Spawn guid/position resolved once from the world DB.
    struct RouteBoss
    {
        uint32 entry = 0;
        uint32 dbguid = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    std::map<uint32, std::vector<RouteBoss>> s_routes;
    bool s_routesLoaded = false;

    void LoadRoutes()
    {
        if (s_routesLoaded)
            return;
        s_routesLoaded = true;

        // resolved like the aux confs: relative to cwd (bin/)
        std::ifstream in("../etc/party_routes.json");
        if (!in.is_open())
            in.open("party_routes.json");
        if (!in.is_open())
            return;             // no routes: master-steered mode everywhere

        nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
        if (doc.is_discarded() || !doc.contains("routes") || !doc["routes"].is_object())
        {
            sLog.outError("PartyExecutor: party_routes.json unparseable — tank routing disabled");
            return;
        }

        for (const auto& item : doc["routes"].items())
        {
            uint32 mapId = uint32(atoi(item.key().c_str()));
            if (!item.value().contains("bosses") || !item.value()["bosses"].is_array())
                continue;
            for (const auto& entryNode : item.value()["bosses"])
            {
                if (!entryNode.is_number_unsigned())
                    continue;
                RouteBoss boss;
                boss.entry = entryNode.get<uint32>();
                auto result = WorldDatabase.PQuery(
                    "SELECT guid, position_x, position_y, position_z FROM creature "
                    "WHERE id = %u AND map = %u LIMIT 1", boss.entry, mapId);
                if (!result)
                {
                    sLog.outError("PartyExecutor: route map %u boss entry %u has no spawn — skipped", mapId, boss.entry);
                    continue;
                }
                Field* fields = result->Fetch();
                boss.dbguid = fields[0].GetUInt32();
                boss.x = fields[1].GetFloat();
                boss.y = fields[2].GetFloat();
                boss.z = fields[3].GetFloat();
                s_routes[mapId].push_back(boss);
            }
        }
    }
}

bool PartyExecutor::ShouldOwn(PlayerbotAI* ai, Player* bot)
{
    if (!sPlayerbotAIConfig.executorEnabled || !sPlayerbotAIConfig.directiveEnabled)
        return false;
    // arenas: the executor owns EVERY combatant — masterless opponents get
    // the same reflex kits (the old engine's jank was the difficulty floor).
    // The LLM shot-caller stays exclusive to real-player parties (ArenaTick
    // requires a master), preserving the human side's edge.
    if (bot->InArena())
        // every combatant including masterless healers (HealerTriageTick)
        return bot->IsAlive();
    if (!ai->HasRealPlayerMaster())
        return false;
    if (bot->InBattleGround())
        return false;   // regular bgs keep the old brain
    if (!bot->IsAlive())
        return false;   // the module's dead-state engine handles release/rez
    return true;
}

void PartyExecutor::Tick(PlayerbotAI* ai, Player* bot)
{
    if (bot->IsInCombat())
        CombatTick(ai, bot);
    else
        NonCombatTick(ai, bot);
}

// One legality gate for every cast: known spell, range, LoS, power,
// cooldown, GCD — enforced by the module's cast plumbing.
bool PartyExecutor::Cast(PlayerbotAI* ai, const char* spell, Unit* target)
{
    if (!target)
        return false;
    if (!ai->CanCastSpell(spell, target, 0))
        return false;
    if (!ai->CastSpell(spell, target))
        return false;
    ai->SetAIInternalUpdateDelay(AFTER_CAST_DELAY_MS);
    return true;
}

// ---------------------------------------------------------------- non-combat

bool PartyExecutor::TryChargePull(PlayerbotAI* ai, Player* bot)
{
    if (bot->getClass() != CLASS_WARRIOR || !PlayerbotAI::IsTank(bot))
        return false;

    // pull order = the human's skull, or the brain's kill order
    AiObjectContext* context = ai->GetAiObjectContext();
    Unit* target = nullptr;
    if (Group* group = bot->GetGroup())
        if (Unit* skull = ai->GetUnit(ObjectGuid(group->GetTargetIcon(7))))
            if (!sServerFacade.UnitIsDead(skull))
                target = skull;
    if (!target)
        target = GetDirectiveKillTarget(ai, context);
    if (!target || target->IsInCombat())
        return false;

    float distance = sServerFacade.GetDistance2d(bot, target);
    if (distance < 8.0f || distance > 24.0f)
        return false;   // charge envelope

    // charge needs battle stance; combat tick flips back to defensive
    if (!ai->HasAura("battle stance", bot) && Cast(ai, "battle stance", bot))
        return true;
    return Cast(ai, "charge", target);
}

// walk in / charge / body-pull, depending on range
bool PartyExecutor::EngagePull(PlayerbotAI* ai, Player* bot, Unit* target)
{
    float distance = sServerFacade.GetDistance2d(bot, target);
    if (distance > 24.0f)
    {
        // always RUN to a pull — a lingering walk flag must never slow it
        bot->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(),
                                          target->GetPositionZ(), FORCED_MOVEMENT_RUN);
        ai->SetAIInternalUpdateDelay(AFTER_CAST_DELAY_MS);
        return true;
    }
    if (bot->getClass() == CLASS_WARRIOR && distance >= 8.0f)
    {
        if (!ai->HasAura("battle stance", bot) && Cast(ai, "battle stance", bot))
            return true;
        if (Cast(ai, "charge", target))
            return true;
    }
    // prot paladin: Avenger's Shield is the ranged pull (falls through to a
    // body-pull when untalented/unknown)
    if (bot->getClass() == CLASS_PALADIN && distance >= 8.0f && distance <= 30.0f)
        if (Cast(ai, "avenger's shield", target))
            return true;
    // in melee (or charge unavailable): open by hitting it
    AiObjectContext* context = ai->GetAiObjectContext();
    context->GetValue<Unit*>("current target")->Set(target);
    bot->Attack(target, true);
    if (!bot->CanReachWithMeleeAttack(target))
        ai->DoSpecificAction("reach melee", Event(), true);
    ai->SetAIInternalUpdateDelay(AFTER_CAST_DELAY_MS);
    return true;
}

// Out-of-combat buff round: every class keeps its party buffs up, and a
// buff is only cast into an EMPTY slot — the human's own blessings/buffs
// are never overwritten (complementary, not authoritative).
bool PartyExecutor::KeepPartyBuffed(PlayerbotAI* ai, Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    uint8 cls = bot->getClass();
    if (cls != CLASS_MAGE && cls != CLASS_PALADIN && cls != CLASS_PRIEST && cls != CLASS_DRUID &&
        cls != CLASS_SHAMAN)
        return false;

    // resto shaman: earth shield on the master before the fight
    if (cls == CLASS_SHAMAN && PlayerbotAI::IsHeal(bot))
        if (Player* master = ai->GetMaster())
            if (master->IsInWorld() && master->GetMapId() == bot->GetMapId() &&
                !ai->HasAura("earth shield", master) && Cast(ai, "earth shield", master))
                return true;

    // prot paladin: Righteous Fury goes up before the first pull, not after
    if (cls == CLASS_PALADIN && PlayerbotAI::IsTank(bot) &&
        !ai->HasAura("righteous fury", bot) && Cast(ai, "righteous fury", bot))
        return true;

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId() ||
            sServerFacade.GetDistance2d(bot, member) > 30.0f)
            continue;

        switch (cls)
        {
            case CLASS_MAGE:
                if (member->GetPowerType() == POWER_MANA && !ai->HasAura("arcane intellect", member) &&
                    !ai->HasAura("arcane brilliance", member))
                    if (Cast(ai, "arcane intellect", member))
                        return true;
                break;
            case CLASS_PALADIN:
            {
                // one blessing per paladin; skip anyone already blessed
                if (ai->HasAura("blessing of might", member) || ai->HasAura("blessing of wisdom", member) ||
                    ai->HasAura("blessing of kings", member) || ai->HasAura("blessing of salvation", member) ||
                    ai->HasAura("blessing of light", member) || ai->HasAura("blessing of sanctuary", member) ||
                    ai->HasAura("greater blessing of might", member) || ai->HasAura("greater blessing of wisdom", member) ||
                    ai->HasAura("greater blessing of kings", member) || ai->HasAura("greater blessing of salvation", member))
                    break;
                // physical classes get Might, mana users get Wisdom;
                // a paladin gets Might only if he IS this ret bot
                bool physical = member->getClass() == CLASS_WARRIOR || member->getClass() == CLASS_ROGUE ||
                                member->getClass() == CLASS_HUNTER || member == bot;
                if (Cast(ai, physical ? "blessing of might" : "blessing of wisdom", member))
                    return true;
                break;
            }
            case CLASS_PRIEST:
                if (!ai->HasAura("power word: fortitude", member) && !ai->HasAura("prayer of fortitude", member))
                    if (Cast(ai, "power word: fortitude", member))
                        return true;
                if (member->GetPowerType() == POWER_MANA && !ai->HasAura("divine spirit", member) &&
                    !ai->HasAura("prayer of spirit", member))
                    if (Cast(ai, "divine spirit", member))
                        return true;
                break;
            case CLASS_DRUID:
                if (!ai->HasAura("mark of the wild", member) && !ai->HasAura("gift of the wild", member))
                    if (Cast(ai, "mark of the wild", member))
                        return true;
                break;
            default:
                break;
        }
    }
    return false;
}

// The tank WALKS POINT in a routed dungeon: the objective is the first
// still-alive boss in encounters/party_routes.json order; he advances toward
// it in mmap-path legs, pulls whatever stands in the way, and never outruns
// the group. The master no longer steers — stopping out of leash range (or
// saying "hold") is the human's brake.
bool PartyExecutor::RouteAdvance(PlayerbotAI* ai, Player* bot)
{
    if (!PlayerbotAI::IsTank(bot))
        return false;
    if (HoldPolicy(ai))
        return false;   // "hold" in party chat parks the tank

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    LoadRoutes();
    auto routeItr = s_routes.find(bot->GetMapId());
    if (routeItr == s_routes.end() || routeItr->second.empty())
        return false;   // no route for this map: master-steered mode

    Player* master = ai->GetMaster();
    Group* group = bot->GetGroup();
    if (!master || !group || !master->IsInWorld() || master->GetMapId() != bot->GetMapId())
        return false;

    // wait for the human: never advance further than the leash
    if (sServerFacade.GetDistance2d(bot, master) > ROUTE_LEASH)
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;    // stand ground, don't run back either
    }

    // party not topped: hold position while they drink (don't drift home)
    if (!PartyReadyToPull(group))
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    // objective: first boss in route order that is not confirmed dead
    const RouteBoss* objective = nullptr;
    Unit* objectiveUnit = nullptr;
    for (const RouteBoss& boss : routeItr->second)
    {
        Creature* creature = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, boss.entry, boss.dbguid));
        if (creature && creature->IsDead())
            continue;
        objective = &boss;
        objectiveUnit = creature;   // null until his grid loads: head for the spawn
        break;
    }
    if (!objective)
        return false;   // route cleared — dungeon done

    float objX = objectiveUnit ? objectiveUnit->GetPositionX() : objective->x;
    float objY = objectiveUnit ? objectiveUnit->GetPositionY() : objective->y;
    float objZ = objectiveUnit ? objectiveUnit->GetPositionZ() : objective->z;

    // clear what stands in the way: nearest out-of-combat hostile inside a
    // ~75° cone from the TANK toward the objective (includes the boss when
    // the group finally fronts him)
    float dxo = objX - bot->GetPositionX(), dyo = objY - bot->GetPositionY();
    float dobj = std::sqrt(dxo * dxo + dyo * dyo);
    float hx = dobj > 0.1f ? dxo / dobj : std::cos(bot->GetOrientation());
    float hy = dobj > 0.1f ? dyo / dobj : std::sin(bot->GetOrientation());

    AiObjectContext* context = ai->GetAiObjectContext();
    std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
    Unit* pick = nullptr;
    float best = ROUTE_PULL_RANGE;
    for (const ObjectGuid& guid : possible)
    {
        Unit* candidate = ai->GetUnit(guid);
        if (!candidate || sServerFacade.UnitIsDead(candidate) || candidate->IsInCombat())
            continue;
        if (candidate->IsPlayer())
            continue;
        float dx = candidate->GetPositionX() - bot->GetPositionX();
        float dy = candidate->GetPositionY() - bot->GetPositionY();
        float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < 1.0f || distance > best)
            continue;
        if ((dx * hx + dy * hy) / distance < 0.25f)
            continue;   // outside the cone
        best = distance;
        pick = candidate;
    }
    if (pick)
        return EngagePull(ai, bot, pick);

    // clean road: keep one smooth spline running along the mmap path —
    // point-move legs stutter at every boundary and read as bad pathing
    if (!bot->movespline->Finalized())
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;    // chunk in progress
    }

    PathFinder pathfinder(bot);
    pathfinder.setPathLengthLimit(ROUTE_CHUNK);
    pathfinder.calculate(objX, objY, objZ, false);
    if (pathfinder.getPathType() & PATHFIND_NOPATH)
        return false;

    const PointsArray& points = pathfinder.getPath();
    if (points.size() < 2)
        return false;

    Movement::MoveSplineInit init(*bot);
    init.MovebyPath(points);
    init.SetWalk(false);
    init.Launch();
    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
    return true;
}

void PartyExecutor::ReloadRoutes()
{
    s_routesLoaded = false;
    s_routes.clear();   // next dungeon tick re-reads party_routes.json
}

// The tank pulls ON HIS OWN. The human's facing is the route intent: the
// next pack is the nearest attackable, out-of-combat hostile inside a ~75°
// cone along the master's heading. Pacing is a human tank's checklist —
// nobody in combat, nobody eating/low, healer has mana, party in tow.
bool PartyExecutor::AutoAdvance(PlayerbotAI* ai, Player* bot)
{
    if (bot->getClass() != CLASS_WARRIOR && !PlayerbotAI::IsTank(bot))
        return false;
    if (!PlayerbotAI::IsTank(bot))
        return false;
    if (HoldPolicy(ai))
        return false;   // "hold" in party chat parks the tank

    Player* master = ai->GetMaster();
    Group* group = bot->GetGroup();
    if (!master || !group || !master->IsInWorld() || master->GetMapId() != bot->GetMapId())
        return false;

    // wait for the human: never advance further than ~35y from them
    if (sServerFacade.GetDistance2d(bot, master) > 35.0f)
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;    // stand ground, don't run back either
    }

    // human-tank pacing checklist
    if (!PartyReadyToPull(group))
        return false;

    // next pack: nearest hostile in the master's heading cone
    float heading = master->GetOrientation();
    float hx = std::cos(heading), hy = std::sin(heading);
    AiObjectContext* context = ai->GetAiObjectContext();
    std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
    Unit* pick = nullptr;
    float best = 45.0f;
    for (const ObjectGuid& guid : possible)
    {
        Unit* candidate = ai->GetUnit(guid);
        if (!candidate || sServerFacade.UnitIsDead(candidate) || candidate->IsInCombat())
            continue;
        if (candidate->IsPlayer())
            continue;
        float dx = candidate->GetPositionX() - master->GetPositionX();
        float dy = candidate->GetPositionY() - master->GetPositionY();
        float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < 1.0f || distance > best)
            continue;
        // inside the heading cone? (dot product against the facing vector)
        if ((dx * hx + dy * hy) / distance < 0.25f)
            continue;   // cos(~75°)
        best = distance;
        pick = candidate;
    }
    if (!pick)
        return false;   // nothing ahead: turn to steer me

    return EngagePull(ai, bot, pick);
}

// non-tank bots anchor on the TANK (he leads); the master steers by walking
bool PartyExecutor::FollowLeader(PlayerbotAI* ai, Player* bot)
{
    Player* leader = ai->GetMaster();
    if (Group* group = bot->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
                member->GetMapId() == bot->GetMapId() && PlayerbotAI::IsTank(member) &&
                member->GetPlayerbotAI())
            {
                leader = member;
                break;
            }
        }
    if (!leader || leader == bot || !leader->IsInWorld() || leader->GetMapId() != bot->GetMapId())
        return false;

    if (sServerFacade.GetDistance2d(bot, leader) <= 5.0f)
        return false;
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        return false;   // already in tow

    bot->GetMotionMaster()->MoveFollow(leader, 3.0f, M_PI_F * 0.7f);
    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
    return true;
}

void PartyExecutor::NonCombatTick(PlayerbotAI* ai, Player* bot)
{
    // never trample an in-progress cast: conjures take 3s and summons 10s,
    // and the fall-through to FollowLeader was cancelling them with
    // movement every tick — no healthstone or demon ever got finished
    if (bot->IsNonMeleeSpellCasted(false, true, true))
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return;
    }

    // social reactions the retired strategy engine used to service — the
    // executor answers them itself (party/guild/arena invites, charter
    // signatures, trades). Packet handlers arm these triggers upstream in
    // UpdateAIInternal; without this loop the events rot unanswered.
    static const struct { const char* trigger; const char* action; } SOCIAL[] = {
        { "group invite",      "accept invitation" },
        { "petition offer",    "petition sign" },
        { "guild accept",      "guild accept" },
        { "arena team invite", "arena team accept" },
        { "trade status",      "accept trade" },
        { "bg status",         "bg status" },      // queue pops + arena ports
        { "ready check",       "ready check" },
        { "uninvite",          "uninvite" },
    };
    AiObjectContext* socialContext = ai->GetAiObjectContext();
    for (const auto& social : SOCIAL)
    {
        Trigger* trigger = socialContext->GetTrigger(social.trigger);
        if (!trigger)
            continue;
        Event socialEvent = trigger->Check();
        if (socialEvent.getSource().empty())
            continue;
        trigger->Reset();
        if (!strcmp(social.trigger, "trade status"))
        {
            // a warlock being traded by the master offers a healthstone —
            // the whole point of opening trade with your lock
            if (bot->getClass() == CLASS_WARLOCK)
                if (TradeData* trade = bot->GetTradeData())
                    if (!trade->GetItem(TradeSlots(0)))
                        if (Item* stone = FindBagItem(bot, HEALTHSTONE_IDS, sizeof(HEALTHSTONE_IDS) / sizeof(uint32)))
                            trade->SetItem(TradeSlots(0), stone);
            ai->DoSpecificAction(social.action, socialEvent, true);
            ai->DoSpecificAction("equip upgrades", Event(), true);
        }
        else
            ai->DoSpecificAction(social.action, socialEvent, true);
        return;
    }

    // directives are consumed out of combat too (pull orders arrive here)
    if (sDirectiveMgr.HasPending(bot->GetObjectGuid()))
        if (ai->DoSpecificAction("apply directive", Event(), true))
            return;

    // upkeep between pulls (humans drink; bots that never drink are a tell)
    if (bot->GetPowerType() == POWER_MANA && bot->GetPower(POWER_MANA) * 2 < bot->GetMaxPower(POWER_MANA))
        if (ai->DoSpecificAction("drink", Event(), true))
            return;
    if (bot->GetHealth() * 2 < bot->GetMaxHealth())
        if (ai->DoSpecificAction("food", Event(), true))
            return;

    // keep the party buffed before anyone thinks about pulling
    if (KeepPartyBuffed(ai, bot))
        return;

    // warlock upkeep: a demon out, soul link on, a healthstone in the bags
    if (bot->getClass() == CLASS_WARLOCK)
    {
        Pet* demon = bot->GetPet();

        // arena prep, the classic lock open: hard-cast a voidwalker, sac it
        // for the absorb shield, hard-cast the felhunter after — Fel
        // Domination stays banked for a mid-fight pet death
        if (bot->InArena() && !ai->HasAura("sacrifice", bot))
        {
            if (!demon && Cast(ai, "summon voidwalker", bot))
                return;
            if (demon && demon->GetEntry() == 1860 /*Voidwalker*/ && demon->IsAlive())
            {
                demon->CastSpell(demon, 27273 /*Sacrifice r7*/, TRIGGERED_NONE);
                ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
                return;
            }
        }

        if (!demon)
        {
            if (Cast(ai, "summon felhunter", bot) || Cast(ai, "summon voidwalker", bot) ||
                Cast(ai, "summon imp", bot))
                return;
            sLog.outBasic("PartyExecutor: %s cannot summon any demon (felhunter castable=%d, imp castable=%d)",
                          bot->GetName(), ai->CanCastSpell("summon felhunter", bot, 0),
                          ai->CanCastSpell("summon imp", bot, 0));
        }
        else if (demon->IsAlive() && !ai->HasAura("soul link", bot) && Cast(ai, "soul link", demon))
            return;     // SL/SL without soul link is half a spec

        if (!FindBagItem(bot, HEALTHSTONE_IDS, sizeof(HEALTHSTONE_IDS) / sizeof(uint32)))
        {
            if (Cast(ai, "create healthstone", bot))
                return;
            // shard hoard is the usual blocker (non-stacking, bags fill up):
            // burn the surplus like a human lock cleaning bags
            uint32 shards = bot->GetItemCount(SOUL_SHARD);
            if (shards > 3)
            {
                bot->DestroyItemCount(SOUL_SHARD, shards - 3, true);
                ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
                return;
            }
            SpellCastResult createResult = SPELL_CAST_OK;
            ai->CanCastSpell("create healthstone", bot, 0, nullptr, false, false, false, &createResult);
            sLog.outBasic("PartyExecutor: %s cannot create healthstone (result=%u, shards=%u)",
                          bot->GetName(), uint32(createResult), shards);
        }
    }

    // rogue: poisons on both blades, then stealth before the fight finds you
    if (bot->getClass() == CLASS_ROGUE)
    {
        if (ApplyPoisonTo(bot, EQUIPMENT_SLOT_MAINHAND, INSTANT_POISON_VII, INSTANT_POISON_ENCHANT, 40) ||
            ApplyPoisonTo(bot, EQUIPMENT_SLOT_OFFHAND, DEADLY_POISON_VII, DEADLY_POISON_ENCHANT, 30))
        {
            ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
            return;
        }
        if (bot->InArena() && !ai->HasAura("stealth", bot) && Cast(ai, "stealth", bot))
            return;

        // arena, stealthed, out of combat (gates or a vanish reset): creep
        // to the nearest enemy and open with cheap shot — never auto-attack
        // out of stealth
        if (bot->InArena() && ai->HasAura("stealth", bot))
        {
            AiObjectContext* stealthContext = ai->GetAiObjectContext();
            std::list<ObjectGuid> possible = stealthContext->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
            Unit* enemy = nullptr;
            float best = 60.0f;
            for (const ObjectGuid& guid : possible)
            {
                Unit* candidate = ai->GetUnit(guid);
                if (!candidate || !candidate->IsPlayer() || !candidate->IsAlive())
                    continue;
                float distance = sServerFacade.GetDistance2d(bot, candidate);
                if (distance < best)
                {
                    best = distance;
                    enemy = candidate;
                }
            }
            if (enemy)
            {
                if (bot->CanReachWithMeleeAttack(enemy))
                {
                    if (Cast(ai, "cheap shot", enemy))
                        return;
                }
                else
                {
                    bot->GetMotionMaster()->MovePoint(0, enemy->GetPositionX(), enemy->GetPositionY(),
                                                      enemy->GetPositionZ(), FORCED_MOVEMENT_RUN);
                    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
                    return;
                }
            }
        }
    }

    // arena, gates open, out of combat: converge on the enemy AS A TEAM —
    // nobody camps the starting room (non-stealth path; rogues creep above)
    if (bot->InArena() && bot->GetBattleGround() &&
        bot->GetBattleGround()->GetStatus() == STATUS_IN_PROGRESS &&
        !ai->HasAura("stealth", bot))
    {
        // healers advance WITH the team, not at the enemy: shadow the
        // nearest living dps partner (12yd leash); solo-survivor healers
        // fall through to the enemy-convergence path below
        if (PlayerbotAI::IsHeal(bot))
            if (Group* arenaGroup = bot->GetGroup())
            {
                Player* escort = nullptr;
                float escortDistance = 10000.0f;
                for (GroupReference* itr = arenaGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->getSource();
                    if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
                        PlayerbotAI::IsHeal(member))
                        continue;
                    float distance = sServerFacade.GetDistance2d(bot, member);
                    if (distance < escortDistance)
                    {
                        escortDistance = distance;
                        escort = member;
                    }
                }
                if (escort)
                {
                    if (escortDistance > 12.0f)
                    {
                        bot->GetMotionMaster()->MovePoint(0, escort->GetPositionX(), escort->GetPositionY(),
                                                          escort->GetPositionZ(), FORCED_MOVEMENT_RUN);
                        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
                    }
                    return;
                }
            }

        AiObjectContext* arenaContext = ai->GetAiObjectContext();
        std::list<ObjectGuid> possible = arenaContext->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
        Unit* enemy = nullptr;
        float best = 200.0f;
        for (const ObjectGuid& guid : possible)
        {
            Unit* candidate = ai->GetUnit(guid);
            if (!candidate || !candidate->IsPlayer() || !candidate->IsAlive())
                continue;
            float distance = sServerFacade.GetDistance2d(bot, candidate);
            if (distance < best)
            {
                best = distance;
                enemy = candidate;
            }
        }
        if (enemy)
        {
            if (EngageTarget(ai, bot, enemy))
                return;
            bot->GetMotionMaster()->MovePoint(0, enemy->GetPositionX(), enemy->GetPositionY(),
                                              enemy->GetPositionZ(), FORCED_MOVEMENT_RUN);
            ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
            return;
        }
    }

    // tank: skull/LLM pull orders first, then route the dungeon on your
    // own; master-steered advance is the fallback outside routed maps
    if (TryChargePull(ai, bot))
        return;
    if (RouteAdvance(ai, bot))
        return;
    if (AutoAdvance(ai, bot))
        return;

    // everyone else: the tank leads, you follow him (master as fallback).
    // No loot, no travel, no grind — ever.
    if (FollowLeader(ai, bot))
        return;

    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
}

// -------------------------------------------------------------------- combat

bool PartyExecutor::KeepCcApplied(PlayerbotAI* ai, Player* bot)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    Directive directive = context->GetValue<Directive>("directive")->Get();
    if (!directive.IsActiveNow(WorldTimer::getMSTime()) || directive.cc.empty())
        return false;

    for (const Directive::CcAssignment& assignment : directive.cc)
    {
        Unit* target = ai->GetUnit(assignment.guid);
        if (!target || sServerFacade.UnitIsDead(target))
            continue;
        if (ai->HasAura(assignment.spell, target))
            continue;
        if (Cast(ai, assignment.spell.c_str(), target))
            return true;
    }
    return false;
}

bool PartyExecutor::TryInterrupt(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (!target || !target->IsNonMeleeSpellCasted(false, true, true))
        return false;

    switch (bot->getClass())
    {
        case CLASS_ROGUE:   return Cast(ai, "kick", target);
        case CLASS_MAGE:    return Cast(ai, "counterspell", target);
        case CLASS_WARRIOR: return Cast(ai, "shield bash", target);
        case CLASS_SHAMAN:  return Cast(ai, "earth shock", target);
        default:            return false;
    }
}

Unit* PartyExecutor::LooseMobOnParty(PlayerbotAI* ai, Player* bot)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    for (const ObjectGuid& guid : attackers)
    {
        Unit* attacker = ai->GetUnit(guid);
        if (!attacker || sServerFacade.UnitIsDead(attacker))
            continue;
        Unit* victim = attacker->GetVictim();
        if (!victim || victim == bot)
            continue;
        Player* member = victim->IsPlayer() ? (Player*)victim : nullptr;
        if (!member || !bot->GetGroup() || member->GetGroup() != bot->GetGroup())
            continue;
        if (PlayerbotAI::IsTank(member))
            continue;
        return attacker;
    }
    return nullptr;
}

// tank threat cycling: the engaged mob where MY threat is lowest is the one
// about to peel off — feed it the next sunder/slam
Unit* PartyExecutor::LowestThreatAttacker(PlayerbotAI* ai, Player* bot)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    Unit* lowest = nullptr;
    float best = 0.0f;
    for (const ObjectGuid& guid : attackers)
    {
        Unit* attacker = ai->GetUnit(guid);
        if (!attacker || sServerFacade.UnitIsDead(attacker))
            continue;
        if (!bot->CanReachWithMeleeAttack(attacker))
            continue;
        float threat = attacker->getThreatManager().getThreat(bot);
        if (!lowest || threat < best)
        {
            best = threat;
            lowest = attacker;
        }
    }
    return lowest;
}

Unit* PartyExecutor::PickTarget(PlayerbotAI* ai, Player* bot)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    Group* group = bot->GetGroup();

    // 3a. skull mark (human intent always wins)
    if (group)
        if (Unit* skull = ai->GetUnit(ObjectGuid(group->GetTargetIcon(7))))
            if (!sServerFacade.UnitIsDead(skull) && bot->IsWithinDistInMap(skull, sPlayerbotAIConfig.sightDistance))
                return skull;

    // 3b. directive kill order
    if (Unit* directed = GetDirectiveKillTarget(ai, context))
        return directed;

    // 3c. role default
    if (PlayerbotAI::IsTank(bot))
    {
        // emergency first, then keep the pack glued via lowest-threat cycling
        if (Unit* loose = LooseMobOnParty(ai, bot))
            return loose;
        if (Unit* lowest = LowestThreatAttacker(ai, bot))
            return lowest;
        Unit* current = bot->GetVictim();
        if (current && !sServerFacade.UnitIsDead(current))
            return current;
    }
    else if (group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member != bot && member->IsInWorld() && PlayerbotAI::IsTank(member))
                if (Unit* tanked = member->GetVictim())
                    if (!sServerFacade.UnitIsDead(tanked))
                        return tanked;
        }
    }

    // 3d. nearest attacker
    std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    Unit* nearest = nullptr;
    float best = 1000.0f;
    for (const ObjectGuid& guid : attackers)
    {
        Unit* attacker = ai->GetUnit(guid);
        if (!attacker || sServerFacade.UnitIsDead(attacker))
            continue;
        float distance = sServerFacade.GetDistance2d(bot, attacker);
        if (distance < best)
        {
            best = distance;
            nearest = attacker;
        }
    }
    if (nearest)
        return nearest;

    // 3e. arena: focus fire — a living teammate's target outranks nearest
    if (bot->InArena() && group)
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive())
                continue;
            if (Unit* focused = member->GetVictim())
                if (focused->IsPlayer() && focused->IsAlive())
                    return focused;
        }

    // arena: nobody engaged yet — the nearest enemy player IS the fight
    if (bot->InArena())
    {
        std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
        for (const ObjectGuid& guid : possible)
        {
            Unit* candidate = ai->GetUnit(guid);
            if (!candidate || !candidate->IsPlayer() || sServerFacade.UnitIsDead(candidate))
                continue;
            float distance = sServerFacade.GetDistance2d(bot, candidate);
            if (distance < best)
            {
                best = distance;
                nearest = candidate;
            }
        }
    }
    return nearest;
}

bool PartyExecutor::EngageTarget(PlayerbotAI* ai, Player* bot, Unit* target)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    if (context->GetValue<Unit*>("current target")->Get() != target)
        context->GetValue<Unit*>("current target")->Set(target);

    bool ranged = ai->IsRanged(bot);
    if (bot->GetVictim() != target)
        bot->Attack(target, !ranged);

    if (!ranged && !bot->CanReachWithMeleeAttack(target))
        return ai->DoSpecificAction("reach melee", Event(), true);
    if (ranged && !bot->IsWithinDistInMap(target, 25.0f))
        return ai->DoSpecificAction("reach spell", Event(), true);
    return false;
}

// directives' cooldown policy: burn -> offensive cooldowns allowed
bool PartyExecutor::BurnPolicy(PlayerbotAI* ai)
{
    Directive directive = ai->GetAiObjectContext()->GetValue<Directive>("directive")->Get();
    return directive.IsActiveNow(WorldTimer::getMSTime()) &&
           directive.cooldowns == CooldownPolicy::Burn;
}

// "hold" doubles as the pull brake: 'hold' in party chat pauses the tank's
// auto-advance, 'go'/'keep pulling' (normal/burn) resumes it
bool PartyExecutor::HoldPolicy(PlayerbotAI* ai)
{
    Directive directive = ai->GetAiObjectContext()->GetValue<Directive>("directive")->Get();
    return directive.IsActiveNow(WorldTimer::getMSTime()) &&
           directive.cooldowns == CooldownPolicy::Hold;
}

// good tanks turn the mob away from the party: stand on the FAR side of the
// target from the party's center — the mob turns to face the tank, so its
// frontal cone (cleaves, breaths) points into empty space. Memoryless: once
// the tank stands there the condition self-clears and no more moves happen.
bool PartyExecutor::TankFaceAway(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (target->GetVictim() != bot)
        return false;   // reposition only once the mob is actually on me
    if (bot->IsMoving() || target->IsMoving())
        return false;
    if (!bot->CanReachWithMeleeAttack(target))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;
    float cx = 0.0f, cy = 0.0f;
    uint32 count = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;
        if (sServerFacade.GetDistance2d(bot, member) > 40.0f)
            continue;
        cx += member->GetPositionX();
        cy += member->GetPositionY();
        ++count;
    }
    if (!count)
        return false;
    cx /= count;
    cy /= count;

    float tx = target->GetPositionX(), ty = target->GetPositionY();
    float dx = tx - cx, dy = ty - cy;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f)
        return false;   // party is on top of the mob; nothing sane to do
    dx /= length;
    dy /= length;

    // far-side melee spot, 2y past the target away from the party
    float px = tx + dx * 2.0f, py = ty + dy * 2.0f;
    float bx = bot->GetPositionX() - px, by = bot->GetPositionY() - py;
    float away = std::sqrt(bx * bx + by * by);
    if (away < 2.5f)
        return false;   // already on the far side
    if (away > 15.0f)
        return false;   // something is off; don't sprint across the room

    bot->GetMotionMaster()->MovePoint(0, px, py, target->GetPositionZ());
    ai->SetAIInternalUpdateDelay(300);
    return true;
}

// 4. dps discipline: never ride past the tank's threat
bool PartyExecutor::ThreatCapped(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (PlayerbotAI::IsTank(bot))
        return false;
    Unit* tank = target->GetVictim();
    if (!tank || tank == bot || !tank->IsPlayer() || !PlayerbotAI::IsTank((Player*)tank))
        return false;   // nobody tanking it: no ceiling to respect
    float mine = target->getThreatManager().getThreat(bot);
    float tanks = target->getThreatManager().getThreat(tank);
    return tanks > 0.0f && mine > THREAT_CEILING * tanks;
}

// melee dps belong behind the target (when someone else is tanking it)
bool PartyExecutor::MeleeGetBehind(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ai->IsRanged(bot) || PlayerbotAI::IsTank(bot))
        return false;
    if (target->IsPlayer())
        return false;   // never chase a moving player's back: the reposition
                        // loop starves the whole rotation (arena regression)
    if (target->GetVictim() == bot)
        return false;
    if (!target->HasInArc(bot, M_PI_F))
        return false;   // already behind
    return ai->DoSpecificAction("set behind", Event(), true);
}

// ---- class rotations (researched for 2.4.3; sources in the repo docs) ----

bool PartyExecutor::TankWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (!ai->HasAura("defensive stance", bot) && Cast(ai, "defensive stance", bot))
        return true;

    if (target->GetVictim() && target->GetVictim() != bot && Cast(ai, "taunt", target))
        return true;

    if (TankFaceAway(ai, bot, target))
        return true;

    // survival while actively tanked
    if (target->GetVictim() == bot && Cast(ai, "shield block", bot))
        return true;

    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    uint32 meleeCount = 0;
    for (const ObjectGuid& guid : attackers)
        if (Unit* attacker = ai->GetUnit(guid))
            if (bot->CanReachWithMeleeAttack(attacker))
                ++meleeCount;

    // Shield Slam > Revenge > Devastate/Sunder, Thunder Clap + Demo on packs,
    // Heroic Strike as the rage dump (icy-veins/wowtbc.gg prot priority)
    if (Cast(ai, "shield slam", target))
        return true;
    if (Cast(ai, "revenge", target))
        return true;
    if (meleeCount >= 2 && Cast(ai, "thunder clap", target))
        return true;
    if (meleeCount >= 2 && Cast(ai, "demoralizing shout", target))
        return true;
    if (Cast(ai, "devastate", target))
        return true;
    if (Cast(ai, "sunder armor", target))
        return true;
    if (bot->GetPower(POWER_RAGE) > 500 && Cast(ai, "heroic strike", target))
        return true;
    if (meleeCount >= 2 && bot->GetPower(POWER_RAGE) > 400 && Cast(ai, "cleave", target))
        return true;
    if (bot->GetPower(POWER_RAGE) < 200 && Cast(ai, "bloodrage", bot))
        return true;
    return false;
}

bool PartyExecutor::RogueTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ThreatCapped(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;   // white swings only until the tank pulls ahead
    }

    if (BurnPolicy(ai))
    {
        if (Cast(ai, "adrenaline rush", bot))
            return true;
        if (Cast(ai, "blade flurry", bot))
            return true;
    }

    uint8 combo = bot->GetComboPoints();

    // ---- players are a different sport: control beats sustained dps ----
    if (target->IsPlayer())
    {
        // survival: evasion while being trained, cloak purges the dots,
        // vanish resets the fight, preparation refunds vanish
        if (target->GetVictim() == bot && bot->GetHealthPercent() < 60.0f && Cast(ai, "evasion", bot))
            return true;
        if (bot->GetHealthPercent() < 40.0f &&
            bot->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) && Cast(ai, "cloak of shadows", bot))
            return true;
        if (bot->GetHealthPercent() < 25.0f)
        {
            if (Cast(ai, "vanish", bot))
                return true;
            if (Cast(ai, "preparation", bot))
                return true;
        }

        // the enemy healer is everyone's problem: kick his casts even
        // off-target; blind him when the kill target enters the window
        if (Unit* healer = NearestEnemyHealer(ai, bot))
        {
            if (healer != target)
            {
                if (healer->IsNonMeleeSpellCasted(false, true, true) &&
                    bot->CanReachWithMeleeAttack(healer) && Cast(ai, "kick", healer))
                    return true;
                if (target->GetHealthPercent() < 50.0f &&
                    !healer->HasAuraType(SPELL_AURA_MOD_STUN) &&
                    !healer->HasAuraType(SPELL_AURA_MOD_CONFUSE) && Cast(ai, "blind", healer))
                    return true;
            }
        }

        // opener out of stealth (shadowstep bridges the gap when talented)
        if (ai->HasAura("stealth", bot))
        {
            if (!bot->CanReachWithMeleeAttack(target) && Cast(ai, "shadowstep", target))
                return true;
            if (Cast(ai, "cheap shot", target))
                return true;
        }

        // gouge is the backup interrupt once kick is down
        if (target->IsNonMeleeSpellCasted(false, true, true) &&
            !ai->CanCastSpell("kick", target, 0) && Cast(ai, "gouge", target))
            return true;

        // stunlock: kidney at 3+ unless they're already stunned (DR waste)
        if (combo >= 3 && !target->HasAuraType(SPELL_AURA_MOD_STUN) && Cast(ai, "kidney shot", target))
            return true;
        if (combo >= 5 && Cast(ai, "eviscerate", target))
            return true;
        // stay glued to the kill target
        if (!bot->CanReachWithMeleeAttack(target))
        {
            if (Cast(ai, "shadowstep", target))
                return true;
            if (Cast(ai, "sprint", bot))
                return true;
        }
        if (Cast(ai, "sinister strike", target))
            return true;
        return false;
    }

    // Slice and Dice uptime is the whole spec (icy-veins combat rogue)
    if (combo >= 2 && !ai->HasAura("slice and dice", bot) && Cast(ai, "slice and dice", target))
        return true;
    if (combo >= 5 && ai->HasAura("slice and dice", bot))
    {
        // long-lived targets get Rupture, everything else gets Eviscerate
        bool bossLike = target->GetMaxHealth() > bot->GetMaxHealth() * 3;
        if (bossLike && !ai->HasAura("rupture", target, false, true) && Cast(ai, "rupture", target))
            return true;
        if (Cast(ai, "eviscerate", target))
            return true;
    }
    if (Cast(ai, "sinister strike", target))
        return true;
    return false;
}

bool PartyExecutor::MageTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    // NB: no Polymorph here, ever — cc happens only via directive assignment
    if (ThreatCapped(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;   // stop casting until the tank pulls ahead
    }

    // spec by talent tab (legible sensor): 0 = arcane, 1 = fire, 2 = frost
    int spec = AiFactory::GetPlayerSpecTab(bot);

    if (spec == 1)
    {
        if (BurnPolicy(ai) && Cast(ai, "combustion", bot))
            return true;
        // 5x Improved Scorch stacks, refresh under 4s, else fireball stream
        Aura* vulnerability = ai->GetAura("fire vulnerability", target);
        uint32 stacks = vulnerability ? vulnerability->GetStackAmount() : 0;
        int32 remaining = vulnerability ? vulnerability->GetAuraDuration() : 0;
        if ((stacks < 5 || remaining < 4000) && Cast(ai, "scorch", target))
            return true;
        if (Cast(ai, "fireball", target))
            return true;
    }
    else if (spec == 0)
    {
        if (BurnPolicy(ai) && Cast(ai, "arcane power", bot))
            return true;
        // arcane blast while mana holds, frostbolt as the recovery filler
        if (bot->GetPower(POWER_MANA) * 5 > bot->GetMaxPower(POWER_MANA) * 2 &&
            Cast(ai, "arcane blast", target))
            return true;
        if (Cast(ai, "frostbolt", target))
            return true;
    }
    else
    {
        if (BurnPolicy(ai) && Cast(ai, "icy veins", bot))
            return true;
        if (Cast(ai, "frostbolt", target))
            return true;
    }

    if (Cast(ai, "fire blast", target))
        return true;
    if (Cast(ai, "shoot", target))
        return true;
    return false;
}

bool PartyExecutor::RetPaladinTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    // NB: no Repentance here, ever — cc happens only via directive assignment
    if (ThreatCapped(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;
    }

    // seal up 100%, Judgement + Crusader Strike on cooldown (warcrafttavern
    // ret priority; judgement consumes the seal — rule 1 reseals next tick)
    if (!ai->HasAura("seal of command", bot) && !ai->HasAura("seal of blood", bot) &&
        !ai->HasAura("seal of righteousness", bot))
    {
        if (Cast(ai, "seal of command", bot) || Cast(ai, "seal of blood", bot) ||
            Cast(ai, "seal of righteousness", bot))
            return true;
    }
    if (BurnPolicy(ai) && Cast(ai, "avenging wrath", bot))
        return true;

    // vs players: hammer of justice the enemy healer inside the kill window
    if (target->IsPlayer())
        if (Unit* healer = NearestEnemyHealer(ai, bot))
            if (healer != target && target->GetHealthPercent() < 50.0f &&
                !healer->HasAuraType(SPELL_AURA_MOD_STUN) && Cast(ai, "hammer of justice", healer))
                return true;

    if (Cast(ai, "judgement", target))
    {
        // judgement is off the GCD and consumes the seal: reseal immediately
        if (!Cast(ai, "seal of command", bot))
            if (!Cast(ai, "seal of blood", bot))
                Cast(ai, "seal of righteousness", bot);
        return true;
    }
    if (Cast(ai, "crusader strike", target))
        return true;
    // ranged execute
    if (target->GetHealthPercent() < 20.0f && Cast(ai, "hammer of wrath", target))
        return true;
    return false;
}

bool PartyExecutor::ArmsWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ThreatCapped(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;
    }

    if (!ai->HasAura("battle stance", bot) && Cast(ai, "battle stance", bot))
        return true;

    if (BurnPolicy(ai) && Cast(ai, "death wish", bot))
        return true;

    // players get snared before anything else (kite control beats damage)
    if (target->IsPlayer() && !ai->HasAura("hamstring", target) && Cast(ai, "hamstring", target))
        return true;

    if (target->GetHealthPercent() < 20.0f && Cast(ai, "execute", target))
        return true;

    if (Cast(ai, "mortal strike", target))
        return true;

    // rage dump only above the MS reserve
    if (bot->GetPower(POWER_RAGE) > 60 && Cast(ai, "heroic strike", target))
        return true;

    return false;
}

bool PartyExecutor::ProtPaladinTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    // Righteous Fury is the tank switch (+90% holy threat): without it every
    // heal outthreats him — nothing else in this rotation matters first
    if (!ai->HasAura("righteous fury", bot) && Cast(ai, "righteous fury", bot))
        return true;

    // rescue: Righteous Defense taunts mobs off the teammate they're eating
    // (it targets the ALLY, and pulls up to 3 mobs back)
    if (Unit* loose = LooseMobOnParty(ai, bot))
        if (Unit* victim = loose->GetVictim())
            if (victim != bot && victim->IsPlayer() && Cast(ai, "righteous defense", victim))
                return true;

    // TBC prot threat priority: Holy Shield > Judgement (of Righteousness)
    // > Avenger's Shield > Consecration (wowhead/tavern prot guides)
    if (Cast(ai, "holy shield", bot))
        return true;

    if (!ai->HasAura("seal of righteousness", bot) && !ai->HasAura("seal of vengeance", bot))
        if (Cast(ai, "seal of righteousness", bot) || Cast(ai, "seal of vengeance", bot))
            return true;

    if (Cast(ai, "judgement", target))
    {
        // judgement consumes the seal: reseal immediately (off-GCD)
        if (!Cast(ai, "seal of righteousness", bot))
            Cast(ai, "seal of vengeance", bot);
        return true;
    }

    if (Cast(ai, "avenger's shield", target))
        return true;

    // consecration is the pack glue; stop below a third mana so Spiritual
    // Attunement income keeps the loop alive
    if (bot->GetPower(POWER_MANA) * 3 > bot->GetMaxPower(POWER_MANA) &&
        Cast(ai, "consecration", bot))
        return true;

    return false;
}

bool PartyExecutor::WarlockTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ThreatCapped(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;
    }

    // pet died mid-fight: Fel Domination is exactly for this — instant
    // replacement so soul link comes back (never hard-cast 10s in combat)
    if (!bot->GetPet())
    {
        if (Cast(ai, "fel domination", bot))
            return true;
        if (ai->HasAura("fel domination", bot) &&
            (Cast(ai, "summon felhunter", bot) || Cast(ai, "summon voidwalker", bot)))
            return true;
    }

    // the demon fights or the warlock is half a class: keep it on target,
    // and keep soul link up even mid-fight (it drops when the pet dies)
    if (Pet* pet = bot->GetPet())
    {
        if (pet->IsAlive() && !ai->HasAura("soul link", bot) && Cast(ai, "soul link", pet))
            return true;
        if (pet->IsAlive() && pet->GetVictim() != target && pet->AI())
        {
            pet->AttackStop();
            pet->GetMotionMaster()->Clear();
            pet->AI()->AttackStart(target);
        }
    }

    // panic buttons: coil for the heal+cc, fear the melee eating us alive
    if (bot->GetHealthPercent() < 50.0f && Cast(ai, "death coil", target))
        return true;
    if (target->IsPlayer() && target->GetVictim() == bot &&
        bot->CanReachWithMeleeAttack(target) &&
        !target->HasAuraType(SPELL_AURA_MOD_FEAR) && !target->HasAuraType(SPELL_AURA_MOD_STUN) &&
        Cast(ai, "fear", target))
        return true;

    // dot suite (SL/SL bread and butter); Cast() skips already-applied ranks
    if (!ai->HasAura("curse of agony", target, false, true) && Cast(ai, "curse of agony", target))
        return true;
    if (!ai->HasAura("corruption", target, false, true) && Cast(ai, "corruption", target))
        return true;
    if (!ai->HasAura("siphon life", target, false, true) && Cast(ai, "siphon life", target))
        return true;

    // resource loop: tap when full of health, drain when not
    if (bot->GetPower(POWER_MANA) * 10 < bot->GetMaxPower(POWER_MANA) * 3 &&
        bot->GetHealthPercent() > 60.0f && Cast(ai, "life tap", bot))
        return true;
    if (bot->GetHealthPercent() < 80.0f && Cast(ai, "drain life", target))
        return true;
    if (Cast(ai, "shadow bolt", target))
        return true;
    return false;
}

// Generic healer triage: lowest groupmate in range gets the class's heal
// ladder. Built so ARENA opponent healers can live on the executor (the
// old engine left them camping the start room — observed as "fighting
// them one at a time"). Movement: stay glued to the triage target /
// partner; never chase enemies.
bool PartyExecutor::HealerTriageTick(PlayerbotAI* ai, Player* bot)
{
    Group* group = bot->GetGroup();

    Player* lowest = bot;
    float lowestPct = bot->GetHealthPercent();
    Player* partner = nullptr;          // nearest living non-self groupmate
    float partnerDistance = 10000.0f;
    if (group)
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId())
                continue;
            float distance = sServerFacade.GetDistance2d(bot, member);
            if (distance < partnerDistance)
            {
                partnerDistance = distance;
                partner = member;
            }
            if (distance <= 40.0f && member->GetHealthPercent() < lowestPct)
            {
                lowestPct = member->GetHealthPercent();
                lowest = member;
            }
        }

    // a hurt partner out of heal range beats everything: close the gap
    if (partner && partnerDistance > 35.0f)
    {
        bot->GetMotionMaster()->MovePoint(0, partner->GetPositionX(), partner->GetPositionY(),
                                          partner->GetPositionZ(), FORCED_MOVEMENT_RUN);
        return true;
    }

    switch (bot->getClass())
    {
        case CLASS_SHAMAN:
            return RestoShamanTick(ai, bot);
        case CLASS_PRIEST:
            if (lowestPct < 35.0f)
            {
                if (!ai->HasAura("weakened soul", lowest) && Cast(ai, "power word: shield", lowest))
                    return true;
                if (Cast(ai, "flash heal", lowest))
                    return true;
            }
            if (lowestPct < 60.0f && Cast(ai, "flash heal", lowest))
                return true;
            if (lowestPct < 85.0f && !ai->HasAura("renew", lowest) && Cast(ai, "renew", lowest))
                return true;
            break;
        case CLASS_PALADIN:
            if (lowestPct < 35.0f)
            {
                if (Cast(ai, "holy shock", lowest))
                    return true;
                if (Cast(ai, "flash of light", lowest))
                    return true;
            }
            if (lowestPct < 60.0f && Cast(ai, "holy light", lowest))
                return true;
            if (lowestPct < 85.0f && Cast(ai, "flash of light", lowest))
                return true;
            break;
        case CLASS_DRUID:
            if (lowestPct < 35.0f)
            {
                if (Cast(ai, "nature's swiftness", bot))
                    return true;
                if (Cast(ai, "healing touch", lowest))
                    return true;
            }
            if (lowestPct < 60.0f)
            {
                if (!ai->HasAura("regrowth", lowest) && Cast(ai, "regrowth", lowest))
                    return true;
                if (Cast(ai, "healing touch", lowest))
                    return true;
            }
            if (lowestPct < 85.0f && !ai->HasAura("rejuvenation", lowest) && Cast(ai, "rejuvenation", lowest))
                return true;
            break;
        default:
            break;
    }

    // nothing to heal: hold formation on the partner (12yd leash)
    if (partner && partnerDistance > 12.0f)
    {
        bot->GetMotionMaster()->MovePoint(0, partner->GetPositionX(), partner->GetPositionY(),
                                          partner->GetPositionZ(), FORCED_MOVEMENT_RUN);
        return true;
    }
    return false;
}

// Resto shaman: triage owns the tick — the healer never chases targets.
// Totem suite drops once the fight starts and refreshes when destroyed.
bool PartyExecutor::RestoShamanTick(PlayerbotAI* ai, Player* bot)
{
    Group* group = bot->GetGroup();

    // 1. triage first: lowest party member in range (including self)
    Player* lowest = bot;
    float lowestPct = bot->GetHealthPercent();
    if (group)
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId() ||
                sServerFacade.GetDistance2d(bot, member) > 40.0f)
                continue;
            if (member->GetHealthPercent() < lowestPct)
            {
                lowestPct = member->GetHealthPercent();
                lowest = member;
            }
        }

    if (lowestPct < 30.0f)
    {
        // emergency: NS makes the next Healing Wave instant
        if (Cast(ai, "nature's swiftness", bot))
            return true;
        if (Cast(ai, "healing wave", lowest))
            return true;
    }
    if (lowestPct < 55.0f && Cast(ai, "healing wave", lowest))
        return true;
    if (lowestPct < 80.0f && Cast(ai, "lesser healing wave", lowest))
        return true;

    // 2. totem suite by SLOT ownership — the old self-aura check never saw
    // the buff when the shaman stood outside totem radius, so she re-dropped
    // windfury every tick (GCD + mana gone, zero heals). Totems also wait
    // when mana is needed for triage. Arena: grounding owns the air slot.
    if (bot->GetPower(POWER_MANA) * 100 / std::max(1u, bot->GetMaxPower(POWER_MANA)) > 25)
    {
        if (!bot->GetTotem(TOTEM_SLOT_AIR) &&
            Cast(ai, bot->InArena() ? "grounding totem" : "windfury totem", bot))
            return true;
        if (!bot->GetTotem(TOTEM_SLOT_EARTH) && Cast(ai, "strength of earth totem", bot))
            return true;
        if (!bot->GetTotem(TOTEM_SLOT_WATER) && Cast(ai, "mana spring totem", bot))
            return true;
    }

    // 3. earth shield stays on the master (tank preference comes later)
    if (Player* master = ai->GetMaster())
        if (master->IsInWorld() && master->GetMapId() == bot->GetMapId() &&
            !ai->HasAura("earth shield", master) && Cast(ai, "earth shield", master))
            return true;

    return false;
}

void PartyExecutor::CombatTick(PlayerbotAI* ai, Player* bot)
{
    // 0. break hard cc with the pvp medallion (action self-gates: only
    // fires while stunned/feared/charmed/confused and off cooldown)
    if (ai->DoSpecificAction("use pvp trinket", Event(), true))
        return;

    // arena: the shot-caller watches the fight and calls plays unprompted
    // (edge-triggered + rate-limited inside; snapshot only, never blocks)
    sShotCaller.ArenaTick(ai, bot);

    // mid-cast: let the cast land instead of walking through it
    if (bot->IsNonMeleeSpellCasted(false, true, true))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return;
    }

    // healers: triage owns the tick — never fall through to dps logic
    if (PlayerbotAI::IsHeal(bot))
    {
        bool acted = HealerTriageTick(ai, bot);
        ai->SetAIInternalUpdateDelay(acted ? AFTER_CAST_DELAY_MS : IDLE_DELAY_MS);
        return;
    }

    // 1. standing cc duty from the seam
    if (KeepCcApplied(ai, bot))
        return;

    // 2/3. reflexes + one target decision
    Unit* target = PickTarget(ai, bot);
    if (!target)
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return;
    }

    if (TryInterrupt(ai, bot, target))
        return;

    if (EngageTarget(ai, bot, target))
        return;

    if (MeleeGetBehind(ai, bot, target))
        return;

    // 5. class rotation
    bool acted = false;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: acted = PlayerbotAI::IsTank(bot) ? TankWarriorTick(ai, bot, target)
                                                            : ArmsWarriorTick(ai, bot, target); break;
        case CLASS_ROGUE:   acted = RogueTick(ai, bot, target); break;
        case CLASS_MAGE:    acted = MageTick(ai, bot, target); break;
        case CLASS_PALADIN: acted = PlayerbotAI::IsTank(bot) ? ProtPaladinTick(ai, bot, target)
                                                            : RetPaladinTick(ai, bot, target); break;
        case CLASS_WARLOCK: acted = WarlockTick(ai, bot, target); break;
        default:            acted = false; break;
    }

    if (!acted)
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
}
