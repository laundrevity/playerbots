#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/PartyExecutor.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/directives/Consumables.h"
#include "playerbot/strategy/values/NearestGameObjects.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "playerbot/strategy/directives/DirectiveMgr.h"
#include "playerbot/strategy/directives/DirectiveValues.h"
#include "playerbot/strategy/directives/DungeonRoutePlanner.h"
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
#include "Server/DBCStores.h"
#include "Util/Timer.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <vector>

using namespace ai;
using nlohmann::json;

namespace
{
    constexpr uint32 AFTER_CAST_DELAY_MS = 500;
    constexpr uint32 IDLE_DELAY_MS = 250;
    constexpr uint32 NONCOMBAT_DELAY_MS = 500;

    // dps hold their specials above this fraction of the tank's threat
    // (TBC pulls aggro at 110% melee / 130% ranged; 0.9 leaves margin)
    constexpr float THREAT_CEILING = 0.9f;

    // route chunks and leashes (yards)
    constexpr float ROUTE_LEASH = 25.0f;   // was 35: tank drifted a room away
                                           // toward gauntlet gate pulling
    constexpr float ROUTE_CHUNK = 80.0f;    // one smooth spline per chunk
    constexpr float ROUTE_PULL_RANGE = 24.0f;

    // Pull readiness combines recovery with formation. The tank leads, but
    // does not begin the next engagement until the party is close and visible.
    bool PartyReadyToPull(Group* group, Player* tank, bool fast = false,
                          Player** blocker = nullptr, const char** reason = nullptr)
    {
        if (blocker)
            *blocker = nullptr;
        if (reason)
            *reason = nullptr;
        auto fail = [&](Player* member, const char* why)
        {
            if (blocker)
                *blocker = member;
            if (reason)
                *reason = why;
            return false;
        };

        // fast = chain-pull pacing: only genuinely dangerous states wait
        uint32 hpWait = fast ? 4 : 6;      // tenths: below 40%/60% hp
        uint32 manaWait = fast ? 3 : 5;    // tenths: below 30%/50% mana
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->IsInWorld())
                continue;
            if (member->IsInCombat())
                return fail(member, "combat");
            if (member->GetHealth() * 10 < member->GetMaxHealth() * hpWait)
                return fail(member, "health");
            if (member->GetPowerType() == POWER_MANA &&
                member->GetPower(POWER_MANA) * 10 < member->GetMaxPower(POWER_MANA) * manaWait)
                return fail(member, "mana");   // let the mana users drink
            if (!tank || member == tank)
                continue;
            if (member->GetMapId() != tank->GetMapId())
                return fail(member, "map");
            float limit = (!member->GetPlayerbotAI() || PlayerbotAI::IsHeal(member)) ? 28.0f : 22.0f;
            if (sServerFacade.GetDistance2d(tank, member) > limit)
                return fail(member, "distance");
            if (!tank->IsWithinLOSInMap(member))
                return fail(member, "los");
        }
        return true;
    }

    void LogPullWait(PlayerbotAI* ai, Player* tank, Player* blocker,
                     const char* reason, const char* source)
    {
        // Diagnostic throttles do not need process-global ordering.
        static thread_local std::map<uint32, uint32> lastLog;
        uint32 now = WorldTimer::getMSTime();
        uint32& last = lastLog[tank->GetObjectGuid().GetCounter()];
        if (last && WorldTimer::getMSTimeDiff(last, now) < 5000)
            return;
        last = now;
        std::string detail = std::string(source ? source : "pull") + ":" +
                             (reason ? reason : "unknown");
        PartyExecutor::LogMovementDecision(ai, tank, "formation-wait",
                                           detail.c_str(), blocker);
    }

    // One deterministic party-wide engagement view: every member's victim
    // plus every hostile currently ATTACKING any member. Per-bot "attackers"
    // values and GetVictim scans both missed healer attackers and split
    // engagements (17:24 run: 16 solo windows; mobs on the healer were
    // invisible to every rescue rule).
    void CollectGroupEngagement(PlayerbotAI* ai, Player* bot, std::vector<Unit*>& out)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return;
        std::set<ObjectGuid> seen;
        auto add = [&](Unit* unit)
        {
            if (unit && !unit->IsPlayer() && unit->IsAlive() &&
                seen.insert(unit->GetObjectGuid()).second)
                out.push_back(unit);
        };
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
                continue;
            add(member->GetVictim());
            for (Unit* attacker : member->getAttackers())
                add(attacker);
        }
    }

    // The server never collides units with gameobjects: a closed door
    // exists only client-side, so bot splines walk straight through
    // portcullises the human cannot pass (observed at the live-side Market
    // Row gate). Movement legs that cross a closed DOOR are refused.
    bool SegmentNearPoint2D(float x1, float y1, float x2, float y2,
                            float px, float py, float tolerance)
    {
        float dx = x2 - x1, dy = y2 - y1;
        float lengthSq = dx * dx + dy * dy;
        float t = 0.0f;
        if (lengthSq > 0.0001f)
        {
            t = ((px - x1) * dx + (py - y1) * dy) / lengthSq;
            t = std::max(0.0f, std::min(1.0f, t));
        }
        float cx = x1 + t * dx, cy = y1 + t * dy;
        float ddx = px - cx, ddy = py - cy;
        return ddx * ddx + ddy * ddy <= tolerance * tolerance;
    }

    bool IsDoorClosed(GameObject* door)
    {
        if (!door || !door->GetGOInfo())
            return false;
        // startOpen INVERTS the state semantics: a permanently-open gateway
        // idles in GO_STATE_READY.
        bool startOpen = door->GetGOInfo()->door.startOpen;
        return (door->GetGoState() == GO_STATE_READY) != startOpen;
    }

    // one grid visit per DECISION, not per candidate (the pull loops call
    // the segment test dozens of times per tick)
    void CollectClosedDoors(Player* bot, std::vector<GameObject*>& out)
    {
        std::list<GameObject*> gos;
        AnyGameObjectInObjectRangeCheck check(bot, 60.0f);
        MaNGOS::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(gos, check);
        Cell::VisitAllObjects((const WorldObject*)bot, searcher, 60.0f);
        for (GameObject* go : gos)
        {
            if (go->GetGoType() != GAMEOBJECT_TYPE_DOOR)
                continue;
            if (IsDoorClosed(go))
                out.push_back(go);
        }
    }

    bool SegmentCrossesDoors(const std::vector<GameObject*>& doors,
                             float x1, float y1, float x2, float y2)
    {
        for (GameObject* go : doors)
            if (SegmentNearPoint2D(x1, y1, x2, y2,
                                   go->GetPositionX(), go->GetPositionY(), 4.0f))
                return true;
        return false;
    }

    GameObject* FindCrossedDoor(const std::vector<GameObject*>& doors,
                                float x1, float y1, float x2, float y2)
    {
        for (GameObject* go : doors)
            if (SegmentNearPoint2D(x1, y1, x2, y2,
                                   go->GetPositionX(), go->GetPositionY(), 4.0f))
                return go;
        return nullptr;
    }

    Player* FindGroupDoorKey(Player* bot, GameObject* door, uint32& keyItem)
    {
        keyItem = 0;
        if (!bot || !door || !door->GetGOInfo())
            return nullptr;
        LockEntry const* lock = sLockStore.LookupEntry(door->GetGOInfo()->GetLockId());
        if (!lock)
            return nullptr;

        Group* group = bot->GetGroup();
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_ITEM || !lock->Index[i])
                continue;
            keyItem = lock->Index[i];
            if (bot->HasItemCount(keyItem, 1))
                return bot;
            if (!group)
                continue;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (member && member->IsInWorld() &&
                    member->GetMapId() == bot->GetMapId() &&
                    member->GetInstanceId() == bot->GetInstanceId() &&
                    member->HasItemCount(keyItem, 1))
                    return member;
            }
        }
        keyItem = 0;
        return nullptr;
    }

    bool TryOpenRouteDoor(PlayerbotAI* ai, Player* bot, GameObject* door,
                          const std::string& routeDetail)
    {
        uint32 keyItem = 0;
        Player* keyHolder = FindGroupDoorKey(bot, door, keyItem);
        if (!keyHolder)
            return false;

        char detail[192];
        snprintf(detail, sizeof(detail), "%s door=%u key=%u holder=%s",
                 routeDetail.c_str(), door->GetEntry(), keyItem, keyHolder->GetName());
        float interaction = door->GetInteractionDistance();
        if (bot->GetDistance(door) > interaction - 0.5f)
        {
            // Gameobjects do not collide server-side. GetContactPoint picked
            // the far side of Stratholme's portcullises and the tank walked
            // through the visibly closed gate before Use could run. Derive
            // the stand point from the tank's CURRENT side instead.
            float dx = bot->GetPositionX() - door->GetPositionX();
            float dy = bot->GetPositionY() - door->GetPositionY();
            float distance = std::sqrt(dx * dx + dy * dy);
            float standOff = std::max(1.0f, interaction - 0.75f);
            if (distance < 0.1f)
            {
                dx = -std::cos(bot->GetOrientation());
                dy = -std::sin(bot->GetOrientation());
                distance = 1.0f;
            }
            float x = door->GetPositionX() + dx / distance * standOff;
            float y = door->GetPositionY() + dy / distance * standOff;
            PartyExecutor::CancelRouteMovement(bot);
            bot->GetMotionMaster()->MovePoint(0, x, y, bot->GetPositionZ(), FORCED_MOVEMENT_RUN);
            PartyExecutor::LogMovementDecision(ai, bot, "door-approach", detail, nullptr);
        }
        else if (keyHolder->GetDistance(door) > interaction)
        {
            PartyExecutor::CancelRouteMovement(bot);
            PartyExecutor::LogMovementDecision(ai, bot, "door-key-wait", detail, keyHolder);
        }
        else
        {
            // Preserve lock ownership: the group member who actually carries
            // the key performs the interaction once both they and the tank
            // have reached the gate.
            door->Use(keyHolder);
            PartyExecutor::LogMovementDecision(
                ai, bot, IsDoorClosed(door) ? "door-open-failed" : "door-open",
                detail, keyHolder);
        }
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    bool PathCrossesClosedDoor(Player* bot, float destX, float destY)
    {
        std::vector<GameObject*> doors;
        CollectClosedDoors(bot, doors);
        return SegmentCrossesDoors(doors, bot->GetPositionX(), bot->GetPositionY(), destX, destY);
    }

    // wand churn guard: 17:24 run had 26 Shoot starts and ONE completion —
    // every re-cast cancels the previous autorepeat before it fires
    bool AutoRepeating(Player* bot)
    {
        return bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;
    }

    // role sensor for everything tank-shaped in this file: the strategy-list
    // answer, OR a warrior sitting in Defensive Stance (aura 71). The modern
    // 3/31/17 dual-wield tank reads as FURY by talent tab and would lose the
    // whole tank kit on respec — but any warrior tanking lives in defensive
    // stance (Defiance requires it), so the stance is the honest signal.
    bool IsTankBot(Player* player)
    {
        // sticky per session: charge-pulls flip to battle stance for a moment
        // and the role must not flap to dps mid-pull (nobody would flip the
        // stance back)
        static std::set<uint32> knownTanks;
        uint32 counter = player->GetObjectGuid().GetCounter();
        if (PlayerbotAI::IsTank(player) ||
            (player->getClass() == CLASS_WARRIOR && player->HasAura(71)))
        {
            knownTanks.insert(counter);
            return true;
        }
        if (knownTanks.count(counter))
            return true;

        // adoption: a 3/31/17-style warrior reads FURY by tab and may log in
        // outside defensive stance — the horde group then had NO tank and
        // nobody ever pulled. Deep prot investment (Defiance depth, 11+
        // points) claims the role when no group member holds it yet.
        if (player->getClass() == CLASS_WARRIOR && player->GetPlayerbotAI())
        {
            static std::set<uint32> adoptionChecked;
            if (!adoptionChecked.count(counter))
            {
                adoptionChecked.insert(counter);
                bool groupHasTank = false;
                if (Group* group = player->GetGroup())
                    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* member = itr->getSource();
                        if (!member || member == player)
                            continue;
                        if (knownTanks.count(member->GetObjectGuid().GetCounter()) ||
                            PlayerbotAI::IsTank(member) ||
                            (member->getClass() == CLASS_WARRIOR && member->HasAura(71)))
                        {
                            groupHasTank = true;
                            break;
                        }
                    }
                std::map<uint32, int32> tabs = AiFactory::GetPlayerSpecTabs(player);
                if (!groupHasTank && tabs[2] >= 11)
                {
                    knownTanks.insert(counter);
                    sLog.outBasic("PartyExecutor: %s adopts TANK role (prot depth %d, no other tank)",
                                  player->GetName(), tabs[2]);
                    return true;
                }
            }
        }
        return false;
    }

    // Structured, append-only threat evidence for each tank-held engagement.
    // This records the actual core threat values rather than inferring tank
    // coverage from victims or combat-log damage.
    void LogThreatCoverage(PlayerbotAI* ai, Player* tank, const char* reason)
    {
        Group* group = tank->GetGroup();
        if (!group)
            return;

        std::vector<Unit*> engaged;
        CollectGroupEngagement(ai, tank, engaged);

        uint32 tankVictims = 0;
        uint32 covered = 0;
        uint32 inMelee = 0;
        uint32 zeroTankThreat = 0;
        json mobs = json::array();
        for (Unit* mob : engaged)
        {
            if (!mob->CanHaveThreatList())
                continue;

            float tankThreat = mob->getThreatManager().getThreat(tank);
            float totalGroupThreat = 0.0f;
            float highestOtherThreat = 0.0f;
            std::string highestOtherName;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (!member || !member->IsInWorld() ||
                    member->GetMapId() != tank->GetMapId())
                    continue;
                float threat = mob->getThreatManager().getThreat(member);
                totalGroupThreat += threat;
                if (member != tank && threat > highestOtherThreat)
                {
                    highestOtherThreat = threat;
                    highestOtherName = member->GetName();
                }
            }

            Unit* victim = mob->GetVictim();
            bool owned = victim == tank;
            bool melee = tank->CanReachWithMeleeAttack(mob);
            bool threatCovered = tankThreat > 0.0f &&
                                 (highestOtherThreat <= 0.0f ||
                                  tankThreat >= highestOtherThreat * THREAT_CEILING);
            tankVictims += owned ? 1 : 0;
            covered += threatCovered ? 1 : 0;
            inMelee += melee ? 1 : 0;
            zeroTankThreat += tankThreat <= 0.0f ? 1 : 0;

            json row = {
                {"guid", mob->GetObjectGuid().GetRawValue()},
                {"name", mob->GetName()},
                {"victim", victim ? victim->GetName() : ""},
                {"tank_threat", tankThreat},
                {"highest_other", highestOtherName},
                {"highest_other_threat", highestOtherThreat},
                {"tank_headroom", tankThreat - highestOtherThreat},
                {"tank_share", totalGroupThreat > 0.0f ? tankThreat / totalGroupThreat : 0.0f},
                {"distance", sServerFacade.GetDistance2d(tank, mob)},
                {"in_melee", melee},
                {"owned", owned},
                {"covered", threatCovered},
            };
            mobs.push_back(row);
        }

        Player* master = ai->GetMaster();
        json summary = {
            {"engaged", mobs.size()},
            {"tank_victims", tankVictims},
            {"threat_covered", covered},
            {"in_melee", inMelee},
            {"zero_tank_threat", zeroTankThreat},
        };
        if (master && master->IsInWorld() && master->GetMapId() == tank->GetMapId())
            summary["master_distance"] = tank->GetDistance(master);

        json snapshot = {
            {"t", uint64(std::time(nullptr))},
            {"ms", WorldTimer::getMSTime()},
            {"tank", tank->GetName()},
            {"reason", reason ? reason : "periodic"},
            {"summary", summary},
            {"mobs", mobs},
        };

        static std::mutex s_threatMutex;
        static std::ofstream s_threatLog;
        std::lock_guard<std::mutex> lock(s_threatMutex);
        if (!s_threatLog.is_open())
        {
            s_threatLog.open("../logs/threat_coverage.jsonl", std::ios::app);
            if (!s_threatLog.is_open())
                s_threatLog.open("threat_coverage.jsonl", std::ios::app);
            if (!s_threatLog.is_open())
                return;
        }
        s_threatLog << snapshot.dump() << "\n";
        s_threatLog.flush();
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
        {
            for (auto itr = s_drStates.begin(); itr != s_drStates.end();)
                if (itr->second.observed + 300 < now)
                    itr = s_drStates.erase(itr);
                else
                    ++itr;
        }
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
        // ShouldOwn runs concurrently on the map-worker pool. Keep this
        // diagnostic throttle per worker to avoid racing std::map mutations
        // while random bots are logging in.
        static thread_local std::map<uint32, uint32> lastLog;
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
    if (bot->getClass() != CLASS_WARRIOR || !IsTankBot(bot))
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

    Player* blocker = nullptr;
    const char* reason = nullptr;
    if (Group* group = bot->GetGroup())
        if (!PartyReadyToPull(group, bot, false, &blocker, &reason))
        {
            LogPullWait(ai, bot, blocker, reason, "marked");
            return true;
        }

    if (PathCrossesClosedDoor(bot, target->GetPositionX(), target->GetPositionY()))
    {
        LogMovementDecision(ai, bot, "door-blocked", "marked", target);
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;   // do not let the configured route override the mark
    }

    float distance = sServerFacade.GetDistance2d(bot, target);
    if (distance <= ROUTE_PULL_RANGE)
        return EngagePull(ai, bot, target);

    Player* master = ai->GetMaster();
    float masterDistance = master ? sServerFacade.GetDistance2d(bot, master) : ROUTE_LEASH;
    float advance = std::min(12.0f, ROUTE_LEASH - masterDistance - 1.0f);
    if (advance < 2.0f)
    {
        LogMovementDecision(ai, bot, "formation-wait", "marked:master-leash", master);
        CancelRouteMovement(bot);
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    PathFinder pathfinder(bot);
    pathfinder.setPathLengthLimit(ROUTE_CHUNK);
    pathfinder.calculate(target->GetPositionX(), target->GetPositionY(),
                         target->GetPositionZ(), false);
    if (pathfinder.getPathType() & PATHFIND_NOPATH)
    {
        LogMovementDecision(ai, bot, "marked-no-path", nullptr, target);
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    const PointsArray& path = pathfinder.getPath();
    if (path.size() < 2)
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    PointsArray bounded;
    bounded.push_back(path.front());
    float remaining = advance;
    for (size_t i = 1; i < path.size() && remaining > 0.0f; ++i)
    {
        float dx = path[i].x - path[i - 1].x;
        float dy = path[i].y - path[i - 1].y;
        float dz = path[i].z - path[i - 1].z;
        float segment = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (segment <= remaining)
        {
            bounded.push_back(path[i]);
            remaining -= segment;
            continue;
        }
        float ratio = remaining / segment;
        bounded.push_back(G3D::Vector3(path[i - 1].x + dx * ratio,
                                       path[i - 1].y + dy * ratio,
                                       path[i - 1].z + dz * ratio));
        remaining = 0.0f;
    }
    if (bounded.size() < 2)
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    Movement::MoveSplineInit init(*bot);
    init.MovebyPath(bounded);
    init.SetWalk(false);
    init.Launch();
    LogMovementDecision(ai, bot, "marked-approach", nullptr, target);
    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
    return true;
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
    if (cls == CLASS_PALADIN && IsTankBot(bot) &&
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

// The tank walks point in a routed dungeon by default. Authority and pace are
// independent: normal/fast use the route, manual/steer use human facing.
bool PartyExecutor::RouteAdvance(PlayerbotAI* ai, Player* bot)
{
    if (!IsTankBot(bot))
        return false;
    std::string pullPolicy = ai->GetAiObjectContext()->GetValue<std::string>("pull policy")->Get();
    if (HoldPolicy(ai) || pullPolicy == "hold" ||
        pullPolicy == "steer" || pullPolicy == "manual")
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    if (!sDungeonRoutePlanner.HasRoute(bot->GetMapId()))
        return false;   // no route for this map: master-steered mode

    Player* master = ai->GetMaster();
    Group* group = bot->GetGroup();
    if (!master || !group || !master->IsInWorld() || master->GetMapId() != bot->GetMapId())
        return false;

    // wait for the human: never advance further than the leash
    if (sServerFacade.GetDistance2d(bot, master) > ROUTE_LEASH)
    {
        if (!bot->movespline->Finalized() ||
            bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            CancelRouteMovement(bot);
            LogMovementDecision(ai, bot, "route-leash-stop", nullptr, master);
        }
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    Player* blocker = nullptr;
    const char* waitReason = nullptr;
    if (!PartyReadyToPull(group, bot, pullPolicy == "fast", &blocker, &waitReason))
    {
        LogPullWait(ai, bot, blocker, waitReason, "route");
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    DungeonRouteObjective objective;
    std::string routeState;
    if (!sDungeonRoutePlanner.NextObjective(bot, objective, routeState))
    {
        static std::map<uint32, uint32> lastStateLog;
        uint32 now = WorldTimer::getMSTime();
        uint32& last = lastStateLog[bot->GetObjectGuid().GetCounter()];
        if (!last || WorldTimer::getMSTimeDiff(last, now) >= 10000)
        {
            last = now;
            LogMovementDecision(ai, bot, routeState.c_str(), nullptr, nullptr);
        }
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;    // configured route owns authority, including completion
    }
    Unit* objectiveUnit = objective.bossGuid ? map->GetCreature(objective.bossGuid) : nullptr;
    float objX = objectiveUnit ? objectiveUnit->GetPositionX() : objective.x;
    float objY = objectiveUnit ? objectiveUnit->GetPositionY() : objective.y;
    float objZ = objectiveUnit ? objectiveUnit->GetPositionZ() : objective.z;
    std::string routeDetail = objective.pathId + "/" + objective.nodeId;

    // clear what stands in the way: nearest out-of-combat hostile inside a
    // ~75° cone from the TANK toward the objective (includes the boss when
    // the group finally fronts him)
    float dxo = objX - bot->GetPositionX(), dyo = objY - bot->GetPositionY();
    float dobj = std::sqrt(dxo * dxo + dyo * dyo);
    float hx = dobj > 0.1f ? dxo / dobj : std::cos(bot->GetOrientation());
    float hy = dobj > 0.1f ? dyo / dobj : std::sin(bot->GetOrientation());

    AiObjectContext* context = ai->GetAiObjectContext();
    std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
    std::vector<GameObject*> closedDoors;
    CollectClosedDoors(bot, closedDoors);
    Unit* pick = nullptr;
    float best = ROUTE_PULL_RANGE;
    for (const ObjectGuid& guid : possible)
    {
        Unit* candidate = ai->GetUnit(guid);
        if (!candidate || sServerFacade.UnitIsDead(candidate) || candidate->IsInCombat())
            continue;
        if (candidate->IsPlayer())
            continue;
        if (!sServerFacade.IsHostileTo(candidate, bot))
            continue;
        if (candidate->GetTypeId() == TYPEID_UNIT && ((Creature*)candidate)->IsCritter())
            continue;
        float dx = candidate->GetPositionX() - bot->GetPositionX();
        float dy = candidate->GetPositionY() - bot->GetPositionY();
        float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < 1.0f || distance > best)
            continue;
        if ((dx * hx + dy * hy) / distance < 0.25f)
            continue;   // outside the cone
        if (SegmentCrossesDoors(closedDoors, bot->GetPositionX(), bot->GetPositionY(),
                                candidate->GetPositionX(), candidate->GetPositionY()))
            continue;   // behind a closed door: not pullable (charge splines
                        // ignore doors entirely — the westward gate zap)
        best = distance;
        pick = candidate;
    }
    if (pick)
    {
        uint32 planned = 0;
        for (const ObjectGuid& guid : possible)
            if (Unit* candidate = ai->GetUnit(guid))
                if (candidate->IsAlive() && !candidate->IsPlayer() &&
                    sServerFacade.IsHostileTo(candidate, bot) &&
                    sServerFacade.GetDistance2d(candidate, pick) <= 12.0f)
                    ++planned;
        char detail[192];
        snprintf(detail, sizeof(detail), "%s pack=%u", routeDetail.c_str(), planned);
        LogMovementDecision(ai, bot, "route-plan-pull", detail, pick);
        return EngagePull(ai, bot, pick);
    }

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
    {
        LogMovementDecision(ai, bot, "route-no-path", routeDetail.c_str(), objectiveUnit);
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    const PointsArray& points = pathfinder.getPath();
    if (points.size() < 2)
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return true;
    }

    for (size_t i = 1; i < points.size(); ++i)
        if (GameObject* door = FindCrossedDoor(closedDoors,
                                              points[i - 1].x, points[i - 1].y,
                                              points[i].x, points[i].y))
        {
            if (TryOpenRouteDoor(ai, bot, door, routeDetail))
                return true;
            static std::map<uint32, uint32> lastBlockMs;
            uint32 nowMs = WorldTimer::getMSTime();
            uint32& lastBlock = lastBlockMs[bot->GetObjectGuid().GetCounter()];
            if (!lastBlock || nowMs - lastBlock > 5000)
            {
                lastBlock = nowMs;
                LogMovementDecision(ai, bot, "door-blocked", routeDetail.c_str(), objectiveUnit);
            }
            ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
            return true;    // stand until the door opens
        }

    Movement::MoveSplineInit init(*bot);
    init.MovebyPath(points);
    init.SetWalk(false);
    init.Launch();
    LogMovementDecision(ai, bot, "route-plan", routeDetail.c_str(), objectiveUnit);
    ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
    return true;
}

void PartyExecutor::ReloadRoutes()
{
    sDungeonRoutePlanner.Reload();
}

// The tank pulls ON HIS OWN. The human's facing is the route intent: the
// next pack is the nearest attackable, out-of-combat hostile inside a ~75°
// cone along the master's heading. Pacing is a human tank's checklist —
// nobody in combat, nobody eating/low, healer has mana, party in tow.
bool PartyExecutor::AutoAdvance(PlayerbotAI* ai, Player* bot)
{
    if (bot->getClass() != CLASS_WARRIOR && !IsTankBot(bot))
        return false;
    if (!IsTankBot(bot))
        return false;
    AiObjectContext* context = ai->GetAiObjectContext();
    std::string pullPolicy = context->GetValue<std::string>("pull policy")->Get();
    if (HoldPolicy(ai) || pullPolicy == "hold")
        return false;   // "stop pulling" in party chat parks the tank (sticky)
    bool steer = pullPolicy == "steer";
    bool manual = pullPolicy == "manual";
    bool fast = pullPolicy == "fast";

    if (steer)
    {
        static std::map<uint32, uint32> steerStarted;
        uint32 counter = bot->GetObjectGuid().GetCounter();
        uint32 now = WorldTimer::getMSTime();
        uint32& started = steerStarted[counter];
        if (!started)
            started = now;
        if (WorldTimer::getMSTimeDiff(started, now) >= 20000)
        {
            started = 0;
            context->GetValue<std::string>("pull policy")->Set("");
            LogMovementDecision(ai, bot, "steer-timeout", "route-resumed", nullptr);
            return true;
        }
    }

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
    Player* blocker = nullptr;
    const char* waitReason = nullptr;
    if (!PartyReadyToPull(group, bot, fast, &blocker, &waitReason))
    {
        LogPullWait(ai, bot, blocker, waitReason, manual ? "manual" : (steer ? "steer" : "auto"));
        return true;
    }

    // next pack: nearest hostile in the master's heading cone. Fast mode
    // ignores the cone. Normal route fallback also takes anything close to
    // the tank; explicit steer mode is strict so the human's facing wins.
    float heading = master->GetOrientation();
    float hx = std::cos(heading), hy = std::sin(heading);
    std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
    std::vector<GameObject*> closedDoorsAuto;
    CollectClosedDoors(bot, closedDoorsAuto);
    Unit* pick = nullptr;
    float best = 45.0f;
    for (const ObjectGuid& guid : possible)
    {
        Unit* candidate = ai->GetUnit(guid);
        if (!candidate || sServerFacade.UnitIsDead(candidate) || candidate->IsInCombat())
            continue;
        if (candidate->IsPlayer())
            continue;
        if (!sServerFacade.IsHostileTo(candidate, bot))
            continue;
        if (candidate->GetTypeId() == TYPEID_UNIT && ((Creature*)candidate)->IsCritter())
            continue;
        float dx = candidate->GetPositionX() - master->GetPositionX();
        float dy = candidate->GetPositionY() - master->GetPositionY();
        float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < 1.0f || distance > best)
            continue;
        // inside the heading cone? (dot product against the facing vector)
        if (!fast && (dx * hx + dy * hy) / distance < 0.25f)   // cos(~75°)
        {
            if (steer || manual || candidate->GetDistance(bot) > 20.0f)
                continue;   // off-route and not near the tank either
        }
        if (SegmentCrossesDoors(closedDoorsAuto, bot->GetPositionX(), bot->GetPositionY(),
                                candidate->GetPositionX(), candidate->GetPositionY()))
            continue;   // behind a closed door: not pullable
        best = distance;
        pick = candidate;
    }
    if (!pick)
        return false;   // nothing ahead: turn to steer me

    LogMovementDecision(ai, bot, "auto-pull",
                        steer ? "steer" : (manual ? "manual" : (fast ? "fast" : "cone")), pick);
    bool acted = EngagePull(ai, bot, pick);
    if (acted && steer &&
        (pick->IsInCombat() || bot->GetVictim() == pick || !bot->movespline->Finalized()))
    {
        context->GetValue<std::string>("pull policy")->Set("");
        LogMovementDecision(ai, bot, "steer-complete", "route-resumed", pick);
    }
    return acted;
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
                member->GetMapId() == bot->GetMapId() && IsTankBot(member) &&
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
    // nobody ever walks: a stray MOVEFLAG_WALK from any earlier strategy
    // sticks forever and the bot ambles behind the party ("mage is walking")
    if (bot->IsWalking())
        bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);

    // never yank a drinking/eating bot into follow — the aura cancels on
    // movement and the mana never comes back
    if ((ai->HasAura("drink", bot) && bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA)) ||
        (ai->HasAura("food", bot) && bot->GetHealth() < bot->GetMaxHealth()))
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return;
    }

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

    // "come to me" outranks everything the tick could otherwise want
    if (ComeToMasterTick(ai, bot))
        return;

    // NEUTRAL mobs (Strat citizens) only ever aggro their engager: they
    // never appear in a dps bot's per-player attackers list and never put
    // the dps in combat — so the tank soloed every citizen pack while the
    // group watched from the out-of-combat tick. A member fighting a living
    // npc pulls every non-healer bot into the fight.
    if (!IsHealerSpec(bot))
    {
        // party-wide engagement view: also sees mobs beating on the HEALER
        // (they have no offensive victim to scan for) and split fights
        std::vector<Unit*> engaged;
        CollectGroupEngagement(ai, bot, engaged);
        Unit* join = nullptr;
        float joinBest = 40.0f;
        for (Unit* mob : engaged)
        {
            if (!bot->IsWithinDistInMap(mob, joinBest))
                continue;
            joinBest = bot->GetDistance(mob);
            join = mob;
        }
        if (join && EngageTarget(ai, bot, join))
            return;
    }

    // mage: evocation between pulls beats a 20-second drink (8s channel,
    // 8 min cd — the 00:10 session logged TWELVE drinks and zero evocations)
    if (bot->getClass() == CLASS_MAGE &&
        bot->GetPower(POWER_MANA) * 2 < bot->GetMaxPower(POWER_MANA) &&
        Cast(ai, "evocation", bot))
        return;

    // upkeep between pulls (humans drink; bots that never drink are a tell).
    // 60% trigger: at 50% the tank's ready-check (also 50%) could pull the
    // moment before the sit — the mage entered fights at half mana.
    if (bot->GetPowerType() == POWER_MANA && bot->GetPower(POWER_MANA) * 10 < bot->GetMaxPower(POWER_MANA) * 6)
        if (ai->DoSpecificAction("drink", Event(), true))
            return;
    if (bot->GetHealth() * 2 < bot->GetMaxHealth())
        if (ai->DoSpecificAction("food", Event(), true))
            return;

    // keep the party buffed before anyone thinks about pulling
    if (KeepPartyBuffed(ai, bot))
        return;

    // phase-6 consumable doctrine: persistent buffs, weapon stones/oils/
    // poisons, bandages, and dungeon auto-restock (throttled inside)
    if (Consumables::OutOfCombatTick(ai, bot))
    {
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return;
    }

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
        // conjuring is a CAST and cancels an in-progress drink — never
        // conjure while drink-worthy ("mage wasn't drinking"), except the
        // waterless bootstrap where there is nothing to drink yet
        bool thirsty = bot->GetPower(POWER_MANA) * 10 < bot->GetMaxPower(POWER_MANA) * 6;
        if ((water == 0 || !thirsty) && water < 10 && Cast(ai, "conjure water", bot))
            return;
        if (!thirsty && food < 5 && Cast(ai, "conjure food", bot))
            return;
        // conjure gems only near-full: ruby costs ~1370 mana, and a low-mana
        // attempt silently falls through to a worse gem (observed: jade x4
        // from a mage who knows ruby)
        if (bot->GetPower(POWER_MANA) * 10 >= bot->GetMaxPower(POWER_MANA) * 7 &&
            !FindBagItem(bot, MANA_GEM_IDS, sizeof(MANA_GEM_IDS) / sizeof(uint32)))
        {
#ifdef MANGOSBOT_ZERO
            // by spell id: the chat-helper name index resolved these
            // unreliably (known ruby refused DONT_REPORT while an unknown
            // jade cast fine). ruby 10054 > citrine 10053 > jade 3552 > agate 759.
            if (ai->CastSpell(10054u, bot) || ai->CastSpell(10053u, bot) ||
                ai->CastSpell(3552u, bot) || ai->CastSpell(759u, bot))
                return;
            sLog.outBasic("MageGem: %s all gem conjures refused (mana=%u/%u)",
                          bot->GetName(), bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA));
#else
            if (Cast(ai, "conjure mana ruby", bot) || Cast(ai, "conjure mana citrine", bot) ||
                Cast(ai, "conjure mana jade", bot) || Cast(ai, "conjure mana agate", bot))
                return;
#endif
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

    // tank: skull/LLM pull orders first, then route by default. Sticky steer
    // bypasses RouteAdvance and uses the human-facing AutoAdvance path.
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
        case CLASS_WARRIOR:
        {
            // 16:10 run: 306 hostile cast starts, zero interrupts — every
            // warrior tried shield bash and this party dual-wields. Pummel
            // is the DW interrupt (berserker stance; the fury tick lives
            // there). Shield bash only when a shield is actually equipped.
            Item* offhand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            bool hasShield = offhand && offhand->GetProto() &&
                             offhand->GetProto()->InventoryType == INVTYPE_SHIELD;
            if (hasShield && Cast(ai, "shield bash", target))
                return true;
            return Cast(ai, "pummel", target);
        }
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
    std::vector<Unit*> engaged;
    CollectGroupEngagement(ai, bot, engaged);
    for (Unit* attacker : engaged)
    {
        if (!attacker || sServerFacade.UnitIsDead(attacker))
            continue;
        Unit* victim = attacker->GetVictim();
        if (!victim || victim == bot)
            continue;
        Player* member = victim->IsPlayer() ? (Player*)victim : nullptr;
        if (!member || !bot->GetGroup() || member->GetGroup() != bot->GetGroup())
            continue;
        if (IsTankBot(member))
            continue;
        return attacker;
    }
    return nullptr;
}

