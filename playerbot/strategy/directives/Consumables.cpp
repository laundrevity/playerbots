#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/Consumables.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"

#include "Util/Timer.h"

#include <map>

using namespace ai;

namespace
{
    // ---- item ids, all DB-verified against classicmangos 2026-07-24 ----
    constexpr uint32 FLASK_TITANS = 13510, FLASK_SUPREME = 13512, FLASK_WISDOM = 13511;
    constexpr uint32 MONGOOSE = 13452, GREATER_ARCANE = 13454, GREATER_FIREPOWER = 21546,
                     FROST_POWER = 17708, MAGEBLOOD = 20007, FORTITUDE = 3825;
    constexpr uint32 JUJU_POWER = 12451, JUJU_MIGHT = 12460, JUJU_GUILE = 12458;
    constexpr uint32 ROIDS = 8410, SCORPOK = 8412, CEREBRAL = 8423, GIFT_OF_ARTHAS = 9088;
    constexpr uint32 RUMSEY_BLACK = 21151;
    constexpr uint32 ELEMENTAL_STONE = 18262, WIZARD_OIL_ITEM = 20749, MANA_OIL = 20748;
    constexpr uint32 INSTANT_POISON6 = 8928, DEADLY_POISON5 = 20844;
    constexpr uint32 MIGHTY_RAGE = 13442, MAJOR_MANA = 13444, DARK_RUNE = 20520,
                     DEMONIC_RUNE = 12662, THISTLE_TEA = 7676, LIVING_ACTION = 20008;
    constexpr uint32 SAPPER = 10646, DENSE_DYNAMITE = 18641, HOLY_WATER = 13180,
                     OIL_OF_IMMOLATION = 8956, RUNECLOTH_BANDAGE = 14530;

    constexpr uint32 SKILL_ENGINEERING_ID = 202;
    constexpr uint32 SKILL_FIRST_AID_ID = 129;

    constexpr uint32 POTION_THROTTLE_MS = 130 * 1000;      // shared potion cd 2 min
    constexpr uint32 EXPLOSIVE_THROTTLE_MS = 65 * 1000;    // shared explosive cd 1 min
    constexpr uint32 RUNE_THROTTLE_MS = 135 * 1000;
    constexpr uint32 TEA_THROTTLE_MS = 305 * 1000;         // thistle tea cd 5 min
    constexpr uint32 OIL_THROTTLE_MS = 65 * 1000;
    constexpr uint32 RESTOCK_THROTTLE_MS = 60 * 1000;

    struct Throttles
    {
        uint32 potionMs = 0, explosiveMs = 0, runeMs = 0, teaMs = 0, oilMs = 0, restockMs = 0;
    };
    std::map<uint32, Throttles> s_throttles;

    Throttles& ThrottlesFor(Player* bot) { return s_throttles[bot->GetObjectGuid().GetCounter()]; }

    bool Ready(uint32 stampMs, uint32 nowMs, uint32 gapMs)
    {
        return !stampMs || WorldTimer::getMSTimeDiff(stampMs, nowMs) >= gapMs;
    }

    bool TankRole(Player* bot)
    {
        if (PlayerbotAI::IsTank(bot))
            return true;
        return bot->getClass() == CLASS_WARRIOR && bot->HasAura(71);   // defensive stance
    }

