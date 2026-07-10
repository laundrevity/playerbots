#pragma once

// PartyExecutor — the party bots' combat brain, written from scratch.
//
// This file REPLACES the inherited strategy/relevance engine for every bot
// serving a real player's party while in combat. There is no priority soup:
// every decision below is a readable rule, in order, and the only external
// inputs are (a) the LLM/script directives from the seam and (b) the raid
// marks + chat of the human. The module is used strictly as an actuator
// library (CanCastSpell/CastSpell = legality, movement actions = locomotion)
// and as read-only sensors ("attackers" list). Out-of-combat behavior
// (follow, loot, eat) still runs the module engine for now; combat never
// does. Battlegrounds/arenas keep the old brain (separately tuned).
//
// Decision order each tick:
//   1. If this bot has a cc assignment from a directive -> keep it applied.
//   2. Reflex: my target is casting and I have an interrupt -> use it.
//   3. Tank reflex: a mob is on the healer/dps -> taunt it / switch to it.
//   4. Target = skull mark > directive kill order > (tank: attackers,
//      dps: the tank's target) > nearest attacker.
//   5. Class rotation: short, explicit priority list per class.
//   6. Nothing castable -> ensure auto-attack + close distance.

#include "Entities/ObjectGuid.h"

class Player;
class PlayerbotAI;
class Unit;

namespace ai
{
    class PartyExecutor
    {
    public:
        // party bot + in combat + enabled -> the executor owns this tick
        static bool ShouldOwn(PlayerbotAI* ai, Player* bot);
        static void Tick(PlayerbotAI* ai, Player* bot);

    private:
        static bool Cast(PlayerbotAI* ai, const char* spell, Unit* target);
        static bool KeepCcApplied(PlayerbotAI* ai, Player* bot);
        static bool TryInterrupt(PlayerbotAI* ai, Player* bot, Unit* target);
        static Unit* LooseMobOnParty(PlayerbotAI* ai, Player* bot);
        static Unit* PickTarget(PlayerbotAI* ai, Player* bot);
        static bool EngageTarget(PlayerbotAI* ai, Player* bot, Unit* target);

        static bool TankWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool RogueTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool MageTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool RetPaladinTick(PlayerbotAI* ai, Player* bot, Unit* target);
        static bool GenericMeleeTick(PlayerbotAI* ai, Player* bot, Unit* target);
    };
}
