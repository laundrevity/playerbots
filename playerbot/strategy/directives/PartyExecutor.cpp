#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/PartyExecutor.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/directives/DirectiveMgr.h"
#include "playerbot/strategy/directives/DirectiveValues.h"
#include "playerbot/strategy/directives/ShotCaller.h"

#include "playerbot/thirdparty/nlohmann/json.hpp"

#include "BattleGround/BattleGround.h"
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
    bool PartyReadyToPull(Group* group, bool fast = false)
    {
        // fast = chain-pull pacing: only genuinely dangerous states wait
        uint32 hpWait = fast ? 4 : 6;      // tenths: below 40%/60% hp
        uint32 manaWait = fast ? 3 : 5;    // tenths: below 30%/50% mana
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->IsInWorld())
                continue;
            if (member->IsInCombat())
                return false;
            if (member->GetHealth() * 10 < member->GetMaxHealth() * hpWait)
                return false;
            if (member->GetPowerType() == POWER_MANA &&
                member->GetPower(POWER_MANA) * 10 < member->GetMaxPower(POWER_MANA) * manaWait)
                return false;   // let the mana users drink
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

    // mage conjured stocks, best rank first (1.12 items)
    const uint32 CONJURED_WATER_IDS[] = { 8079, 8078, 8077, 3772, 2136, 2288, 5350 };
    const uint32 CONJURED_FOOD_IDS[] = { 22895, 8076, 8075, 1487, 1114, 1113, 5349 };
    const uint32 MANA_GEM_IDS[] = { 5512, 5509, 5513, 5514 };   // ruby > citrine > jade > agate
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

    // healer by TALENTS, not by the old engine's strategy list: arena
    // strategy swaps strip the heal strategy and IsHeal() then misroutes
    // resto bots into dps ticks (observed: resto druid statue in tree form)
    bool IsHealerSpec(Player* bot)
    {
        int tab = AiFactory::GetPlayerSpecTab(bot);
        switch (bot->getClass())
        {
            case CLASS_PRIEST:  return tab == 0 || tab == 1;   // disc / holy
            case CLASS_PALADIN: return tab == 0;               // holy
            case CLASS_SHAMAN:  return tab == 2;               // resto
            case CLASS_DRUID:   return tab == 2;               // resto
        }
        return false;
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
            if (!IsHealerSpec((Player*)candidate))
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

    // breakable incapacitates: hitting these UNDOES the team's crowd control
    // (sap/blind/gouge/polymorph break on any damage; fear breaks fast).
    // Deliberate stuns (cheap shot, kidney) are NOT in this list — a stunned
    // kill target is exactly what you keep hitting.
    bool IsSoftCrowdControlled(PlayerbotAI* ai, Unit* unit)
    {
        return ai->HasAura("sap", unit) || ai->HasAura("blind", unit) ||
               ai->HasAura("gouge", unit) || ai->HasAura("polymorph", unit) ||
               unit->HasAuraType(SPELL_AURA_MOD_FEAR);
    }

    // ---- diminishing returns ledger (arena, observation-based) ----
    // Same-category CC runs full -> 1/2 -> 1/4 -> immune, resetting ~15s
    // after the last aura of the category fades. The module can't hook aura
    // events, so combat ticks OBSERVE each enemy once per second: a rising
    // edge counts an application, a falling edge stamps the fade.
    enum DrCategory { DR_STUN = 0, DR_INCAP, DR_FEAR, DR_BLIND, DR_POLY, DR_CATEGORY_COUNT };

    static const char* const DR_AURAS_STUN[]  = { "cheap shot", "kidney shot", "hammer of justice", "intercept", "concussion blow", nullptr };
    static const char* const DR_AURAS_INCAP[] = { "sap", "gouge", nullptr };
    static const char* const DR_AURAS_FEAR[]  = { "fear", "psychic scream", "howl of terror", "intimidating shout", nullptr };
    static const char* const DR_AURAS_BLIND[] = { "blind", nullptr };
    static const char* const DR_AURAS_POLY[]  = { "polymorph", nullptr };
    static const char* const* const DR_AURAS[DR_CATEGORY_COUNT] =
        { DR_AURAS_STUN, DR_AURAS_INCAP, DR_AURAS_FEAR, DR_AURAS_BLIND, DR_AURAS_POLY };

    struct DrVictimState
    {
        uint8 count[DR_CATEGORY_COUNT] = {};
        bool up[DR_CATEGORY_COUNT] = {};
        time_t fade[DR_CATEGORY_COUNT] = {};
        time_t observed = 0;
    };
    std::map<ObjectGuid, DrVictimState> s_drStates;

    void DrObserve(PlayerbotAI* ai, Unit* victim)
    {
        time_t now = time(nullptr);
        DrVictimState& st = s_drStates[victim->GetObjectGuid()];
        if (st.observed == now)
            return;                     // one observer per victim per second
        st.observed = now;
        for (int c = 0; c < DR_CATEGORY_COUNT; ++c)
        {
            bool up = false;
            for (const char* const* name = DR_AURAS[c]; *name; ++name)
                if (ai->HasAura(*name, victim))
                {
                    up = true;
                    break;
                }
            if (up && !st.up[c] && st.count[c] < 3)
                ++st.count[c];
            else if (!up && st.up[c])
                st.fade[c] = now;
            st.up[c] = up;
            if (!up && st.count[c] && now > st.fade[c] + 16)
                st.count[c] = 0;        // bracket reset after the fade window
        }
        if (s_drStates.size() > 128)    // hygiene: forget long-gone victims
            for (auto itr = s_drStates.begin(); itr != s_drStates.end();)
                if (itr->second.observed + 300 < now)
                    itr = s_drStates.erase(itr);
                else
                    ++itr;
    }

    uint8 DrLevel(Unit* victim, DrCategory cat)
    {
        auto itr = s_drStates.find(victim->GetObjectGuid());
        if (itr == s_drStates.end())
            return 0;
        DrVictimState& st = itr->second;
        if (!st.up[cat] && st.count[cat] && time(nullptr) > st.fade[cat] + 16)
            st.count[cat] = 0;
        return st.count[cat];
    }

    // ---- arena director: one shared kill target per team per fight ----
    // Per-bot nearest-target drift is why the team looks headless: everyone
    // picks his own victim. The director keeps a single plan per (arena
    // instance, side): squishiest dps first, healers only when nothing else
    // stands, and a transient off-target while the main sits in our own CC.
    int ArenaKillPriority(Player* enemy)
    {
        if (PlayerbotAI::IsHeal(enemy))
            return 100;
        switch (enemy->getClass())
        {
            case CLASS_MAGE:    return 0;
            case CLASS_WARLOCK: return 1;
            case CLASS_HUNTER:  return 2;
            case CLASS_PRIEST:  return 3;
            case CLASS_ROGUE:   return 4;
            case CLASS_DRUID:   return 5;
            case CLASS_SHAMAN:  return 6;
            case CLASS_PALADIN: return 7;
            case CLASS_WARRIOR: return 8;
        }
        return 50;
    }

    struct ArenaPlan
    {
        ObjectGuid killTarget;
        time_t lastSeen = 0;
    };
    std::map<uint64, ArenaPlan> s_arenaPlans;

    Unit* ArenaDirectorTarget(PlayerbotAI* ai, Player* bot)
    {
#ifdef MANGOSBOT_ZERO
        return nullptr;     // vanilla has no arenas
#else
        BattleGround* bg = bot->GetBattleGround();
        if (!bg || !bg->IsArena())
            return nullptr;
        Team side = bg->GetPlayerTeam(bot->GetObjectGuid());
        if (side == TEAM_NONE)
            return nullptr;
        time_t now = time(nullptr);
        uint64 key = (uint64(bg->GetInstanceId()) << 1) | (side == HORDE ? 1 : 0);
        ArenaPlan& plan = s_arenaPlans[key];
        plan.lastSeen = now;
        if (s_arenaPlans.size() > 32)
            for (auto itr = s_arenaPlans.begin(); itr != s_arenaPlans.end();)
                if (itr->second.lastSeen + 600 < now)
                    itr = s_arenaPlans.erase(itr);
                else
                    ++itr;

        Unit* current = plan.killTarget ? ai->GetUnit(plan.killTarget) : nullptr;
        if (current && (!current->IsAlive() || !current->IsInWorld()))
            current = nullptr;

        if (!current)
        {
            AiObjectContext* context = ai->GetAiObjectContext();
            std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
            int best = 1000;
            for (const ObjectGuid& guid : possible)
            {
                Unit* candidate = ai->GetUnit(guid);
                if (!candidate || !candidate->IsPlayer() || !candidate->IsAlive())
                    continue;
                int priority = ArenaKillPriority((Player*)candidate);
                if (priority < best)
                {
                    best = priority;
                    current = candidate;
                }
            }
            plan.killTarget = current ? current->GetObjectGuid() : ObjectGuid();
        }

        // main target parked in our own breakable CC: swap transiently to
        // the best free enemy; the plan snaps back when the CC fades
        if (current && IsSoftCrowdControlled(ai, current))
        {
            AiObjectContext* context = ai->GetAiObjectContext();
            std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
            Unit* fallback = nullptr;
            int best = 1000;
            for (const ObjectGuid& guid : possible)
            {
                Unit* candidate = ai->GetUnit(guid);
                if (!candidate || !candidate->IsPlayer() || !candidate->IsAlive() ||
                    candidate == current || IsSoftCrowdControlled(ai, candidate))
                    continue;
                int priority = ArenaKillPriority((Player*)candidate);
                if (priority < best)
                {
                    best = priority;
                    fallback = candidate;
                }
            }
            if (fallback)
                return fallback;
        }
        return current;
#endif
    }

    // burst window: the kill target is stunned in range, or the enemy healer
    // is locked out — the seconds cooldowns exist for
    bool ArenaBurstWindow(PlayerbotAI* ai, Player* bot, Unit* target)
    {
        if (!target || !bot->InArena())
            return false;
        if (target->HasAuraType(SPELL_AURA_MOD_STUN) && target->GetHealthPercent() < 75.0f)
            return true;
        if (Unit* healer = NearestEnemyHealer(ai, bot))
            if (healer != target &&
                (healer->HasAuraType(SPELL_AURA_MOD_STUN) || IsSoftCrowdControlled(ai, healer)))
                return true;
        return false;
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
    {
        // visibility: a missing/unreal master silently demotes a grouped bot
        // to the legacy engine (observed: classic dungeon party idling with
        // zero executor probes — nobody knew the executor wasn't running)
        static std::map<uint32, uint32> lastLog;
        uint32 now = WorldTimer::getMSTime();
        uint32& last = lastLog[bot->GetObjectGuid().GetCounter()];
        if (bot->GetGroup() && (!last || now - last > 60000))
        {
            last = now;
            Player* m = ai->GetMaster();
            sLog.outBasic("Executor: NOT owning grouped bot %s (master=%s, masterHasAI=%d)",
                          bot->GetName(), m ? m->GetName() : "<none>",
                          (m && m->GetPlayerbotAI()) ? 1 : 0);
        }
        return false;
    }
    if (bot->InBattleGround())
        return false;   // regular bgs keep the old brain
    if (!bot->IsAlive())
        return false;   // the module's dead-state engine handles release/rez
    return true;
}

void PartyExecutor::Tick(PlayerbotAI* ai, Player* bot)
{
    if (bot->IsInCombat())
    {
        CombatTick(ai, bot);
        return;
    }

    // arena: a healer whose team is bleeding is IN the fight whether or not
    // the combat flag agrees — triage lived only in CombatTick, and a healer
    // nobody had hit yet never entered combat, so she jogged formation
    // points while her master died (observed: resto shaman, 30 log events
    // in a whole 2v2, zero heals, zero totems)
    bool oocTriage = false;
    if (bot->InArena())
        oocTriage = bot->GetBattleGround() &&
                    bot->GetBattleGround()->GetStatus() == STATUS_IN_PROGRESS;
    else if (ai->HasRealPlayerMaster() && bot->GetGroup())
    {
        // dungeon/world groups: heal and DISPEL between pulls too (Stratholme
        // plagues sat uncured until the next fight started) — but never at the
        // cost of drinking: low mana means sit down, not top-off
        uint32 maxMana = bot->GetMaxPower(POWER_MANA);
        oocTriage = !maxMana || bot->GetPower(POWER_MANA) * 100 / maxMana > 30;
    }
    if (oocTriage && IsHealerSpec(bot) && HealerTriageTick(ai, bot))
    {
        ai->SetAIInternalUpdateDelay(AFTER_CAST_DELAY_MS);
        return;
    }

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
                // directive-seam override ("kings plz" through the shot-caller):
                // outranks the default pick, and a present-but-DIFFERENT blessing
                // is a recast, not a skip
                std::string forced = GetBlessingOverride(
                    ai->GetAiObjectContext()->GetValue<std::string>("blessing overrides")->Get(),
                    member->GetName());
                if (!forced.empty())
                {
                    std::string want = "blessing of " + forced;
                    if (!ai->HasAura(want, member) && !ai->HasAura("greater " + want, member))
                        if (Cast(ai, want.c_str(), member))
                            return true;
                    break;
                }
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
    AiObjectContext* context = ai->GetAiObjectContext();
    std::string pullPolicy = context->GetValue<std::string>("pull policy")->Get();
    if (HoldPolicy(ai) || pullPolicy == "hold")
        return false;   // "stop pulling" in party chat parks the tank (sticky)
    bool fast = pullPolicy == "fast";

    Player* master = ai->GetMaster();
    Group* group = bot->GetGroup();
    if (!master || !group || !master->IsInWorld() || master->GetMapId() != bot->GetMapId())
        return false;

    // healer-away watch: an explicit /afk parks pulls immediately; the human
    // going idle (no move, no turn, no combat for 2 min) parks them too.
    // Announce each transition once so the pause is legible from the party.
    {
        bool away = master->isAFK();
        uint32 nowMs = WorldTimer::getMSTime();
        if (!away)
        {
            static std::map<uint32, std::tuple<float, float, float, uint32>> pose;
            auto& p = pose[master->GetObjectGuid().GetCounter()];
            if (std::fabs(std::get<0>(p) - master->GetPositionX()) > 0.5f ||
                std::fabs(std::get<1>(p) - master->GetPositionY()) > 0.5f ||
                std::fabs(std::get<2>(p) - master->GetOrientation()) > 0.05f ||
                master->IsInCombat())
                p = {master->GetPositionX(), master->GetPositionY(), master->GetOrientation(), nowMs};
            else if (nowMs - std::get<3>(p) > 120000)
                away = true;
        }
        static std::map<uint32, bool> saidAway;
        bool& said = saidAway[bot->GetObjectGuid().GetCounter()];
        if (away)
        {
            if (!said)
            {
                said = true;
                ai->TellPlayerNoFacing(master, "healer's away — holding pulls",
                                       PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            }
            return false;
        }
        if (said)
        {
            said = false;
            ai->TellPlayerNoFacing(master, "healer's back — pulling on",
                                   PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        }
    }

    // wait for the human: never advance further than ~35y from them
    if (sServerFacade.GetDistance2d(bot, master) > 35.0f)
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;    // stand ground, don't run back either
    }

    // human-tank pacing checklist (fast = chain-pull thresholds)
    if (!PartyReadyToPull(group, fast))
        return false;

    // next pack: nearest hostile in the master's heading cone. Fast mode
    // ignores the cone (nearest pack, any direction); normal mode also takes
    // anything close to the TANK himself — a pack on the path shouldn't wait
    // for the healer's camera.
    float heading = master->GetOrientation();
    float hx = std::cos(heading), hy = std::sin(heading);
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
        if (!fast && (dx * hx + dy * hy) / distance < 0.25f)   // cos(~75°)
        {
            if (candidate->GetDistance(bot) > 20.0f)
                continue;   // off-route and not near the tank either
        }
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
    // never trample an in-progress cast OR channel: conjures take 3s,
    // summons 10s, evocation channels 8s — the fall-through to FollowLeader
    // was cancelling them with movement every tick
    if (bot->IsNonMeleeSpellCasted(false, false, true))
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

    // mage upkeep: a human mage keeps conjured stocks topped up and a mana
    // gem banked. Cast-by-name resolves the highest known rank, so training
    // max-rank conjures automatically upgrades the stock.
    if (bot->getClass() == CLASS_MAGE)
    {
        uint32 water = 0, food = 0;
        for (uint32 id : CONJURED_WATER_IDS)
            water += bot->GetItemCount(id);
        for (uint32 id : CONJURED_FOOD_IDS)
            food += bot->GetItemCount(id);
        if (water < 10 && Cast(ai, "conjure water", bot))
            return;
        if (food < 5 && Cast(ai, "conjure food", bot))
            return;
        if (!FindBagItem(bot, MANA_GEM_IDS, sizeof(MANA_GEM_IDS) / sizeof(uint32)) &&
            (Cast(ai, "conjure mana ruby", bot) || Cast(ai, "conjure mana citrine", bot) ||
             Cast(ai, "conjure mana jade", bot) || Cast(ai, "conjure mana agate", bot)))
            return;
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

        // arena, stealthed, out of combat (gates or a vanish reset): the
        // full rogue opener. Sap first — the enemy healer, or in double dps
        // whoever we are NOT opening on (TBC sap breaks neither stealth nor
        // starts combat) — then creep to the kill target and cheap shot.
        // Never auto-attack out of stealth. GATES MUST BE OPEN: server-side
        // movement ignores door collision, so a preparation-phase creep
        // walks the rogue straight through the closed gate.
        if (bot->InArena() && ai->HasAura("stealth", bot) &&
            bot->GetBattleGround() && bot->GetBattleGround()->GetStatus() == STATUS_IN_PROGRESS)
        {
            AiObjectContext* stealthContext = ai->GetAiObjectContext();
            std::list<ObjectGuid> possible = stealthContext->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
            std::vector<Unit*> enemies;
            for (const ObjectGuid& guid : possible)
            {
                Unit* candidate = ai->GetUnit(guid);
                if (candidate && candidate->IsPlayer() && candidate->IsAlive() &&
                    sServerFacade.GetDistance2d(bot, candidate) < 60.0f)
                    enemies.push_back(candidate);
            }

            // no enemy in sight yet: take the scout position — ahead of the
            // master along his facing — so the sap approach starts from the
            // front line instead of from behind his shoulder. By the time
            // the healer is visible it's usually too late to outrun the pull.
            if (enemies.empty())
            {
                Player* master = ai->GetMaster();
                if (master && master->IsAlive() && master->GetMapId() == bot->GetMapId())
                {
                    float const lead = 22.0f;
                    float x = master->GetPositionX() + std::cos(master->GetOrientation()) * lead;
                    float y = master->GetPositionY() + std::sin(master->GetOrientation()) * lead;
                    if (bot->GetDistance2d(x, y) > 5.0f)
                        bot->GetMotionMaster()->MovePoint(0, x, y, master->GetPositionZ(), FORCED_MOVEMENT_RUN);
                    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
                    return;
                }
            }

            // the skull mark names the kill target; sap goes on someone else
            Unit* marked = nullptr;
            if (Group* group = bot->GetGroup())
                if (Unit* skull = ai->GetUnit(ObjectGuid(group->GetTargetIcon(7))))
                    if (skull->IsAlive())
                        marked = skull;

            // sap target: never worth stalking a lone survivor — but a lone
            // VISIBLE healer with unseen (stealthed) teammates still exists
            // per the bracket size, and sapping him is the mirror-matchup play
            uint32 bracketSize = 0;
#ifndef MANGOSBOT_ZERO
            if (BattleGround* bg = bot->GetBattleGround())
                bracketSize = uint32(bg->GetArenaType());
#endif
            bool knowsSap = bot->HasSpell(6770) || bot->HasSpell(2070) || bot->HasSpell(11297);
            Unit* sapTarget = nullptr;
            if (knowsSap && (enemies.size() >= 2 ||
                             (!enemies.empty() && bracketSize > enemies.size())))
            {
                Unit* healer = NearestEnemyHealer(ai, bot);
                if (healer && healer != marked)
                    sapTarget = healer;
                else if (!healer && enemies.size() >= 2)
                {
                    // double dps: sap the one the team reaches last
                    float farthest = 0.0f;
                    for (Unit* enemy : enemies)
                    {
                        if (enemy == marked)
                            continue;
                        float distance = sServerFacade.GetDistance2d(bot, enemy);
                        if (distance > farthest)
                        {
                            farthest = distance;
                            sapTarget = enemy;
                        }
                    }
                }
            }

            // opener diagnostics: one line every ~2s while stealthed in arena
            {
                static std::map<ObjectGuid, time_t> lastDiag;
                time_t nowDiag = time(nullptr);
                time_t& stamp = lastDiag[bot->GetObjectGuid()];
                if (nowDiag >= stamp + 2)
                {
                    stamp = nowDiag;
                    Unit* healerDiag = NearestEnemyHealer(ai, bot);
                    sLog.outBasic("RogueOpener: %s enemies=%zu knowsSap=%d healer=%s marked=%s sapTarget=%s%s openFallbackReady=%d",
                                  bot->GetName(), enemies.size(), knowsSap ? 1 : 0,
                                  healerDiag ? healerDiag->GetName() : "-",
                                  marked ? marked->GetName() : "-",
                                  sapTarget ? sapTarget->GetName() : "-",
                                  (sapTarget && sapTarget->IsInCombat()) ? "(in combat!)" : "",
                                  bot->IsInCombat() ? 0 : 1);
                }
            }

            // phase 1: deliver the sap (done once it lands or combat finds him)
            if (sapTarget && !ai->HasAura("sap", sapTarget) && !sapTarget->IsInCombat())
            {
                if (bot->CanReachWithMeleeAttack(sapTarget))
                {
                    if (Cast(ai, "sap", sapTarget))
                        return;
                }
                else
                {
                    // sprint-sap: stealth walk is 30% slow and the window
                    // closes when the teams collide — burn sprint to get
                    // there first (TBC sprint neither breaks stealth nor
                    // starts combat)
                    if (sServerFacade.GetDistance2d(bot, sapTarget) > 15.0f)
                        Cast(ai, "sprint", bot);
                    bot->GetMotionMaster()->MovePoint(0, sapTarget->GetPositionX(), sapTarget->GetPositionY(),
                                                      sapTarget->GetPositionZ(), FORCED_MOVEMENT_RUN);
                    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
                    return;
                }
            }

            // phase 2: creep to the kill target and open
            Unit* openTarget = marked;
            if (!openTarget)
            {
                float best = 60.0f;
                for (Unit* enemy : enemies)
                {
                    if (enemy == sapTarget || IsSoftCrowdControlled(ai, enemy))
                        continue;
                    float distance = sServerFacade.GetDistance2d(bot, enemy);
                    if (distance < best)
                    {
                        best = distance;
                        openTarget = enemy;
                    }
                }
            }
            if (openTarget)
            {
                if (bot->CanReachWithMeleeAttack(openTarget))
                {
                    if (Cast(ai, "cheap shot", openTarget))
                        return;
                }
                else
                {
                    bot->GetMotionMaster()->MovePoint(0, openTarget->GetPositionX(), openTarget->GetPositionY(),
                                                      openTarget->GetPositionZ(), FORCED_MOVEMENT_RUN);
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
        if (IsHealerSpec(bot))
            if (Group* arenaGroup = bot->GetGroup())
            {
                Player* escort = nullptr;
                float escortDistance = 10000.0f;
                for (GroupReference* itr = arenaGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->getSource();
                    if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
                        IsHealerSpec(member))
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

// elemental shaman: flame shock rolls, lava burst on cooldown (guaranteed
// crit on a shocked target), lightning bolt filler. Non-healing shamans of
// any spec run this — a wrong-spec bot beats a statue.
bool PartyExecutor::EleShamanTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ThreatCapped(ai, bot, target))
    {
        DpsIdleProbe(ai, bot, "threat-capped");
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;
    }

    if ((BurnPolicy(ai) || ArenaBurstWindow(ai, bot, target)) && Cast(ai, "elemental mastery", bot))
        return true;

    if (!ai->HasAura("flame shock", target, false, true) && Cast(ai, "flame shock", target))
        return true;
    if (Cast(ai, "lava burst", target))
        return true;
    if (Cast(ai, "lightning bolt", target))
        return true;
    return false;
}

// balance druid: dots up, wrath filler. Also the safety net for any
// non-healing druid spec so nobody idles in the dispatch void again.
bool PartyExecutor::BalanceDruidTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ThreatCapped(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;
    }

    if (!ai->HasAura("moonkin form", bot) && Cast(ai, "moonkin form", bot))
        return true;
    if (!ai->HasAura("moonfire", target, false, true) && Cast(ai, "moonfire", target))
        return true;
    if (!ai->HasAura("insect swarm", target, false, true) && Cast(ai, "insect swarm", target))
        return true;
    if (Cast(ai, "starfire", target))
        return true;
    if (Cast(ai, "wrath", target))
        return true;
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
#ifdef MANGOSBOT_TWO
        case CLASS_SHAMAN:  return Cast(ai, "wind shear", target);   // earth shock stopped interrupting in 3.0
#else
        case CLASS_SHAMAN:  return Cast(ai, "earth shock", target);
#endif
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

    // 3b'. arena: the director's shared kill target — the whole side plays
    // one plan instead of per-bot nearest-enemy drift
    if (bot->InArena())
        if (Unit* called = ArenaDirectorTarget(ai, bot))
            return called;

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
        // healer rescue outranks the assist train: a mob eating the master
        // or a healer dies FIRST. Taunt peels one mob per 10s and the threat
        // governor doesn't cap loose mobs — dps switching is the real answer
        // ("i keep aggro for entire pulls just from healing").
        {
            std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("attackers")->Get();
            Player* master = ai->GetMaster();
            Unit* menace = nullptr;
            float best = 1000.0f;
            for (const ObjectGuid& guid : attackers)
            {
                Unit* mob = ai->GetUnit(guid);
                if (!mob || sServerFacade.UnitIsDead(mob) || IsSoftCrowdControlled(ai, mob))
                    continue;
                Unit* victim = mob->GetVictim();
                if (!victim || !victim->IsPlayer())
                    continue;
                bool protectee = (master && victim == master) ||
                                 PlayerbotAI::IsHeal((Player*)victim);
                if (!protectee)
                    continue;
                float distance = sServerFacade.GetDistance2d(bot, mob);
                if (distance < best)
                {
                    best = distance;
                    menace = mob;
                }
            }
            if (menace)
                return menace;
        }

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
    if (!nearest)
    {
        // between-kill gap ("no-target" probes): assist the human — the
        // master's living target is always a valid pick. ("possible targets"
        // proved unreliable here: it never populated all night.)
        if (Player* master = ai->GetMaster())
            if (master != bot && master->IsAlive())
                if (Unit* mtarget = master->GetVictim())
                    if (!sServerFacade.UnitIsDead(mtarget) && !mtarget->IsPlayer() &&
                        bot->IsWithinDistInMap(mtarget, sPlayerbotAIConfig.sightDistance))
                        nearest = mtarget;
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

    // arena: nobody engaged yet — the nearest enemy player IS the fight.
    // Skip soft-CC'd enemies (sap/blind/poly/fear): hitting them undoes the
    // team's control. Only when EVERYONE is under CC does the nearest one
    // become fair game (someone has to break the stalemate).
    if (bot->InArena())
    {
        std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
        Unit* nearestControlled = nullptr;
        float bestControlled = 1000.0f;
        for (const ObjectGuid& guid : possible)
        {
            Unit* candidate = ai->GetUnit(guid);
            if (!candidate || !candidate->IsPlayer() || sServerFacade.UnitIsDead(candidate))
                continue;
            float distance = sServerFacade.GetDistance2d(bot, candidate);
            if (IsSoftCrowdControlled(ai, candidate))
            {
                if (distance < bestControlled)
                {
                    bestControlled = distance;
                    nearestControlled = candidate;
                }
                continue;
            }
            if (distance < best)
            {
                best = distance;
                nearest = candidate;
            }
        }
        if (!nearest)
            nearest = nearestControlled;
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
    if (ranged)
    {
        // casters cast from max range: chasing to a tight leash keeps the
        // bot permanently moving, and a moving caster can only use instants
        // (observed: arena mage reduced to fire blast + wand for the whole
        // match). 33yd covers frostbolt/fireball with talent reach — but
        // shamans live at shock range (25yd), so they plant closer or the
        // flame shock/lava burst half of the kit never fires.
        float leash = bot->getClass() == CLASS_SHAMAN ? 24.0f : 33.0f;
        if (!bot->IsWithinDistInMap(target, leash) || !bot->IsWithinLOSInMap(target))
            return ai->DoSpecificAction("reach spell", Event(), true);
        // in range with line of sight: retire the leftover chase generator
        // ONCE (a bare StopMoving fights the still-active chase and reads
        // as running in place), then let the rotation cast from here
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
        {
            bot->GetMotionMaster()->Clear(false);
            ai->StopMoving();
        }
        return false;
    }
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
// cheap visibility for "the dps play badly": mastered bots that end a combat
// tick without acting log WHY, throttled per bot. Vibes say slow; this says
// which gate (no-target / threat-capped / rotation-idle) and lets the harness
// attribute idle time to causes instead of guesses.
void PartyExecutor::DpsIdleProbe(PlayerbotAI* ai, Player* bot, const char* why)
{
    if (!ai->HasRealPlayerMaster() || !bot->IsInCombat())
        return;
    static std::map<uint32, uint32> lastLog;
    uint32 now = WorldTimer::getMSTime();
    uint32& last = lastLog[bot->GetObjectGuid().GetCounter()];
    if (last && now - last < 10000)
        return;
    last = now;
    sLog.outBasic("ExecutorIdle: %s %s", bot->GetName(), why);
}

bool PartyExecutor::ThreatCapped(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (PlayerbotAI::IsTank(bot))
        return false;
    if (target->GetHealthPercent() < 20.0f)
        return false;   // execute window: it dies before threat matters
    Unit* tank = target->GetVictim();
    if (!tank || tank == bot || !tank->IsPlayer() || !PlayerbotAI::IsTank((Player*)tank))
        return false;   // nobody tanking it: no ceiling to respect
    float mine = target->getThreatManager().getThreat(bot);
    float tanks = target->getThreatManager().getThreat(tank);
    // classic aggro rule: mobs turn at 110% of current-target threat for
    // melee, 130% for ranged — the ranged budget is much bigger than the
    // melee one, and 0.9 for everyone idled well-geared casters (observed:
    // Naxx-BiS mages parked after two frostbolts behind a green-geared tank)
    float ceiling = ai->IsRanged(bot) ? 1.2f : THREAT_CEILING;
    return tanks > 0.0f && mine > ceiling * tanks;
}

// melee dps belong behind the target (when someone else is tanking it)
bool PartyExecutor::MeleeGetBehind(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ai->IsRanged(bot) || PlayerbotAI::IsTank(bot))
        return false;
    if (target->IsPlayer())
        return false;   // never chase a moving player's back: the reposition
                        // loop starves the whole rotation (arena regression)
    if (target->IsMoving())
        return false;   // gathering tanks drag packs around now — chasing a
                        // moving mob's back is pure thrash (17x reposition
                        // probes in one run); wait for it to settle
    if (target->GetVictim() == bot)
        return false;
    if (!target->HasInArc(bot, M_PI_F))
        return false;   // already behind
    return ai->DoSpecificAction("set behind", Event(), true);
}

// ---- class rotations (researched for 2.4.3; sources in the repo docs) ----

bool PartyExecutor::TankWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    // kill-target marking: when the group has no live skull, the tank puts
    // one on his target — humans read marks, and the bots' rti targeting
    // (TankTargetValue checks rti first) converges the party on it
    if (Group* group = bot->GetGroup())
    {
        if (target->IsAlive() && !target->IsPlayer())
        {
            ObjectGuid skullGuid = group->GetTargetIcon(7);
            Unit* skullUnit = skullGuid ? ai->GetUnit(skullGuid) : nullptr;
            if (!skullUnit || !skullUnit->IsAlive() || !skullUnit->IsInCombat())
                if (skullGuid != target->GetObjectGuid())
                    group->SetTargetIcon(7, target->GetObjectGuid());
        }
    }

    if (!ai->HasAura("defensive stance", bot) && Cast(ai, "defensive stance", bot))
        return true;

    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    uint32 meleeCount = 0;
    for (const ObjectGuid& guid : attackers)
        if (Unit* attacker = ai->GetUnit(guid))
            if (bot->CanReachWithMeleeAttack(attacker))
                ++meleeCount;

    // the HUMAN being hit is the emergency: taunt that mob off them before
    // any other priority ("tank ignores me pulling aggro")
    if (Player* master = ai->GetMaster())
        if (master != bot && master->IsAlive())
            for (const ObjectGuid& guid : attackers)
                if (Unit* menace = ai->GetUnit(guid))
                    if (menace->IsAlive() && menace->GetVictim() == master &&
                        Cast(ai, "taunt", menace))
                        return true;

    if (target->GetVictim() && target->GetVictim() != bot && Cast(ai, "taunt", target))
        return true;

    // swarm on the backline: taunt handles ONE mob and only in range — when
    // several mobs are on the master/healer standing at range, a human tank
    // RUNS to them and unloads there. Scholo wipe forensics: 5 mobs beat the
    // healer to death while the tank stood in his own pack, challenging
    // shout's 10y radius never containing any of them.
    {
        Player* master = ai->GetMaster();
        uint32 swarm = 0;
        Unit* anchor = nullptr;
        for (const ObjectGuid& guid : attackers)
            if (Unit* mob = ai->GetUnit(guid))
                if (mob->IsAlive() && mob->GetVictim() && mob->GetVictim() != bot &&
                    mob->GetVictim()->IsPlayer() && !IsSoftCrowdControlled(ai, mob))
                {
                    Player* victim = (Player*)mob->GetVictim();
                    if ((master && victim == master) || PlayerbotAI::IsHeal(victim))
                    {
                        ++swarm;
                        if (!anchor)
                            anchor = victim;
                    }
                }
        if (swarm >= 2 && anchor)
        {
            if (sServerFacade.GetDistance2d(bot, anchor) > 8.0f)
            {
                static std::map<uint32, uint32> lastRunMs;
                uint32 nowMs = WorldTimer::getMSTime();
                uint32& lastRun = lastRunMs[bot->GetObjectGuid().GetCounter()];
                if (!lastRun || nowMs - lastRun > 3000)
                {
                    lastRun = nowMs;
                    sLog.outBasic("TankRescue: %s running to %s (%u mobs on the backline)",
                                  bot->GetName(), anchor->GetName(), swarm);
                    bot->GetMotionMaster()->MovePoint(0, anchor->GetPositionX(),
                        anchor->GetPositionY(), anchor->GetPositionZ(), FORCED_MOVEMENT_RUN);
                }
                return true;
            }
            if (Cast(ai, "challenging shout", bot))
            {
                sLog.outBasic("TankRescue: %s challenging shout at %s (%u mobs)",
                              bot->GetName(), anchor->GetName(), swarm);
                return true;
            }
        }
    }

    // big-pull rescue: taunt peels one mob per 10s — when a pile is beating
    // on the party (healing aggro on mobs the tank never touched),
    // Challenging Shout fixates everything within 10y for 6s and buys the
    // sunder ledger time. 10-min cd = a genuine emergency button.
    {
        uint32 loose = 0;
        for (const ObjectGuid& guid : attackers)
            if (Unit* mob = ai->GetUnit(guid))
                if (mob->IsAlive() && mob->GetVictim() && mob->GetVictim() != bot &&
                    mob->GetVictim()->IsPlayer() &&
                    sServerFacade.GetDistance2d(bot, mob) < 10.0f &&
                    !IsSoftCrowdControlled(ai, mob))
                    ++loose;
        if (loose >= 3 && Cast(ai, "challenging shout", bot))
        {
            sLog.outBasic("TankRescue: %s challenging shout (%u loose mobs)", bot->GetName(), loose);
            return true;
        }
    }

    if (TankFaceAway(ai, bot, target))
        return true;

    // shield block is survivability, not an action: fire it and fall through
    // so the tick still generates threat (it burned 47 whole ticks in one
    // Stratholme run as a returning branch)
    if (target->GetVictim() == bot)
        Cast(ai, "shield block", bot);

    // pack gathering: casters won't walk to the tank, so the tank walks TO
    // the caster, dragging his melee train into one AoE-able clump.
    // Probed: was never observed firing in two full runs.
    {
        static std::map<uint32, uint32> lastGather;
        uint32 nowMs = WorldTimer::getMSTime();
        uint32& last = lastGather[bot->GetObjectGuid().GetCounter()];
        if ((!last || nowMs - last > 6000) && meleeCount >= 1)
        {
            Unit* pick = nullptr;
            uint32 candidates = 0;
            for (const ObjectGuid& guid : attackers)
                if (Unit* caster = ai->GetUnit(guid))
                    // any STATIONARY ranged attacker anchors the pack — the
                    // mana-only test rejected Strat's ranged skeletons
                    // (probes: 24x no-candidate at attackers > melee)
                    if (caster->IsAlive() && !caster->IsPlayer() &&
                        !bot->CanReachWithMeleeAttack(caster) &&
                        (caster->GetPowerType() == POWER_MANA || !caster->IsMoving()) &&
                        sServerFacade.GetDistance2d(bot, caster) < 30.0f &&
                        !IsSoftCrowdControlled(ai, caster))
                    {
                        ++candidates;
                        if (!pick)
                            pick = caster;
                    }
            if (pick)
            {
                last = nowMs;
                sLog.outBasic("TankGather: %s -> %s (dist %.0f, melee train %u)",
                              bot->GetName(), pick->GetName(),
                              sServerFacade.GetDistance2d(bot, pick), meleeCount);
                bot->GetMotionMaster()->MovePoint(0, pick->GetPositionX(),
                    pick->GetPositionY(), pick->GetPositionZ(), FORCED_MOVEMENT_RUN);
                return true;
            }
            if (bot->IsInCombat())
            {
                last = nowMs;   // reuse throttle for the negative report
                sLog.outBasic("TankGather: %s no caster candidate (melee=%u attackers=%u)",
                              bot->GetName(), meleeCount, uint32(attackers.size()));
            }
        }
    }

    // vanilla AoE threat is TAB-SUNDER: a loose melee mob gets a sunder
    // before anything else (beats taunt-spam, starts its threat ledger)
    if (meleeCount >= 2)
        for (const ObjectGuid& guid : attackers)
            if (Unit* attacker = ai->GetUnit(guid))
                if (attacker != target && attacker->IsAlive() &&
                    attacker->GetVictim() != bot &&
                    bot->CanReachWithMeleeAttack(attacker) &&
                    !IsSoftCrowdControlled(ai, attacker) &&
                    Cast(ai, "sunder armor", attacker))
                    return true;

    // Shield Slam > Revenge > Devastate/Sunder, Demo + Battle Shout on packs,
    // Heroic Strike/Cleave as the true-excess rage dump (icy-veins classic
    // prot priority; HS replaces the next auto WHICH THEN YIELDS NO RAGE)
    if (Cast(ai, "shield slam", target))
        return true;
    if (Cast(ai, "revenge", target))
        return true;
#ifndef MANGOSBOT_ZERO
    // 1.12 thunder clap is battle-stance-only; TBC+ prot uses it tanking
    if (meleeCount >= 2 && Cast(ai, "thunder clap", target))
        return true;
#endif
    if (meleeCount >= 2 && Cast(ai, "demoralizing shout", target))
        return true;
    if (meleeCount >= 2 && !ai->HasAura("battle shout", bot) && Cast(ai, "battle shout", bot))
        return true;    // battle shout = pack-wide threat per cast in vanilla
    if (Cast(ai, "devastate", target))
        return true;
    if (Cast(ai, "sunder armor", target))
        return true;
    if (bot->GetPower(POWER_RAGE) > 400)
    {
        if (Cast(ai, meleeCount >= 2 ? "cleave" : "heroic strike", target))
            return true;
        // zero HS in every log so far: name the failure if the queue refuses
        DpsIdleProbe(ai, bot, "rage-dump-failed");
    }
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

    if (BurnPolicy(ai) || ArenaBurstWindow(ai, bot, target))
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
        // off-target; blind him when the kill target enters the window —
        // but never blind through existing control (stun/fear/sap = DR and
        // duration wasted)
        if (Unit* healer = NearestEnemyHealer(ai, bot))
        {
            if (healer != target)
            {
                if (healer->IsNonMeleeSpellCasted(false, true, true) &&
                    bot->CanReachWithMeleeAttack(healer) && Cast(ai, "kick", healer))
                    return true;
                if (target->GetHealthPercent() < 50.0f &&
                    !healer->HasAuraType(SPELL_AURA_MOD_STUN) &&
                    !healer->HasAuraType(SPELL_AURA_MOD_CONFUSE) &&
                    !IsSoftCrowdControlled(ai, healer) &&
                    DrLevel(healer, DR_BLIND) < 2 && Cast(ai, "blind", healer))
                    return true;
            }
        }

        // peel: an off-target enemy training my master eats a gouge — buys
        // a free cast/drink window. Gouge on the KILL target is a trap (our
        // own dots break it instantly), so only ever peel with it.
        if (Player* master = ai->GetMaster())
        {
            if (master->IsAlive() && master != bot)
            {
                AiObjectContext* peelContext = ai->GetAiObjectContext();
                std::list<ObjectGuid> possible = peelContext->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
                for (const ObjectGuid& guid : possible)
                {
                    Unit* menace = ai->GetUnit(guid);
                    if (!menace || !menace->IsPlayer() || !menace->IsAlive() || menace == target)
                        continue;
                    if (menace->GetVictim() != master)
                        continue;
                    if (menace->HasAuraType(SPELL_AURA_MOD_STUN) || IsSoftCrowdControlled(ai, menace))
                        continue;
                    if (bot->CanReachWithMeleeAttack(menace) && Cast(ai, "gouge", menace))
                        return true;
                }
            }
        }

        // still stealthed although combat already found the team: the sap
        // window stays open as long as the TARGET is out of combat (sap
        // needs a stealthed rogue, not a peaceful one). Land it on the
        // trailing healer before joining the fight — within reason (30yd).
        if (ai->HasAura("stealth", bot))
        {
            Unit* sapTarget = NearestEnemyHealer(ai, bot);
            if (sapTarget && sapTarget != target && !sapTarget->IsInCombat() &&
                !ai->HasAura("sap", sapTarget) && DrLevel(sapTarget, DR_INCAP) < 2)
            {
                if (bot->CanReachWithMeleeAttack(sapTarget))
                {
                    if (Cast(ai, "sap", sapTarget))
                        return true;
                }
                else if (sServerFacade.GetDistance2d(bot, sapTarget) < 30.0f)
                {
                    if (sServerFacade.GetDistance2d(bot, sapTarget) > 15.0f)
                        Cast(ai, "sprint", bot);
                    bot->GetMotionMaster()->MovePoint(0, sapTarget->GetPositionX(), sapTarget->GetPositionY(),
                                                      sapTarget->GetPositionZ(), FORCED_MOVEMENT_RUN);
                    ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
                    return true;
                }
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

        // stunlock with DR sense: kidney at 3+ unless the stun bracket is
        // spent or they're already stunned; pool to ~55 energy first so the
        // stun window opens with a loaded bar instead of an empty one
        if (combo >= 3 && !target->HasAuraType(SPELL_AURA_MOD_STUN) &&
            DrLevel(target, DR_STUN) < 2)
        {
            if (bot->GetPower(POWER_ENERGY) < 55 && target->GetHealthPercent() > 40.0f)
            {
                ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
                return true;    // white swings while energy pools
            }
            if (Cast(ai, "kidney shot", target))
                return true;
        }
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

    // diagnostic: every dungeon log so far shows ZERO finishers despite
    // hundreds of builders — report the combo value the executor sees
    {
        static std::map<uint32, uint32> comboLog;
        uint32 nowMs = WorldTimer::getMSTime();
        uint32& lastMs = comboLog[bot->GetObjectGuid().GetCounter()];
        if (bot->IsInCombat() && (!lastMs || nowMs - lastMs > 10000))
        {
            lastMs = nowMs;
            sLog.outBasic("RogueCombo: %s combo=%u", bot->GetName(), uint32(combo));
        }
    }

    // pack cleave: blade flurry whenever 2+ mobs are in reach, not just on
    // burn policy (it was gated behind burns and never fired in dungeons)
    {
        uint32 nearbyMelee = 0;
        std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
        for (const ObjectGuid& guid : attackers)
            if (Unit* attacker = ai->GetUnit(guid))
                if (attacker->IsAlive() && bot->CanReachWithMeleeAttack(attacker))
                    ++nearbyMelee;
        if (nearbyMelee >= 2 && !ai->HasAura("blade flurry", bot) && Cast(ai, "blade flurry", bot))
            return true;
    }

    // Slice and Dice uptime is the whole spec (icy-veins combat rogue).
    // NB self-cast: SnD is a self-buff — casting it "at the enemy" failed
    // validation every time, and eviscerate was gated behind an aura that
    // therefore never existed (observed: 59 SS, zero finishers, points
    // capped at 5 all run)
    if (combo >= 2 && !ai->HasAura("slice and dice", bot) &&
        (Cast(ai, "slice and dice", bot) || Cast(ai, "slice and dice", target)))
        return true;

    // vanilla combo is PER-TARGET and trash dies in seconds: holding for 5
    // wastes everything (probes: combo=0 at 30 of 36 samples, 93 builders ->
    // one finisher). Spend EARLY on dying mobs; hold to 5 only on bosses.
    bool bossLike = target->GetMaxHealth() > bot->GetMaxHealth() * 3;
    if (!bossLike && combo >= 2 && target->GetHealthPercent() < 40.0f &&
        Cast(ai, "eviscerate", target))
        return true;
    if (!bossLike && combo >= 3 && target->GetHealthPercent() < 60.0f &&
        Cast(ai, "eviscerate", target))
        return true;
    if (combo >= 5)
    {
        // long-lived targets get Rupture, everything else gets Eviscerate;
        // never sit at cap even if SnD refuses to go up
        if (ai->HasAura("slice and dice", bot) && bossLike &&
            !ai->HasAura("rupture", target, false, true) && Cast(ai, "rupture", target))
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
    // NB: no Polymorph vs MOBS, ever — pve cc happens only via directive
    // assignment. Arena players are fair game below.
    if (ThreatCapped(ai, bot, target))
    {
        // wand while parked: negligible threat, real vanilla damage
        if (Cast(ai, "shoot", target))
            return true;
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;   // stop casting until the tank pulls ahead
    }

    if (target->IsPlayer() && bot->InArena())
    {
        // sheep the off-target: the enemy we are NOT killing sits in a
        // sheep — unless he's already controlled, his poly DR is spent,
        // dots would break it instantly, or the kill target is in execute
        // range (no cc during the kill, just kill)
        if (target->GetHealthPercent() > 25.0f)
        {
            AiObjectContext* context = ai->GetAiObjectContext();
            std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
            for (const ObjectGuid& guid : possible)
            {
                Unit* off = ai->GetUnit(guid);
                if (!off || !off->IsPlayer() || !off->IsAlive() || off == target)
                    continue;
                if (IsSoftCrowdControlled(ai, off) || off->HasAuraType(SPELL_AURA_MOD_STUN) ||
                    off->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) || DrLevel(off, DR_POLY) >= 2)
                    continue;
                if (Cast(ai, "polymorph", off))
                    return true;
                break;  // one candidate per tick is enough
            }
        }

        // shatter setup: nova the kill target at point blank — but never
        // when it would shatter the team's OWN cc (sheep/sap/blind/fear
        // on anyone inside nova radius)
        if (bot->IsWithinDistInMap(target, 11.0f) && !target->HasAuraType(SPELL_AURA_MOD_ROOT))
        {
            bool ccInRadius = false;
            AiObjectContext* context = ai->GetAiObjectContext();
            std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
            for (const ObjectGuid& guid : possible)
            {
                Unit* nearby = ai->GetUnit(guid);
                if (nearby && nearby->IsPlayer() && nearby->IsAlive() &&
                    bot->IsWithinDistInMap(nearby, 12.0f) && IsSoftCrowdControlled(ai, nearby))
                {
                    ccInRadius = true;
                    break;
                }
            }
            if (!ccInRadius && Cast(ai, "frost nova", bot))
                return true;
        }
    }

    // mana economy: gem first (instant, works under fire), evocation only
    // when nothing is hitting the mage (8s channel, pushback otherwise)
    if (uint32 manaMax = bot->GetMaxPower(POWER_MANA))
    {
        uint32 manaPct = bot->GetPower(POWER_MANA) * 100 / manaMax;
        uint32 nowMs = WorldTimer::getMSTime();
        if (manaPct < 40)
        {
            static std::map<uint32, uint32> lastGemMs;
            uint32& lastGem = lastGemMs[bot->GetObjectGuid().GetCounter()];
            if (!lastGem || nowMs - lastGem > 30000)   // gem cd is 2 min; don't spam the fail path
                if (Item* gem = FindBagItem(bot, MANA_GEM_IDS, sizeof(MANA_GEM_IDS) / sizeof(uint32)))
                {
                    lastGem = nowMs;
                    SpellCastTargets targets;
                    targets.setUnitTarget(bot);
                    bot->CastItemUseSpell(gem, targets, 0);
                    return true;
                }
        }
        if (manaPct < 20 && !bot->InArena())
        {
            bool beingHit = false;
            std::list<ObjectGuid> hostiles = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
            for (const ObjectGuid& guid : hostiles)
                if (Unit* mob = ai->GetUnit(guid))
                    if (mob->GetVictim() == bot)
                    {
                        beingHit = true;
                        break;
                    }
            if (!beingHit && Cast(ai, "evocation", bot))
                return true;
        }
    }

    // dungeon packs: Blizzard beats single-target once the tank has a pile
    // (Strat runs were pure Frostbolt into 4-mob pulls). NB this block spent
    // three tuning rounds inside EleShamanTick by mistake — every "mage still
    // not AoEing" report was this. Count from ATTACKERS, not "possible
    // targets" (the latter came up empty all night).
    if (!target->IsPlayer() && target->GetVictim() && target->GetVictim() != bot)
    {
        uint32 packed = 0;
        std::list<ObjectGuid> hostiles = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
        for (const ObjectGuid& guid : hostiles)
            if (Unit* mob = ai->GetUnit(guid))
                if (!mob->IsPlayer() && mob->IsAlive() &&
                    mob->GetDistance(target) < 10.0f)
                    ++packed;
        if (target->IsAlive() && !target->IsPlayer())
            packed = packed > 0 ? packed : 1;   // target itself counts
        uint32 maxMana = bot->GetMaxPower(POWER_MANA);
        {
            static std::map<uint32, uint32> lastPack;
            uint32 nowMs = WorldTimer::getMSTime();
            uint32& last = lastPack[bot->GetObjectGuid().GetCounter()];
            if (packed >= 2 && (!last || nowMs - last > 10000))
            {
                last = nowMs;
                sLog.outBasic("MagePack: %s sees packed=%u near %s", bot->GetName(), packed,
                              target->GetName());
            }
        }
        if (packed >= 3 && maxMana && bot->GetPower(POWER_MANA) * 100 / maxMana > 30)
        {
            if (Cast(ai, "blizzard", target))
                return true;
            // dest-targeted cast refused? name it so we know to re-plumb
            DpsIdleProbe(ai, bot, "blizzard-failed");
        }
    }

    // spec by talent tab (legible sensor): 0 = arcane, 1 = fire, 2 = frost
    int spec = AiFactory::GetPlayerSpecTab(bot);

    if (spec == 1)
    {
        if (BurnPolicy(ai) && Cast(ai, "combustion", bot))
            return true;
        // 5x Improved Scorch stacks — but only on targets that live long
        // enough to repay the ramp (last session: 70 scorches to 3 fireballs
        // because every trash mob got the full stack treatment)
        if (target->GetMaxHealth() > bot->GetMaxHealth() * 3)
        {
            Aura* vulnerability = ai->GetAura("fire vulnerability", target);
            uint32 stacks = vulnerability ? vulnerability->GetStackAmount() : 0;
            int32 remaining = vulnerability ? vulnerability->GetAuraDuration() : 0;
            if ((stacks < 5 || remaining < 4000) && Cast(ai, "scorch", target))
                return true;
        }
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

    // frozen target: ice lance shatters, and it works on the move — the
    // instant follow-up when frostbolt can't be channeled
    if (target->HasAuraType(SPELL_AURA_MOD_ROOT) && Cast(ai, "ice lance", target))
        return true;
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

    // pack AoE: sweeping strikes doubles everything, cleave replaces HS as
    // the rage dump (vanilla arms cleave rotation)
    uint32 nearbyMelee = 0;
    {
        std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
        for (const ObjectGuid& guid : attackers)
            if (Unit* attacker = ai->GetUnit(guid))
                if (attacker->IsAlive() && bot->CanReachWithMeleeAttack(attacker))
                    ++nearbyMelee;
    }
    if (nearbyMelee >= 2 && Cast(ai, "sweeping strikes", bot))
        return true;

    if (Cast(ai, "mortal strike", target))
        return true;

    // rage dump only above the MS reserve
    if (bot->GetPower(POWER_RAGE) > 60 &&
        Cast(ai, nearbyMelee >= 2 ? "cleave" : "heroic strike", target))
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
        // wand while parked: negligible threat, real vanilla damage
        if (Cast(ai, "shoot", target))
            return true;
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

// Shadow priest: form first, dots up, mind blast / SW:D on cooldown,
// mind flay filler. Shield under pressure (castable in form); dispersion
// is wotlk-only and simply fails the legality gate on TBC.
bool PartyExecutor::ShadowPriestTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (!ai->HasAura("shadowform", bot) && Cast(ai, "shadowform", bot))
        return true;

    if (bot->GetHealthPercent() < 30.0f && Cast(ai, "dispersion", bot))
        return true;
    if (bot->GetHealthPercent() < 55.0f &&
        !ai->HasAura("weakened soul", bot) && Cast(ai, "power word: shield", bot))
        return true;

    // canonical 3.3.5 priority: VT > DP > Mind Blast > SW:P > Mind Flay
    // (DP is wotlk-trainable; on TBC it's undead-only and the legality
    // gate just skips it. SW:D stays last as a moving/final filler.)
    if (!ai->HasAura("vampiric touch", target) && Cast(ai, "vampiric touch", target))
        return true;
    if (!ai->HasAura("devouring plague", target) && Cast(ai, "devouring plague", target))
        return true;
    if (Cast(ai, "mind blast", target))
        return true;
    if (!ai->HasAura("shadow word: pain", target) && Cast(ai, "shadow word: pain", target))
        return true;
    if (Cast(ai, "mind flay", target))
        return true;
    if (Cast(ai, "shadow word: death", target))
        return true;
    return false;
}

// Generic healer triage: lowest groupmate in range gets the class's heal
// ladder. Built so ARENA opponent healers can live on the executor (the
// old engine left them camping the start room — observed as "fighting
// them one at a time"). Movement: stay glued to the triage target /
// partner; never chase enemies.
// Cure harmful dispellable auras on nearby group members. Spell names cover
// classic through wotlk; Cast() fails cleanly on ranks a class/era lacks.
bool PartyExecutor::DispelPartyTick(PlayerbotAI* ai, Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    uint32 canCure = 0;
    switch (bot->getClass())
    {
        case CLASS_PRIEST:  canCure = (1 << DISPEL_MAGIC) | (1 << DISPEL_DISEASE); break;
        case CLASS_PALADIN: canCure = (1 << DISPEL_MAGIC) | (1 << DISPEL_DISEASE) | (1 << DISPEL_POISON); break;
        case CLASS_SHAMAN:  canCure = (1 << DISPEL_DISEASE) | (1 << DISPEL_POISON); break;
        case CLASS_DRUID:   canCure = (1 << DISPEL_CURSE) | (1 << DISPEL_POISON); break;
        case CLASS_MAGE:    canCure = (1 << DISPEL_CURSE); break;
        default: return false;
    }

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId() ||
            sServerFacade.GetDistance2d(bot, member) > 30.0f)
            continue;

        uint32 needMask = 0;
        Unit::SpellAuraHolderMap const& holders = member->GetSpellAuraHolderMap();
        for (const auto& pair : holders)
        {
            SpellAuraHolder* holder = pair.second;
            if (!holder || holder->IsPositive())
                continue;
            needMask |= (1 << holder->GetSpellProto()->Dispel);
        }
        needMask &= canCure;
        if (!needMask)
            continue;

        // magic (polymorph, fear effects) first, then disease/poison/curse
        if (needMask & (1 << DISPEL_MAGIC))
        {
            if (Cast(ai, "dispel magic", member))
                return true;
            if (Cast(ai, "cleanse", member))
                return true;
        }
        if (needMask & (1 << DISPEL_DISEASE))
        {
            if (Cast(ai, "abolish disease", member))
                return true;
            if (Cast(ai, "cure disease", member))
                return true;
            if (Cast(ai, "cleanse", member))
                return true;
            if (Cast(ai, "purify", member))
                return true;
        }
        if (needMask & (1 << DISPEL_POISON))
        {
            if (Cast(ai, "abolish poison", member))
                return true;
            if (Cast(ai, "cure poison", member))
                return true;
            if (Cast(ai, "cleanse", member))
                return true;
            if (Cast(ai, "purify", member))
                return true;
        }
        if (needMask & (1 << DISPEL_CURSE))
        {
            if (Cast(ai, "remove curse", member))
                return true;
            if (Cast(ai, "remove lesser curse", member))
                return true;
        }
    }
    return false;
}

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

    // movement belongs to SELF-DIRECTED bots only (masterless arena
    // opponents): mastered healers ride the module's follow system, and
    // hijacking them here starved the whole tick behind a mobile master
    // (observed: resto shaman forever chasing a charging warrior, zero
    // casts, zero totems)
    bool const selfDirected = !ai->HasRealPlayerMaster();

    // a hurt partner out of heal range beats everything: close the gap
    if (selfDirected && partner && partnerDistance > 35.0f)
    {
        bot->GetMotionMaster()->MovePoint(0, partner->GetPositionX(), partner->GetPositionY(),
                                          partner->GetPositionZ(), FORCED_MOVEMENT_RUN);
        return true;
    }

    // dispels (Stratholme plagues, polymorphs, poisons) outrank comfort
    // healing but never an emergency heal
    if (lowestPct > 45.0f && DispelPartyTick(ai, bot))
        return true;

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
#ifdef MANGOSBOT_TWO
            // wotlk: Tree of Life form locks out healing touch/regrowth-era
            // direct heals — the tree toolkit is swiftmend/nourish/lifebloom
            if (lowestPct < 35.0f)
            {
                if (Cast(ai, "swiftmend", lowest))
                    return true;
                if (Cast(ai, "nourish", lowest))
                    return true;
            }
            if (lowestPct < 60.0f)
            {
                if (!ai->HasAura("rejuvenation", lowest) && Cast(ai, "rejuvenation", lowest))
                    return true;
                if (Cast(ai, "nourish", lowest))
                    return true;
            }
            if (lowestPct < 85.0f)
            {
                if (!ai->HasAura("rejuvenation", lowest) && Cast(ai, "rejuvenation", lowest))
                    return true;
                if (!ai->HasAura("lifebloom", lowest) && Cast(ai, "lifebloom", lowest))
                    return true;
            }
#else
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
#endif
            break;
        default:
            break;
    }

    // nothing to heal: hold formation on the partner (12yd leash)
    if (selfDirected && partner && partnerDistance > 12.0f)
    {
        bot->GetMotionMaster()->MovePoint(0, partner->GetPositionX(), partner->GetPositionY(),
                                          partner->GetPositionZ(), FORCED_MOVEMENT_RUN);
        return true;
    }

    // nothing needed healing: cure whatever's dispellable before going idle
    if (DispelPartyTick(ai, bot))
        return true;

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
    // when mana is needed for triage. In pvp the picks read the enemy comp:
    // fear class on the other side -> tremor; rogue -> poison cleansing;
    // caster and no melee teammate to serve -> grounding over windfury.
    if (bot->GetPower(POWER_MANA) * 100 / std::max(1u, bot->GetMaxPower(POWER_MANA)) > 25)
    {
        bool enemyCaster = false, enemyFear = false, enemyRogue = false;
        if (bot->InArena())
        {
            std::list<ObjectGuid> possible =
                ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
            for (const ObjectGuid& guid : possible)
            {
                Unit* candidate = ai->GetUnit(guid);
                if (!candidate || !candidate->IsPlayer() || !candidate->IsAlive())
                    continue;
                switch (candidate->getClass())
                {
                    case CLASS_MAGE: case CLASS_SHAMAN: case CLASS_DRUID:
                        enemyCaster = true; break;
                    case CLASS_WARLOCK: case CLASS_PRIEST:
                        enemyCaster = true; enemyFear = true; break;
                    case CLASS_WARRIOR:
                        enemyFear = true; break;
                    case CLASS_ROGUE:
                        enemyRogue = true; break;
                    default: break;
                }
            }
        }

        bool meleeTeammate = false;
        if (group)
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (Player* member = itr->getSource())
                    if (member != bot && member->IsAlive() && member->IsInWorld() &&
                        !ai->IsRanged(member) &&
                        sServerFacade.GetDistance2d(bot, member) < 20.0f)
                        meleeTeammate = true;

        if (!bot->GetTotem(TOTEM_SLOT_AIR) &&
            Cast(ai, (enemyCaster && !meleeTeammate) ? "grounding totem" : "windfury totem", bot))
            return true;
        if (!bot->GetTotem(TOTEM_SLOT_EARTH) &&
            Cast(ai, enemyFear ? "tremor totem" : "strength of earth totem", bot))
            return true;
        if (!bot->GetTotem(TOTEM_SLOT_WATER) &&
            Cast(ai, enemyRogue ? "poison cleansing totem" : "mana spring totem", bot))
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

    // arena: keep the DR ledger current (one observation per enemy per
    // second, whichever bot's tick gets there first)
    if (bot->InArena())
    {
        std::list<ObjectGuid> drGuids = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
        for (const ObjectGuid& guid : drGuids)
            if (Unit* enemy = ai->GetUnit(guid))
                if (enemy->IsPlayer() && enemy->IsAlive())
                    DrObserve(ai, enemy);
    }

    // mid-cast OR mid-channel: let it land instead of walking through it.
    // skipChanneled=true here made blizzard invisible to the guard — the
    // next tick's fireball stomped every channel almost instantly.
    if (bot->IsNonMeleeSpellCasted(false, false, true))
    {
        DpsIdleProbe(ai, bot, "mid-cast");
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return;
    }

    // healers: triage owns the tick — never fall through to dps logic
    if (IsHealerSpec(bot))
    {
        bool acted = HealerTriageTick(ai, bot);
        ai->SetAIInternalUpdateDelay(acted ? AFTER_CAST_DELAY_MS : IDLE_DELAY_MS);
        return;
    }

    // 1. standing cc duty from the seam
    if (KeepCcApplied(ai, bot))
    {
        DpsIdleProbe(ai, bot, "cc-duty");
        return;
    }

    // 2/3. reflexes + one target decision
    Unit* target = PickTarget(ai, bot);
    if (!target)
    {
        DpsIdleProbe(ai, bot, "no-target");
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return;
    }

    if (TryInterrupt(ai, bot, target))
        return;

    if (EngageTarget(ai, bot, target))
        return;

    if (MeleeGetBehind(ai, bot, target))
    {
        DpsIdleProbe(ai, bot, "repositioning");
        return;
    }

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
        case CLASS_PRIEST:  acted = ShadowPriestTick(ai, bot, target); break;
        case CLASS_SHAMAN:  acted = EleShamanTick(ai, bot, target); break;
        case CLASS_DRUID:   acted = BalanceDruidTick(ai, bot, target); break;
        default:            acted = false; break;
    }

    if (!acted)
    {
        DpsIdleProbe(ai, bot, "rotation-idle");
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
    }
}