    Item* FindItem(Player* bot, uint32 itemId)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetEntry() == itemId)
                    return item;
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                    if (Item* item = pBag->GetItemByPos(j))
                        if (item->GetEntry() == itemId)
                            return item;
        return nullptr;
    }

    // the item's use spell (raw — enchant resolution wants this one)
    uint32 ItemBuffSpell(uint32 itemId)
    {
        ItemPrototype const* proto = ObjectMgr::GetItemPrototype(itemId);
        if (!proto)
            return 0;
        for (const auto& spell : proto->Spells)
            if (spell.SpellId)
                return spell.SpellId;
        return 0;
    }

    // the aura the item actually leaves on you: some use-spells only TRIGGER
    // the real buff (R.O.I.D.S. 10667 -> "Holy Strength" 20007). Checking
    // the use-spell's aura read those buffs as missing and re-ate the item
    // every upkeep pass (finding C2C-20260724-1339-002).
    uint32 ItemBuffAura(uint32 itemId)
    {
        uint32 spellId = ItemBuffSpell(itemId);
        if (!spellId)
            return 0;
        SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellId);
        if (spellInfo)
            for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
                if (spellInfo->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL &&
                    spellInfo->EffectTriggerSpell[i])
                    return spellInfo->EffectTriggerSpell[i];
        return spellId;
    }

    bool HasBuffOf(Player* bot, uint32 itemId)
    {
        uint32 spellId = ItemBuffAura(itemId);
        return spellId && bot->HasAura(spellId);
    }

    bool UseItemOn(Player* bot, Item* item, Unit* unitTarget, bool atDest)
    {
        SpellCastTargets targets;
        if (atDest && unitTarget)
            targets.setDestination(unitTarget->GetPositionX(), unitTarget->GetPositionY(),
                                   unitTarget->GetPositionZ());
        else
            targets.setUnitTarget(unitTarget ? unitTarget : bot);
        bot->CastItemUseSpell(item, targets, 0);
        return true;
    }

    bool ApplySelfBuffItem(Player* bot, uint32 itemId)
    {
        if (HasBuffOf(bot, itemId))
            return false;
        Item* item = FindItem(bot, itemId);
        if (!item)
            return false;   // skip unavailable, never block progress
        return UseItemOn(bot, item, bot, false);
    }

    // temporary weapon enhancement (stone/oil/poison): resolve the enchant id
    // from the item's spell, never overwrite an existing temp enchant
    bool ApplyWeaponConsumable(Player* bot, uint8 slot, uint32 itemId, uint32 charges)
    {
        Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!weapon || weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return false;
        Item* consumable = FindItem(bot, itemId);
        if (!consumable)
            return false;
        uint32 spellId = ItemBuffSpell(itemId);
        SpellEntry const* spellInfo = spellId ? sServerFacade.LookupSpellInfo(spellId) : nullptr;
        if (!spellInfo)
            return false;
        uint32 enchantId = 0;
        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (spellInfo->Effect[i] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                enchantId = spellInfo->EffectMiscValue[i];
        if (!enchantId)
            return false;
        bot->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, false);
        weapon->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchantId, 1800 * IN_MILLISECONDS, charges);
        bot->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, true);
        bot->DestroyItemCount(itemId, 1, true);
        return true;
    }

    void Restock(Player* bot, uint32 itemId, uint32 want)
    {
        if (!itemId)
            return;
        uint32 have = bot->GetItemCount(itemId);
        if (have < want)
            bot->StoreNewItemInBestSlots(itemId, want - have);
    }

    // A category is a set of mutually EXCLUSIVE alternatives, preferred
    // first: satisfied when ANY member's aura is up, else the first
    // available item is applied. This core's stacking rules make Juju Power
    // and R.O.I.D.S. replace each other (runtime: Juju Power aura_off at the
    // same ms Holy Strength lands) — modeling them as independent desires
    // churned buffs and ate consumables forever (C2C-20260724-1339-002).
    using BuffCategory = std::vector<uint32>;

    // persistent sets per the profile doc (well-fed foods deferred: eating
    // channels don't fit the between-pull cadence yet)
    void PersistentSetFor(Player* bot, PlayerbotAI* ai, std::vector<BuffCategory>& out)
    {
        bool tank = TankRole(bot);
        bool healer = PlayerbotAI::IsHeal(bot);
        switch (bot->getClass())
        {
            case CLASS_WARRIOR:
                out = { { FLASK_TITANS }, { MONGOOSE }, { JUJU_POWER, ROIDS },
                        { JUJU_MIGHT }, { FORTITUDE }, { RUMSEY_BLACK } };
                if (tank)
                    out.push_back({ GIFT_OF_ARTHAS });   // tank-only: mobs hit HIM
                break;
            case CLASS_ROGUE:
                out = { { FLASK_TITANS }, { MONGOOSE }, { JUJU_POWER, ROIDS },
                        { JUJU_MIGHT }, { SCORPOK }, { FORTITUDE }, { RUMSEY_BLACK } };
                break;
            case CLASS_MAGE:
            {
                int spec = AiFactory::GetPlayerSpecTab(bot);
                out = { { FLASK_SUPREME }, { GREATER_ARCANE }, { MAGEBLOOD },
                        { CEREBRAL }, { FORTITUDE }, { RUMSEY_BLACK } };
                if (spec == 1)
                    out.push_back({ GREATER_FIREPOWER });
                else if (spec == 2)
                    out.push_back({ FROST_POWER });
                break;
            }
            case CLASS_PALADIN:
                if (healer)
                    out = { { FLASK_WISDOM }, { MAGEBLOOD }, { CEREBRAL },
                            { FORTITUDE }, { RUMSEY_BLACK } };
                break;
            case CLASS_SHAMAN:
                if (healer)
                    out = { { FLASK_WISDOM }, { MAGEBLOOD }, { JUJU_GUILE },
                            { FORTITUDE }, { RUMSEY_BLACK } };
                break;
            default:
                break;
        }
    }
}

