#pragma once

#include "playerbot/strategy/actions/GenericActions.h"
#include "playerbot/strategy/directives/Directive.h"

namespace ai
{
    // Whisper/party ingress: `brain {json directive}` — pushes the raw JSON
    // into this bot's mailbox (source: script). Parsing and validation happen
    // on the bot's own tick in ApplyDirectiveAction.
    class BrainCommandAction : public ChatCommandAction
    {
    public:
        BrainCommandAction(PlayerbotAI* ai) : ChatCommandAction(ai, "brain") {}
        virtual bool Execute(Event& event) override;
    };

    // Executor-side consumption: pop -> parse -> validate against live game
    // state -> write the "directive" blackboard slot; accept/reject is
    // instrumented via CombatEventLog ("directive" events) and, for
    // script-sourced directives, whispered back to the master.
    class ApplyDirectiveAction : public Action
    {
    public:
        ApplyDirectiveAction(PlayerbotAI* ai) : Action(ai, "apply directive") {}
        virtual bool Execute(Event& event) override;

    private:
        void Report(const Directive& directive, bool accepted, const std::string& note);
    };

    // Mirror-mode stub brain: emits "keep attacking your current target" as a
    // directive so the full seam runs continuously with zero behavior change.
    class StubBrainEmitAction : public Action
    {
    public:
        StubBrainEmitAction(PlayerbotAI* ai) : Action(ai, "stub brain emit") {}
        virtual bool Execute(Event& event) override;
    };

    // "maintain" (whisper or /p): systematic character upkeep — reset and
    // refill the bot's OWN talent tree from the factory premades, apply the
    // per-spec enchant template to equipped gear, fill empty sockets, save.
    // Never rerolls equipment.
    class MaintainAction : public ChatCommandAction
    {
    public:
        MaintainAction(PlayerbotAI* ai) : ChatCommandAction(ai, "maintain") {}
        virtual bool Execute(Event& event) override;
    };
}