// tank threat cycling: the engaged mob where MY threat is lowest is the one
// about to peel off — feed it the next sunder/slam
Unit* PartyExecutor::LowestThreatAttacker(PlayerbotAI* ai, Player* bot)
{
    std::vector<Unit*> engaged;
    CollectGroupEngagement(ai, bot, engaged);
    Unit* lowest = nullptr;
    float best = 0.0f;
    for (Unit* attacker : engaged)
    {
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
    if (IsTankBot(bot))
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
        // rescue outranks the assist train: a mob eating the master or a
        // healer dies FIRST, and any party member being SWARMED (2+ mobs)
        // is everyone's problem — Hfuryc solo-tanked a 5-pack for 40s while
        // three warriors watched, because only master/healers triggered this.
        {
            // sourced from the party-wide engagement view: per-bot attackers
            // missed healer attackers and split fights (17:24 run: 16 solo
            // windows, eleven of them mobs on the healer)
            std::vector<Unit*> engaged;
            CollectGroupEngagement(ai, bot, engaged);
            Player* master = ai->GetMaster();
            std::map<ObjectGuid, uint32> perVictim;
            for (Unit* mob : engaged)
            {
                if (!mob->GetVictim() || !mob->GetVictim()->IsPlayer())
                    continue;
                Player* victim = (Player*)mob->GetVictim();
                if (victim->GetGroup() == group)
                    ++perVictim[victim->GetObjectGuid()];
            }
            Unit* menace = nullptr;
            float best = 1000.0f;
            bool bestProtectee = false;
            for (Unit* mob : engaged)
            {
                if (IsSoftCrowdControlled(ai, mob))
                    continue;
                Unit* victim = mob->GetVictim();
                if (!victim || !victim->IsPlayer() || victim == bot)
                    continue;
                Player* member = (Player*)victim;
                if (member->GetGroup() != group)
                    continue;
                bool protectee = (master && member == master) ||
                                 PlayerbotAI::IsHeal(member);
                bool swarmed = perVictim[victim->GetObjectGuid()] >= 2 &&
                               !IsTankBot(member);
                if (!protectee && !swarmed)
                    continue;
                float distance = sServerFacade.GetDistance2d(bot, mob);
                // master/healer targets always outrank a swarmed-dps rescue
                if (protectee && !bestProtectee)
                {
                    best = distance;
                    menace = mob;
                    bestProtectee = true;
                }
                else if (protectee == bestProtectee && distance < best)
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
            if (member && member != bot && member->IsInWorld() && IsTankBot(member))
                if (Unit* tanked = member->GetVictim())
                    if (!sServerFacade.UnitIsDead(tanked))
                        return UncappedAlternative(ai, bot, tanked);
        }
    }

    // 3d. nearest group-engaged hostile (party-wide view — works from both
    // ticks and sees healer attackers), leashed to the tank for dps: no
    // private fights 35y+ from the anvil (Amageone's ghoul sat 57-62y out)
    std::vector<Unit*> engagedView;
    CollectGroupEngagement(ai, bot, engagedView);
    Player* leashTank = nullptr;
    if (group && !IsTankBot(bot))
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member != bot && member->IsInWorld() &&
                member->GetMapId() == bot->GetMapId() && IsTankBot(member))
            {
                leashTank = member;
                break;
            }
        }
    Unit* nearest = nullptr;
    float best = 1000.0f;
    for (Unit* attacker : engagedView)
    {
        if (sServerFacade.UnitIsDead(attacker))
            continue;
        if (leashTank && sServerFacade.GetDistance2d(leashTank, attacker) > 35.0f)
            continue;   // dps leash: outside the tank's fight is not our fight
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
    {
        // roam probe: a dps resolving a target far from the TANK is how the
        // Hfuryc solo-wander started — name it when it happens again
        if (!IsTankBot(bot) && group)
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (member && member != bot && member->IsInWorld() && IsTankBot(member))
                {
                    float tankDist = sServerFacade.GetDistance2d(member, nearest);
                    if (tankDist > 30.0f)
                    {
                        static std::map<uint32, uint32> lastRoamLog;
                        uint32 nowMs = WorldTimer::getMSTime();
                        uint32& lastMs = lastRoamLog[bot->GetObjectGuid().GetCounter()];
                        if (!lastMs || WorldTimer::getMSTimeDiff(lastMs, nowMs) >= 10000)
                        {
                            lastMs = nowMs;
                            sLog.outBasic("DpsRoam: %s target %s is %.0fy from tank %s",
                                          bot->GetName(), nearest->GetName(), tankDist, member->GetName());
                        }
                    }
                    break;
                }
            }
        return UncappedAlternative(ai, bot, nearest);
    }

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
    if (IsTankBot(bot))
        return false;
    if (target->GetHealthPercent() < 20.0f)
        return false;   // execute window: it dies before threat matters
    Unit* tank = target->GetVictim();
    if (!tank || tank == bot || !tank->IsPlayer() || !IsTankBot((Player*)tank))
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