bool Consumables::OutOfCombatTick(PlayerbotAI* ai, Player* bot)
{
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;
    Player* master = ai->GetMaster();
    if (!master || master == bot)
        return false;

    uint32 nowMs = WorldTimer::getMSTime();
    Throttles& th = ThrottlesFor(bot);

    // bags stay stocked in dungeons so doctrine never blocks on supplies
    // (localhost server: provisioning is not the gameplay)
    if (Ready(th.restockMs, nowMs, RESTOCK_THROTTLE_MS))
    {
        th.restockMs = nowMs;
        std::vector<BuffCategory> persistent;
        PersistentSetFor(bot, ai, persistent);
        for (const BuffCategory& category : persistent)
            Restock(bot, category.front(), 2);   // stock the winner only
        for (uint32 itemId : { MIGHTY_RAGE, MAJOR_MANA, DARK_RUNE, LIVING_ACTION, THISTLE_TEA,
                               OIL_OF_IMMOLATION })
            Restock(bot, itemId, 3);
        for (uint32 itemId : { SAPPER, DENSE_DYNAMITE, HOLY_WATER })
            Restock(bot, itemId, 5);
        Restock(bot, RUNECLOTH_BANDAGE, 10);
        Restock(bot, ELEMENTAL_STONE, 5);
        if (bot->getClass() == CLASS_ROGUE)
        {
            Restock(bot, INSTANT_POISON6, 5);
            Restock(bot, DEADLY_POISON5, 5);
        }
        if (bot->getClass() == CLASS_MAGE)
            Restock(bot, WIZARD_OIL_ITEM, 2);
        if (PlayerbotAI::IsHeal(bot))
            Restock(bot, MANA_OIL, 2);
    }

    // persistent buffs: a category is satisfied when ANY alternative's aura
    // is up; otherwise the first available item wins. One apply per tick.
    {
        std::vector<BuffCategory> persistent;
        PersistentSetFor(bot, ai, persistent);
        for (const BuffCategory& category : persistent)
        {
            bool satisfied = false;
            for (uint32 itemId : category)
                if (HasBuffOf(bot, itemId))
                {
                    satisfied = true;
                    break;
                }
            if (satisfied)
                continue;
            for (uint32 itemId : category)
                if (ApplySelfBuffItem(bot, itemId))
                    return true;
        }
    }

    // weapon consumables. Horde melee keep the MAIN hand clean for the
    // shaman's Windfury Totem; alliance stones both hands. Casters/healers
    // oil the main hand (rsham keeps mana oil ON despite windfury — doctrine).
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
        {
            bool horde = bot->GetTeam() == HORDE;
            if (!horde && ApplyWeaponConsumable(bot, EQUIPMENT_SLOT_MAINHAND, ELEMENTAL_STONE, 0))
                return true;
            if (ApplyWeaponConsumable(bot, EQUIPMENT_SLOT_OFFHAND, ELEMENTAL_STONE, 0))
                return true;
            break;
        }
        case CLASS_ROGUE:
            if (ApplyWeaponConsumable(bot, EQUIPMENT_SLOT_MAINHAND, INSTANT_POISON6, 115))
                return true;
            if (ApplyWeaponConsumable(bot, EQUIPMENT_SLOT_OFFHAND, DEADLY_POISON5, 115))
                return true;
            break;
        case CLASS_MAGE:
            if (ApplyWeaponConsumable(bot, EQUIPMENT_SLOT_MAINHAND, WIZARD_OIL_ITEM, 0))
                return true;
            break;
        default:
            if (PlayerbotAI::IsHeal(bot) &&
                ApplyWeaponConsumable(bot, EQUIPMENT_SLOT_MAINHAND, MANA_OIL, 0))
                return true;
            break;
    }

    // bandage beats a long eat when only hp is missing (first aid trained)
    if (bot->GetHealth() * 100 < bot->GetMaxHealth() * 85 &&
        bot->GetSkillValue(SKILL_FIRST_AID_ID) >= 225)
        if (Item* bandage = FindItem(bot, RUNECLOTH_BANDAGE))
            return UseItemOn(bot, bandage, bot, false);

    return false;
}

