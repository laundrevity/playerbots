#pragma once

// Phase 6 dungeon consumable doctrine (spec: wow-tbc-local docs, 2026-07-24).
// Three roles (tank / dps / healer) with class- and faction-specific
// overrides; persistent buffs and weapon consumables out of combat, gated
// active items (potions, runes, tea, explosives, oil) in combat. Bots
// auto-restock in dungeons so bags never block doctrine. Route-predictive
// items (pre-pull FAP/LIP) wait for route annotations — v2.

class Player;
class PlayerbotAI;
class Unit;

namespace ai
{
    class Consumables
    {
    public:
        // persistent buffs, weapon stones/oils/poisons, restock, bandages.
        // Returns true when it consumed the tick (a cast/apply happened).
        static bool OutOfCombatTick(PlayerbotAI* ai, Player* bot);

        // gated actives: LAP under cc, mighty rage, major mana, runes,
        // thistle tea, oil of immolation, sapper/dynamite/holy water.
        // target may be null (healers). Returns true when an item was used.
        static bool CombatTick(PlayerbotAI* ai, Player* bot, Unit* target);
    };
}