// parked at the ceiling on ONE mob while the tank holds others with
// headroom stalls the whole pull (market row: the entire dps core idled on
// a big fresh pack until the human marked skulls — the ceiling froze them
// on the tank's main target). Find another engaged, un-cc'd pack mob with
// threat headroom instead of standing there.
Unit* PartyExecutor::UncappedAlternative(PlayerbotAI* ai, Player* bot, Unit* chosen)
{
    if (!chosen || IsTankBot(bot) || !ThreatCapped(ai, bot, chosen))
        return chosen;
    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    Unit* best = nullptr;
    float bestDist = 1000.0f;
    for (const ObjectGuid& guid : attackers)
        if (Unit* mob = ai->GetUnit(guid))
        {
            if (mob == chosen || !mob->IsAlive() || mob->IsPlayer() ||
                IsSoftCrowdControlled(ai, mob))
                continue;
            if (ThreatCapped(ai, bot, mob))
                continue;
            float distance = sServerFacade.GetDistance2d(bot, mob);
            if (distance < bestDist)
            {
                bestDist = distance;
                best = mob;
            }
        }
    return best ? best : chosen;
}

// Movement-decision telemetry: one JSONL line per navigation decision,
// append-only in LogsDir so a mangosd restart cannot destroy the evidence
// (Server.log truncates every boot — the 16:10 gate excursion could not be
// replayed). Fields per the 022 handoff: reason, policy, actor/target
// positions, master/tank 3D distances, LoS to target.
void PartyExecutor::LogMovementDecision(PlayerbotAI* ai, Player* bot, const char* reason,
                                        const char* detail, Unit* target)
{
    static std::mutex s_moveMutex;
    static std::ofstream s_moveLog;
    std::lock_guard<std::mutex> lock(s_moveMutex);
    if (!s_moveLog.is_open())
    {
        const char* primary = "../logs/movement_decisions.jsonl";
        // rotate at 50MB: append-only by design, unbounded by accident
        {
            std::ifstream probe(primary, std::ios::ate | std::ios::binary);
            if (probe.is_open() && probe.tellg() > std::streamoff(50) * 1024 * 1024)
            {
                probe.close();
                std::rename(primary, "../logs/movement_decisions.1.jsonl");
            }
        }
        s_moveLog.open(primary, std::ios::app);
        if (!s_moveLog.is_open())
            s_moveLog.open("movement_decisions.jsonl", std::ios::app);
        if (!s_moveLog.is_open())
            return;
    }
    std::string policy = ai->GetAiObjectContext()->GetValue<std::string>("pull policy")->Get();
    Player* master = ai->GetMaster();
    Player* tank = nullptr;
    Group* group = bot->GetGroup();
    if (group)
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
                IsTankBot(member))
            {
                tank = member;
                break;
            }
        }

    float maxPartyDist = 0.0f;
    float maxDpsDist = 0.0f;
    bool formationLos = true;
    bool formationKnown = tank && group;
    if (formationKnown)
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || member == tank || !member->IsInWorld() ||
                member->GetMapId() != tank->GetMapId())
                continue;
            float distance = sServerFacade.GetDistance2d(tank, member);
            maxPartyDist = std::max(maxPartyDist, distance);
            if (member->GetPlayerbotAI() && !PlayerbotAI::IsHeal(member) && !IsTankBot(member))
                maxDpsDist = std::max(maxDpsDist, distance);
            formationLos = formationLos && tank->IsWithinLOSInMap(member);
        }

    s_moveLog << "{\"t\":" << uint64(std::time(nullptr))
              << ",\"ms\":" << WorldTimer::getMSTime()
              << ",\"actor\":\"" << bot->GetName() << "\""
              << ",\"reason\":\"" << reason << "\""
              << ",\"detail\":\"" << (detail ? detail : "") << "\""
              << ",\"policy\":\"" << (policy.empty() ? "normal" : policy.c_str()) << "\""
              << ",\"x\":" << bot->GetPositionX() << ",\"y\":" << bot->GetPositionY()
              << ",\"z\":" << bot->GetPositionZ();
    if (target)
        s_moveLog << ",\"target\":\"" << target->GetName() << "\""
                  << ",\"tx\":" << target->GetPositionX() << ",\"ty\":" << target->GetPositionY()
                  << ",\"tz\":" << target->GetPositionZ()
                  << ",\"los\":" << (bot->IsWithinLOSInMap(target) ? 1 : 0);
    if (master && master->IsInWorld() && master->GetMapId() == bot->GetMapId())
        s_moveLog << ",\"masterDist\":" << bot->GetDistance(master);
    if (tank && tank != bot)
        s_moveLog << ",\"tankDist\":" << bot->GetDistance(tank);
    if (formationKnown)
        s_moveLog << ",\"maxPartyDist\":" << maxPartyDist
                  << ",\"maxDpsDist\":" << maxDpsDist
                  << ",\"formationLos\":" << (formationLos ? 1 : 0);
    s_moveLog << "}\n";
    s_moveLog.flush();
}