bool Consumables::CombatTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    uint32 nowMs = WorldTimer::getMSTime();
    Throttles& th = ThrottlesFor(bot);

    // 1. control comes first: living action while stunned/rooted
    if ((bot->HasAuraType(SPELL_AURA_MOD_STUN) || bot->HasAuraType(SPELL_AURA_MOD_ROOT)) &&
        Ready(th.potionMs, nowMs, POTION_THROTTLE_MS))
        if (Item* lap = FindItem(bot, LIVING_ACTION))
        {
            th.potionMs = nowMs;
            sLog.outBasic("Consumables: %s living action potion (cc break)", bot->GetName());
            return UseItemOn(bot, lap, bot, false);
        }

    uint32 attackersNear = 0, attackersNearTarget = 0, undeadNearTarget = 0;
    {
        std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
        for (const ObjectGuid& guid : attackers)
            if (Unit* mob = ai->GetUnit(guid))
                if (mob->IsAlive() && !mob->IsPlayer())
                {
                    if (sServerFacade.GetDistance2d(bot, mob) < 5.0f)
                        ++attackersNear;
                    if (target && mob->GetDistance(target) < 8.0f)
                    {
                        ++attackersNearTarget;
                        if (mob->GetCreatureType() == CREATURE_TYPE_UNDEAD)
                            ++undeadNearTarget;
                    }
                }
    }

    // 2. mana keeps the group moving: major mana potion, then a rune
    uint32 manaMax = bot->GetMaxPower(POWER_MANA);
    if (manaMax && (PlayerbotAI::IsHeal(bot) || bot->getClass() == CLASS_MAGE))
    {
        uint32 manaPct = bot->GetPower(POWER_MANA) * 100 / manaMax;
        uint32 hpPct = bot->GetHealth() * 100 / bot->GetMaxHealth();
        uint32 potionAt = PlayerbotAI::IsHeal(bot) ? 30 : 35;
        if (manaPct <= potionAt && Ready(th.potionMs, nowMs, POTION_THROTTLE_MS))
            if (Item* pot = FindItem(bot, MAJOR_MANA))
            {
                th.potionMs = nowMs;
                sLog.outBasic("Consumables: %s major mana potion (%u%%)", bot->GetName(), manaPct);
                return UseItemOn(bot, pot, bot, false);
            }
        if (manaPct <= 60 && hpPct >= 80 && Ready(th.runeMs, nowMs, RUNE_THROTTLE_MS))
        {
            Item* rune = FindItem(bot, DARK_RUNE);
            if (!rune)
                rune = FindItem(bot, DEMONIC_RUNE);
            if (rune)
            {
                th.runeMs = nowMs;
                sLog.outBasic("Consumables: %s rune (mana %u%%, hp %u%%)", bot->GetName(), manaPct, hpPct);
                return UseItemOn(bot, rune, bot, false);
            }
        }
    }

    // 3. mighty rage: warriors low on rage with real work in front of them
    if (bot->getClass() == CLASS_WARRIOR && bot->GetPower(POWER_RAGE) <= 350 &&
        Ready(th.potionMs, nowMs, POTION_THROTTLE_MS))
    {
        bool eliteWork = target && !target->IsPlayer() && target->IsAlive() &&
                         ((Creature*)target)->IsElite();
        if (eliteWork || attackersNear >= 3 || ai->HasAura("death wish", bot))
            if (Item* mrp = FindItem(bot, MIGHTY_RAGE))
            {
                th.potionMs = nowMs;
                sLog.outBasic("Consumables: %s mighty rage potion", bot->GetName());
                return UseItemOn(bot, mrp, bot, false);
            }
    }

    // 4. thistle tea: rogue starved during a burn window or boss
    if (bot->getClass() == CLASS_ROGUE && bot->GetPower(POWER_ENERGY) <= 20 &&
        Ready(th.teaMs, nowMs, TEA_THROTTLE_MS))
    {
        bool burning = ai->HasAura("blade flurry", bot) || ai->HasAura("adrenaline rush", bot) ||
                       (target && !target->IsPlayer() &&
                        target->GetMaxHealth() > bot->GetMaxHealth() * 3);
        if (burning)
            if (Item* tea = FindItem(bot, THISTLE_TEA))
            {
                th.teaMs = nowMs;
                sLog.outBasic("Consumables: %s thistle tea", bot->GetName());
                return UseItemOn(bot, tea, bot, false);
            }
    }

    // 5. explosives on packs (engineering-gated). Holy water outranks the
    // generic explosive against undead density; sapper needs the pack ON us.
    if (bot->GetSkillValue(SKILL_ENGINEERING_ID) >= 250 &&
        Ready(th.explosiveMs, nowMs, EXPLOSIVE_THROTTLE_MS) && target && !target->IsPlayer())
    {
        Item* charge = nullptr;
        bool atDest = false;
        const char* what = nullptr;
        if (undeadNearTarget >= 3 && (charge = FindItem(bot, HOLY_WATER)))
        {
            atDest = true;
            what = "stratholme holy water";
        }
        else if (attackersNear >= 4 && (charge = FindItem(bot, SAPPER)))
            what = "goblin sapper charge";
        else if (attackersNearTarget >= 3 && (charge = FindItem(bot, DENSE_DYNAMITE)))
        {
            atDest = true;
            what = "dense dynamite";
        }
        if (charge)
        {
            th.explosiveMs = nowMs;
            sLog.outBasic("Consumables: %s %s (near=%u pack=%u undead=%u)",
                          bot->GetName(), what, attackersNear, attackersNearTarget, undeadNearTarget);
            return UseItemOn(bot, charge, target, atDest);
        }
    }

    // 6. oil of immolation: melee roles standing in a pack that will live
    if (attackersNear >= 3 && !PlayerbotAI::IsHeal(bot) && !ai->IsRanged(bot) &&
        Ready(th.oilMs, nowMs, OIL_THROTTLE_MS))
        if (Item* oil = FindItem(bot, OIL_OF_IMMOLATION))
        {
            th.oilMs = nowMs;
            sLog.outBasic("Consumables: %s oil of immolation (near=%u)", bot->GetName(), attackersNear);
            return UseItemOn(bot, oil, bot, false);
        }

    return false;
}
