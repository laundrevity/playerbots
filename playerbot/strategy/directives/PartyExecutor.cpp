#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/PartyExecutor.h"

#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/directives/DirectiveMgr.h"
#include "playerbot/strategy/directives/DirectiveValues.h"

#include "Groups/Group.h"
#include "MotionGenerators/MotionMaster.h"
#include "Util/Timer.h"

#include <cmath>

using namespace ai;

namespace
{
    constexpr uint32 AFTER_CAST_DELAY_MS = 500;
    constexpr uint32 IDLE_DELAY_MS = 250;
    constexpr uint32 NONCOMBAT_DELAY_MS = 500;

    // dps hold their specials above this fraction of the tank's threat
    // (TBC pulls aggro at 110% melee / 130% ranged; 0.9 leaves margin)
    constexpr float THREAT_CEILING = 0.9f;
}

bool PartyExecutor::ShouldOwn(PlayerbotAI* ai, Player* bot)
{
    if (!sPlayerbotAIConfig.executorEnabled || !sPlayerbotAIConfig.directiveEnabled)
        return false;
    if (!ai->HasRealPlayerMaster())
        return false;
    if (bot->InBattleGround())
        return false;   // bg/arena keep the old brain (separately tuned)
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

void PartyExecutor::NonCombatTick(PlayerbotAI* ai, Player* bot)
{
    // directives are consumed out of combat too (pull orders arrive here)
    if (sDirectiveMgr.HasPending(bot->GetObjectGuid()))
        if (ai->DoSpecificAction("apply directive", Event(), true))
            return;

    // tank initiates pulls with charge — never with a ranged weapon
    if (TryChargePull(ai, bot))
        return;

    // upkeep between pulls (humans drink; bots that never drink are a tell)
    if (bot->GetPowerType() == POWER_MANA && bot->GetPower(POWER_MANA) * 2 < bot->GetMaxPower(POWER_MANA))
        if (ai->DoSpecificAction("drink", Event(), true))
            return;
    if (bot->GetHealth() * 2 < bot->GetMaxHealth())
        if (ai->DoSpecificAction("food", Event(), true))
            return;

    // otherwise: stay with the human. No loot, no travel, no grind — ever.
    Player* master = ai->GetMaster();
    if (master && sServerFacade.GetDistance2d(bot, master) > 4.0f)
    {
        ai->DoSpecificAction("follow", Event(), true);
        ai->SetAIInternalUpdateDelay(NONCOMBAT_DELAY_MS);
        return;
    }

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
    return false;
}

void PartyExecutor::CombatTick(PlayerbotAI* ai, Player* bot)
{
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
        case CLASS_WARRIOR: acted = PlayerbotAI::IsTank(bot) ? TankWarriorTick(ai, bot, target) : false; break;
        case CLASS_ROGUE:   acted = RogueTick(ai, bot, target); break;
        case CLASS_MAGE:    acted = MageTick(ai, bot, target); break;
        case CLASS_PALADIN: acted = RetPaladinTick(ai, bot, target); break;
        default:            acted = false; break;
    }

    if (!acted)
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
}