// stop an in-flight route spline (raw MoveSplineInit) or point move —
// steer/manual/hold/come must not fight a launched leg toward the old goal
void PartyExecutor::CancelRouteMovement(Player* bot)
{
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        bot->GetMotionMaster()->Clear();
    // StopMoving launches a stop spline but does not interrupt the existing
    // raw MoveSplineInit leg. The leash trace showed the tank continue from
    // 26 to 57 yards through repeated stop requests.
    bot->InterruptMoving(true);
}

// "come to me": deterministic spatial recall. Postconditions, not
// acknowledgements: keeps moving to the master's CURRENT position until 3D
// distance <= 8y AND line of sight hold, with a 60s safety timeout.
bool PartyExecutor::ComeToMasterTick(PlayerbotAI* ai, Player* bot)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    uint32 until = context->GetValue<uint32>("come to master until")->Get();
    if (!until)
        return false;

    uint32 nowMs = WorldTimer::getMSTime();
    Player* master = ai->GetMaster();
    if (!master || !master->IsInWorld() || master->GetMapId() != bot->GetMapId() ||
        nowMs > until)
    {
        context->GetValue<uint32>("come to master until")->Set(0);
        LogMovementDecision(ai, bot, "come-timeout", nullptr, master);
        return false;
    }

    if (bot->GetDistance(master) <= 8.0f && bot->IsWithinLOSInMap(master))
    {
        context->GetValue<uint32>("come to master until")->Set(0);
        LogMovementDecision(ai, bot, "come-done", nullptr, master);
        return false;
    }

    static std::map<uint32, uint32> s_lastComeMs;
    uint32& lastMove = s_lastComeMs[bot->GetObjectGuid().GetCounter()];
    if (!lastMove || nowMs - lastMove > 2000)
    {
        lastMove = nowMs;
        if (PathCrossesClosedDoor(bot, master->GetPositionX(), master->GetPositionY()))
        {
            LogMovementDecision(ai, bot, "door-blocked", "come", master);
            ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
            return true;    // wait at the door; retry while armed
        }
        CancelRouteMovement(bot);
        bot->GetMotionMaster()->MovePoint(0, master->GetPositionX(), master->GetPositionY(),
                                          master->GetPositionZ(), FORCED_MOVEMENT_RUN);
        LogMovementDecision(ai, bot, "come", nullptr, master);
    }
    ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
    return true;
}

