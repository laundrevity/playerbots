#pragma once

// PartyExecutor — the party bots' brain, written from scratch.
//
// Owns EVERY decision tick (combat and non-combat) for every bot serving a
// real player's party in PvE. The inherited strategy/relevance engine never
// runs for these bots: no looting, no wandering, no unprompted cc — every
// behavior below is a readable rule. The module is used strictly as an
// actuator library (CanCastSpell/CastSpell = legality; follow / set behind /
// reach melee / food / drink = locomotion & upkeep) and as read-only sensors
// ("attackers" list, threat manager). Battlegrounds/arenas and the ambient
// random bots keep the old engine.
//
// Combat, in order:
//   1. cc assignments from the seam are a standing duty (and the ONLY cc)
//   2. interrupt reflex (target mid-cast -> kick/counterspell/shield bash)
//   3. one target rule: human's skull > LLM kill order > role default
//      (tank: mob-on-healer emergency, then lowest-threat cycling;
//       dps: assist the tank) > nearest attacker
//   4. dps threat ceiling: specials stop above 90% of the tank's threat
//   5. researched per-class rotations (prot warrior, combat rogue, mage,
//      ret paladin; sources in wow-tbc-local docs) — others auto-attack
//
// Out of combat:
//   follow the master, consume directives, eat/drink when low, and (tank)
//   CHARGE-pull the skull or the LLM's ordered target. Nothing else: no
//   loot, no travel, no grind.

#include "Entities/ObjectGuid.h"

class Player;
class PlayerbotAI;
class Unit;

namespace ai
{
    class PartyExecutor
    {
    public:
        static bool ShouldOwn(PlayerbotAI* ai, Player* bot);
        static void Tick(PlayerbotAI* ai, Player* bot);
        static void ReloadRoutes();     // ".bot reload" re-reads party_routes.json

    private:
        static void CombatTick(PlayerbotAI* ai, Player* bot);
        static void NonCombatTick(PlayerbotAI* ai, Player* bot);

        static bool Cast(PlayerbotAI* ai, const char* spell, Unit* target);
        static bool KeepCcApplied(PlayerbotAI* ai, Player* bot);
        static bool TryInterrupt(PlayerbotAI* ai, Player* bot, Unit* target);
        static Unit* LooseMobOnParty(PlayerbotAI* ai, Player* bot);
        static Unit* LowestThreatAttacker(PlayerbotAI* ai, Player* bot);
        static Unit* PickTarget(PlayerbotAI* ai, Player* bot);
        static bool EngageTarget(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool ThreatCapped(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool MeleeGetBehind(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool TankFaceAway(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool KeepPartyBuffed(PlayerbotAI* ai, Player* bot);
        static bool TryChargePull(PlayerbotAI* ai, Player* bot);
        static bool RouteAdvance(PlayerbotAI* ai, Player* bot);
        static bool AutoAdvance(PlayerbotAI* ai, Player* bot);
        static bool EngagePull(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool FollowLeader(PlayerbotAI* ai, Player* bot);
        static bool BurnPolicy(PlayerbotAI* ai);
        static bool HoldPolicy(PlayerbotAI* ai);

        static bool TankWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool ArmsWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool ProtPaladinTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool RogueTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool MageTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool RetPaladinTick(PlayerbotAI* ai, Player* bot, Unit* target);
    };
}
