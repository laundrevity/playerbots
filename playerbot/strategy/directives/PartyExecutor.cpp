#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/PartyExecutor.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/directives/DirectiveValues.h"

#include "Groups/Group.h"
#include "Util/Timer.h"

using namespace ai;

namespace
{
    // after a successful cast, come back quickly but don't spin
    constexpr uint32 AFTER_CAST_DELAY_MS = 500;
    constexpr uint32 IDLE_DELAY_MS = 250;
}

bool PartyExecutor::ShouldOwn(PlayerbotAI* ai, Player* bot)
{
    if (!sPlayerbotAIConfig.executorEnabled || !sPlayerbotAIConfig.directiveEnabled)
        return false;
    if (!ai->HasRealPlayerMaster())
        return false;
    if (bot->InBattleGround())
        return false;   // bg/arena keep the old brain (separately tuned)
    return bot->IsInCombat();
}

// One legality gate for every cast the executor makes: known spell, range,
// LoS, power, cooldown, GCD — all enforced by the module's cast plumbing.
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

// Rule 1: a cc assignment from the seam is a standing duty — apply it and
// keep it applied. Without an assignment a bot NEVER casts cc on its own.
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
        // already locked down (by anyone)? leave it alone
        if (ai->HasAura(assignment.spell, target))
            continue;
        if (Cast(ai, assignment.spell.c_str(), target))
            return true;
    }
    return false;
}

// Rule 2: interrupt reflex — my current target is mid-cast and my class has
// a kick: use it now, before any rotation thought.
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

// Rule 3 (tank only): anything attacking a non-tank party member is an
// emergency. Taunt it if taunt is up; otherwise switch to it immediately.
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
            continue;   // another tank has it: fine
        return attacker;
    }
    return nullptr;
}

// Rule 4: one explicit target priority — the human's skull, then the
// brain's kill order, then role default.
Unit* PartyExecutor::PickTarget(PlayerbotAI* ai, Player* bot)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    Group* group = bot->GetGroup();

    // 4a. skull mark (human intent always wins)
    if (group)
        if (Unit* skull = ai->GetUnit(ObjectGuid(group->GetTargetIcon(7))))
            if (!sServerFacade.UnitIsDead(skull) && bot->IsWithinDistInMap(skull, sPlayerbotAIConfig.sightDistance))
                return skull;

    // 4b. directive kill order (validated at acceptance, revalidated here)
    if (Unit* directed = GetDirectiveKillTarget(ai, context))
        return directed;

    // 4c. role default
    if (PlayerbotAI::IsTank(bot))
    {
        if (Unit* loose = LooseMobOnParty(ai, bot))
            return loose;
        Unit* current = bot->GetVictim();
        if (current && !sServerFacade.UnitIsDead(current))
            return current;
    }
    else if (group)
    {
        // dps assist the tank
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member != bot && member->IsInWorld() && PlayerbotAI::IsTank(member))
                if (Unit* tanked = member->GetVictim())
                    if (!sServerFacade.UnitIsDead(tanked))
                        return tanked;
        }
    }

    // 4d. nearest attacker
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

    // locomotion via the module's movement actuators
    if (!ranged && !bot->CanReachWithMeleeAttack(target))
        return ai->DoSpecificAction("reach melee", Event(), true);
    if (ranged && !bot->IsWithinDistInMap(target, 25.0f))
        return ai->DoSpecificAction("reach spell", Event(), true);
    return false;
}

// ---- class rotations: short, explicit, in priority order ----------------

bool PartyExecutor::TankWarriorTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (!ai->HasAura("defensive stance", bot) && Cast(ai, "defensive stance", bot))
        return true;

    // taunt duty: if my chosen target is beating on someone else, taunt
    if (target->GetVictim() && target->GetVictim() != bot && Cast(ai, "taunt", target))
        return true;

    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    uint32 meleeCount = 0;
    for (const ObjectGuid& guid : attackers)
        if (Unit* attacker = ai->GetUnit(guid))
            if (bot->CanReachWithMeleeAttack(attacker))
                ++meleeCount;

    if (meleeCount >= 2 && Cast(ai, "thunder clap", target))
        return true;
    if (Cast(ai, "shield slam", target))
        return true;
    if (Cast(ai, "revenge", target))
        return true;
    if (meleeCount >= 2 && Cast(ai, "cleave", target))
        return true;
    if (Cast(ai, "sunder armor", target))
        return true;
    if (bot->GetPower(POWER_RAGE) < 200 && Cast(ai, "bloodrage", bot))
        return true;
    return false;
}

bool PartyExecutor::RogueTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    if (bot->GetComboPoints() >= 4 && Cast(ai, "eviscerate", target))
        return true;
    if (Cast(ai, "sinister strike", target))
        return true;
    return false;
}

bool PartyExecutor::MageTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    // NB: no Polymorph here, ever — cc happens only via directive assignment
    if (Cast(ai, "fireball", target))
        return true;
    if (Cast(ai, "frostbolt", target))
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
    if (!ai->HasAura("seal of command", bot) && !ai->HasAura("seal of blood", bot) &&
        !ai->HasAura("seal of righteousness", bot))
    {
        if (Cast(ai, "seal of command", bot) || Cast(ai, "seal of blood", bot) ||
            Cast(ai, "seal of righteousness", bot))
            return true;
    }
    if (Cast(ai, "judgement", target))
        return true;
    if (Cast(ai, "crusader strike", target))
        return true;
    return false;
}

bool PartyExecutor::GenericMeleeTick(PlayerbotAI* ai, Player* bot, Unit* target)
{
    return false;   // auto-attack only until this class gets its rotation
}

void PartyExecutor::Tick(PlayerbotAI* ai, Player* bot)
{
    // 1. standing cc duty from the seam
    if (KeepCcApplied(ai, bot))
        return;

    // 2/3/4. reflexes + one target decision
    Unit* target = PickTarget(ai, bot);
    if (!target)
    {
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
        return;
    }

    if (TryInterrupt(ai, bot, target))
        return;

    EngageTarget(ai, bot, target);

    // 5. class rotation
    bool acted = false;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: acted = PlayerbotAI::IsTank(bot) ? TankWarriorTick(ai, bot, target)
                                                             : GenericMeleeTick(ai, bot, target); break;
        case CLASS_ROGUE:   acted = RogueTick(ai, bot, target); break;
        case CLASS_MAGE:    acted = MageTick(ai, bot, target); break;
        case CLASS_PALADIN: acted = RetPaladinTick(ai, bot, target); break;
        default:            acted = GenericMeleeTick(ai, bot, target); break;
    }

    if (!acted)
        ai->SetAIInternalUpdateDelay(IDLE_DELAY_MS);
}