// melee dps belong behind the target (when someone else is tanking it)
bool PartyExecutor::MeleeGetBehind(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (ai->IsRanged(bot) || IsTankBot(bot))
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

    std::vector<Unit*> attackers;
    CollectGroupEngagement(ai, bot, attackers);
    uint32 meleeCount = 0;
    for (Unit* attacker : attackers)
        if (bot->CanReachWithMeleeAttack(attacker))
            ++meleeCount;

    {
        static std::map<uint32, uint32> lastThreatSnapshot;
        uint32 nowMs = WorldTimer::getMSTime();
        uint32& last = lastThreatSnapshot[bot->GetObjectGuid().GetCounter()];
        if (!last || nowMs - last >= 5000)
        {
            last = nowMs;
            LogThreatCoverage(ai, bot, "periodic");
        }
    }

    // the HUMAN being hit is the emergency: taunt that mob off them before
    // any other priority ("tank ignores me pulling aggro")
    if (Player* master = ai->GetMaster())
        if (master != bot && master->IsAlive())
            for (Unit* menace : attackers)
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
        for (Unit* mob : attackers)
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
                // hysteresis: a tank holding a formed pack (4+ in melee)
                // does NOT sprint away and drag it across the room — local
                // answers first (taunt already tried; challenging shout
                // below), the dps healer-peel handles the rest
                if (meleeCount >= 4)
                    swarm = 0;
                static std::map<uint32, uint32> lastRunMs;
                uint32 nowMs = WorldTimer::getMSTime();
                uint32& lastRun = lastRunMs[bot->GetObjectGuid().GetCounter()];
                if (swarm)
                {
                    if (!lastRun || nowMs - lastRun > 3000)
                    {
                        lastRun = nowMs;
                        sLog.outBasic("TankRescue: %s running to %s (%u mobs on the backline)",
                                      bot->GetName(), anchor->GetName(), swarm);
                        {
                            char snapshot[64];
                            snprintf(snapshot, sizeof(snapshot), "swarm=%u melee=%u", swarm, meleeCount);
                            LogMovementDecision(ai, bot, "rescue", snapshot, anchor);
                        }
                        LogThreatCoverage(ai, bot, "rescue");
                        bot->GetMotionMaster()->MovePoint(0, anchor->GetPositionX(),
                            anchor->GetPositionY(), anchor->GetPositionZ(), FORCED_MOVEMENT_RUN);
                    }
                    return true;    // sprint owns the tick only when it runs
                }
                // formed pack held: fall through to local answers (shout,
                // sunder ledger) instead of dragging the anchor
                if (!swarm)
                    LogThreatCoverage(ai, bot, "rescue-blocked-formed-pack");
            }
            if (swarm && Cast(ai, "challenging shout", bot))
            {
                // swarm==0 here means the formed-pack hysteresis fired:
                // don't burn the 10-min shout on mobs it cannot reach
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
        for (Unit* mob : attackers)
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
        // never kite the pack while snared/dazed, and keep gather legs short
        // when surrounded — long back-turned runs through a melee train are
        // the daze machine ("tank showing their back")
        bool snared = bot->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
        float gatherRange = 30.0f;
        // pack-anchor hysteresis: with 3+ mobs already in melee the anchor
        // is WORKING — walking off to a caster drags the pack, spreads it
        // away from blizzard, and exposes the backline (17:24: tank took
        // only 56% of direct melee; gather legs ran 30y from the human)
        if (!snared && meleeCount < 3 && (!last || nowMs - last > 6000) && meleeCount >= 1)
        {
            Unit* pick = nullptr;
            uint32 candidates = 0;
            for (Unit* caster : attackers)
                // any STATIONARY ranged attacker anchors the pack — the
                // mana-only test rejected Strat's ranged skeletons
                // (probes: 24x no-candidate at attackers > melee)
                if (caster->IsAlive() && !caster->IsPlayer() &&
                    !bot->CanReachWithMeleeAttack(caster) &&
                    (caster->GetPowerType() == POWER_MANA || !caster->IsMoving()) &&
                    sServerFacade.GetDistance2d(bot, caster) < gatherRange &&
                    !IsSoftCrowdControlled(ai, caster))
                {
                    ++candidates;
                    if (!pick)
                        pick = caster;
                }
            if (pick)
            {
                last = nowMs;
                Player* master = ai->GetMaster();
                bool beyondMasterLeash =
                    master && master->IsInWorld() &&
                    master->GetMapId() == bot->GetMapId() &&
                    bot->GetDistance(master) > ROUTE_LEASH;
                if (beyondMasterLeash)
                {
                    sLog.outBasic("TankGather: %s blocked beyond %.0fy master leash (master %.0fy, candidate %s)",
                                  bot->GetName(), ROUTE_LEASH,
                                  bot->GetDistance(master), pick->GetName());
                    LogMovementDecision(ai, bot, "gather-blocked", "master-leash", master);
                    LogThreatCoverage(ai, bot, "gather-blocked-master-leash");
                    // Keep processing the local tank rotation below.
                }
                else
                {
                    sLog.outBasic("TankGather: %s -> %s (dist %.0f, melee train %u)",
                                  bot->GetName(), pick->GetName(),
                                  sServerFacade.GetDistance2d(bot, pick), meleeCount);
                    {
                        char snapshot[64];
                        snprintf(snapshot, sizeof(snapshot), "melee=%u attackers=%u",
                                 meleeCount, uint32(attackers.size()));
                        LogMovementDecision(ai, bot, "gather", snapshot, pick);
                    }
                    LogThreatCoverage(ai, bot, "gather");
                    bot->GetMotionMaster()->MovePoint(0, pick->GetPositionX(),
                        pick->GetPositionY(), pick->GetPositionZ(), FORCED_MOVEMENT_RUN);
                    return true;
                }
            }
            if (bot->IsInCombat())
            {
                last = nowMs;   // reuse throttle for the negative report
                sLog.outBasic("TankGather: %s no caster candidate (melee=%u attackers=%u)",
                              bot->GetName(), meleeCount, uint32(attackers.size()));
            }
        }
    }

    // Priority rebuilt after the DW-respec probe (25 min: 108 sunders, 40
    // bloodthirsts on a 6s cd, TWELVE heroic strikes — sunder spam ate every
    // gcd and all the rage). Bloodthirst on cooldown is the core; a loose mob
    // gets ONE ledger-starting sunder, the kill target builds to 5 stacks
    // once; everything past that is the HS/cleave dump.
    if (BurnPolicy(ai) && Cast(ai, "death wish", bot))
        return true;    // fury burn button, honors the shot-caller's 'burn'
    if (Cast(ai, "bloodthirst", target))
        return true;
    if (Cast(ai, "shield slam", target))
        return true;
    if (Cast(ai, "revenge", target))
        return true;

    // tab-sunder: start each loose melee mob's threat ledger ONCE — a mob
    // that's already sundered gained nothing from another 15-rage refresh
    if (meleeCount >= 2)
        for (Unit* attacker : attackers)
            if (attacker != target && attacker->IsAlive() &&
                attacker->GetVictim() != bot &&
                bot->CanReachWithMeleeAttack(attacker) &&
                !IsSoftCrowdControlled(ai, attacker) &&
                !ai->GetAura("sunder armor", attacker) &&
                Cast(ai, "sunder armor", attacker))
                return true;

#ifndef MANGOSBOT_ZERO
    // 1.12 thunder clap is battle-stance-only; TBC+ prot uses it tanking
    if (meleeCount >= 2 && Cast(ai, "thunder clap", target))
        return true;
#endif
    if (meleeCount >= 2 && !ai->HasAura("demoralizing shout", target) &&
        Cast(ai, "demoralizing shout", target))
        return true;    // gate on the debuff: 57 recasts into its own 30s aura
    if (!ai->HasAura("battle shout", bot) && Cast(ai, "battle shout", bot))
        return true;    // maintained: pack-wide threat + party AP (was 5 casts/25min)
    if (Cast(ai, "devastate", target))
        return true;

    // kill-target sunder: build to 5 stacks, refresh only when expiring —
    // NOT the old unconditional refresh that soaked every spare gcd
    {
        Aura* sunder = ai->GetAura("sunder armor", target);
        uint32 stacks = sunder ? sunder->GetStackAmount() : 0;
        int32 remaining = sunder ? sunder->GetAuraDuration() : 0;
        if ((stacks < 5 || remaining < 6000) && Cast(ai, "sunder armor", target))
            return true;
    }

    // rage dump: DW income is huge and HS/cleave ride the next swing (off-
    // gcd) — 30 rage keeps BT funded while actually spending the surplus
    if (bot->GetPower(POWER_RAGE) > 300)
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

    bool bossLike = target->GetMaxHealth() > bot->GetMaxHealth() * 3;

    // dying mob: finish it — don't buff for its corpse (report card: 25 SnD
    // to 3 eviscerates; every trash mob's points went into re-buffing)
    if (!bossLike && combo >= 2 && target->GetHealthPercent() < 40.0f &&
        Cast(ai, "eviscerate", target))
        return true;

    // Slice and Dice uptime is the whole spec (icy-veins combat rogue), but
    // maintenance is CHEAP: 1 point on trash (dies before a long SnD pays),
    // 2+ on bosses. NB self-cast: SnD is a self-buff — casting it "at the
    // enemy" failed validation every time.
    if (combo >= (bossLike ? 2 : 1) && !ai->HasAura("slice and dice", bot) &&
        (Cast(ai, "slice and dice", bot) || Cast(ai, "slice and dice", target)))
        return true;

    // vanilla combo is PER-TARGET and trash dies in seconds: holding for 5
    // wastes everything (probes: combo=0 at 30 of 36 samples, 93 builders ->
    // one finisher). Spend EARLY on dying mobs; hold to 5 only on bosses.
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
        if (AutoRepeating(bot))
        {
            ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
            return true;   // wand already running — do not restart it
        }
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
        uint32 packed = 0, heldByTank = 0;
        std::list<ObjectGuid> hostiles = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
        for (const ObjectGuid& guid : hostiles)
            if (Unit* mob = ai->GetUnit(guid))
                if (!mob->IsPlayer() && mob->IsAlive() &&
                    mob->GetDistance(target) < 10.0f)
                {
                    ++packed;
                    Unit* victim = mob->GetVictim();
                    if (victim && victim->IsPlayer() && IsTankBot((Player*)victim))
                        ++heldByTank;
                }
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
        // 40% floor: blizzard is 1400 mana a cast — the 30% gate let her AoE
        // herself dry and drink for 20s a pull (12 drinks in 10 min).
        // Tank-held gate: AoE into a pack the tank hasn't established yet
        // hands the mage 3-4 mobs ('mage would blizzard once, pull aggro')
        if (packed >= 3 && heldByTank * 2 >= packed &&
            maxMana && bot->GetPower(POWER_MANA) * 100 / maxMana > 40)
        {
            // anchor on the densest tank-held cluster — the current target
            // may sit at the pack's edge while the pile stands on the tank
            Unit* anchor = target;
            uint32 anchorScore = 0;
            for (const ObjectGuid& guid : hostiles)
                if (Unit* mob = ai->GetUnit(guid))
                {
                    if (mob->IsPlayer() || !mob->IsAlive())
                        continue;
                    Unit* victim = mob->GetVictim();
                    if (!victim || !victim->IsPlayer() || !IsTankBot((Player*)victim))
                        continue;
                    uint32 score = 0;
                    for (const ObjectGuid& other : hostiles)
                        if (Unit* neighbour = ai->GetUnit(other))
                            if (!neighbour->IsPlayer() && neighbour->IsAlive() &&
                                mob->GetDistance(neighbour) < 8.0f)
                                ++score;
                    if (score > anchorScore)
                    {
                        anchorScore = score;
                        anchor = mob;
                    }
                }
            if (Cast(ai, "blizzard", anchor))
                return true;
            // name the refusal reason (8 unexplained refusals last session)
            {
                SpellCastResult blizzResult = SPELL_CAST_OK;
                ai->CanCastSpell("blizzard", target, 0, nullptr, false, false, false, &blizzResult);
                sLog.outBasic("MagePack: %s blizzard refused (result=%u, packed=%u)",
                              bot->GetName(), uint32(blizzResult), packed);
            }
            DpsIdleProbe(ai, bot, "blizzard-failed");
        }
    }

    // target-lifetime gate: no 3-second casts into trash the party is
    // already deleting (17:24: 42 of 83 Fireballs died with their target).
    // Instants or wand until the retarget.
    if (!target->IsPlayer() && target->GetMaxHealth() < bot->GetMaxHealth() * 3 &&
        target->GetHealthPercent() < 25.0f)
    {
        if (Cast(ai, "fire blast", target))
            return true;
        if (!AutoRepeating(bot) && Cast(ai, "shoot", target))
            return true;
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;
    }

    // spec by talent tab (legible sensor): 0 = arcane, 1 = fire, 2 = frost
    int spec = AiFactory::GetPlayerSpecTab(bot);

    if (spec == 1)
    {
        if (BurnPolicy(ai) && Cast(ai, "combustion", bot))
            return true;
        // 5x Improved Scorch stacks — but only on targets that live long
        // enough to repay the ramp. 3x let Scholo elite trash qualify
        // (27 scorches in 10 min); 5x means real bosses only.
        if (target->GetMaxHealth() > bot->GetMaxHealth() * 5)
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

bool PartyExecutor::FuryWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    // fury dps (report card: Adps ran ZERO bloodthirst — dps warriors all
    // fell into the arms tick where mortal strike fails and battle stance is
    // forced). Berserker stance, BT on cooldown, whirlwind on packs, execute
    // window, HS/cleave dump. Same doctrine the Hfury trio needs.
    if (ThreatCapped(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return true;
    }

    if (!ai->HasAura("berserker stance", bot) && Cast(ai, "berserker stance", bot))
        return true;
    if (!ai->HasAura("battle shout", bot) && Cast(ai, "battle shout", bot))
        return true;    // imp battle shout is the fury AP centerpiece

    // rage engines first: BT at 4.5/min on a 6s cd = starvation. Bloodrage
    // and berserker rage are free income; both fall through to the rotation.
    if (bot->GetPower(POWER_RAGE) < 200)
        Cast(ai, "bloodrage", bot);
    if (bot->GetPower(POWER_RAGE) < 300)
        Cast(ai, "berserker rage", bot);

    uint32 nearbyMelee = 0;
    {
        std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
        for (const ObjectGuid& guid : attackers)
            if (Unit* attacker = ai->GetUnit(guid))
                if (attacker->IsAlive() && bot->CanReachWithMeleeAttack(attacker))
                    ++nearbyMelee;
    }

    // death wish on real work, not just LLM burn calls (dungeons only see
    // those on emergencies — the fury button rotted on cooldown)
    bool eliteWork = !target->IsPlayer() && target->IsAlive() &&
                     target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->IsElite();
    if ((BurnPolicy(ai) || eliteWork || nearbyMelee >= 3) && Cast(ai, "death wish", bot))
        return true;
    if (BurnPolicy(ai) && Cast(ai, "recklessness", bot))
        return true;

    if (target->IsPlayer() && !ai->HasAura("hamstring", target) && Cast(ai, "hamstring", target))
        return true;

    if (target->GetHealthPercent() < 20.0f && Cast(ai, "execute", target))
        return true;

    if (Cast(ai, "bloodthirst", target))
        return true;

    // whirlwind: packs always; single target only off a full bar (25 rage
    // that would otherwise sit idle)
    if ((nearbyMelee >= 2 || bot->GetPower(POWER_RAGE) > 500) && Cast(ai, "whirlwind", target))
        return true;

    // dump above 35 rage: keeps the next BT funded while spending the rest
    if (bot->GetPower(POWER_RAGE) > 350 &&
        Cast(ai, nearbyMelee >= 2 ? "cleave" : "heroic strike", target))
        return true;

    return false;
}

bool PartyExecutor::ArmsWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    // fury-specced dps warriors get the fury tick (tab 1); arms/undecided
    // fall through to the mortal-strike line below
    if (AiFactory::GetPlayerSpecTab(bot) == 1)
        return FuryWarriorTick(ai, bot, target);

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
        if (AutoRepeating(bot))
        {
            ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
            return true;   // wand already running — do not restart it
        }
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
    // nobody ever walks (see NonCombatTick — the flag sticks from strategy
    // leftovers and slows every chase/reposition too)
    if (bot->IsWalking())
        bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);

    // 0. break hard cc with the pvp medallion (action self-gates: only
    // fires while stunned/feared/charmed/confused and off cooldown)
    if (ai->DoSpecificAction("use pvp trinket", Event(), true))
        return;

    // "come to me" continues even in combat — the human recalling the party
    // through a door outranks the fight they are stuck in
    if (ComeToMasterTick(ai, bot))
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

    // healers: triage owns the tick — never fall through to dps logic.
    // Consumables first: a mana potion IS triage when the tank eats a hit
    // during the 20 seconds the healer would otherwise be dry.
    if (IsHealerSpec(bot))
    {
        if (Consumables::CombatTick(ai, bot, nullptr))
        {
            ai->SetAIInternalUpdateDelay(AFTER_CAST_DELAY_MS);
            return;
        }
        bool acted = HealerTriageTick(ai, bot);
        ai->SetAIInternalUpdateDelay(acted ? AFTER_CAST_DELAY_MS : IDLE_DELAY_MS);
        return;
    }

    // mages and druids own curse removal (vanilla decurse-or-die design) —
    // in combat only healers ran DispelPartyTick, so the mage never
    // decursed. Cheap no-op when nobody's cursed; runs before dps logic so
    // even a threat-capped mage cleans the party.
    if ((bot->getClass() == CLASS_MAGE || bot->getClass() == CLASS_DRUID) &&
        DispelPartyTick(ai, bot))
    {
        ai->SetAIInternalUpdateDelay(AFTER_CAST_DELAY_MS);
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
        // leash-return: a dps with no group-valid target does not stand in
        // a private corner — walk back to the anvil
        if (!IsTankBot(bot))
            if (Group* group = bot->GetGroup())
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->getSource();
                    if (member && member != bot && member->IsInWorld() &&
                        member->GetMapId() == bot->GetMapId() && IsTankBot(member))
                    {
                        if (bot->GetDistance(member) > 20.0f)
                        {
                            static std::map<uint32, uint32> lastLeashMs;
                            uint32 nowMs = WorldTimer::getMSTime();
                            uint32& lastLeash = lastLeashMs[bot->GetObjectGuid().GetCounter()];
                            if (!lastLeash || nowMs - lastLeash > 3000)
                            {
                                lastLeash = nowMs;
                                if (PathCrossesClosedDoor(bot, member->GetPositionX(), member->GetPositionY()))
                                    LogMovementDecision(ai, bot, "door-blocked", "leash-return", member);
                                else
                                {
                                    LogMovementDecision(ai, bot, "leash-return", nullptr, member);
                                    bot->GetMotionMaster()->MovePoint(0, member->GetPositionX(),
                                        member->GetPositionY(), member->GetPositionZ(), FORCED_MOVEMENT_RUN);
                                }
                            }
                        }
                        break;
                    }
                }
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

    // 4b. phase-6 consumables: potions/runes/tea/explosives/oil, all gated
    // and throttled inside; a use costs this tick like any cast
    if (Consumables::CombatTick(ai, bot, target))
    {
        ai->SetAIInternalUpdateDelay(AFTER_CAST_DELAY_MS);
        return;
    }

    // 5. class rotation
    bool acted = false;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: acted = IsTankBot(bot) ? TankWarriorTick(ai, bot, target)
                                                            : ArmsWarriorTick(ai, bot, target); break;
        case CLASS_ROGUE:   acted = RogueTick(ai, bot, target); break;
        case CLASS_MAGE:    acted = MageTick(ai, bot, target); break;
        case CLASS_PALADIN: acted = IsTankBot(bot) ? ProtPaladinTick(ai, bot, target)
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
