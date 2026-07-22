#pragma once

#include "playerbot/strategy/Value.h"
#include "playerbot/strategy/directives/Directive.h"

namespace ai
{
    // The seam's blackboard slot: the bot's current accepted directive.
    // Readers must check IsActiveNow() — expiry means graceful fallback to
    // default behavior, never an error.
    class DirectiveValue : public ManualSetValue<Directive>
    {
    public:
        DirectiveValue(PlayerbotAI* ai, std::string name = "directive")
            : ManualSetValue<Directive>(ai, Directive(), name) {}

        virtual std::string Format() override;
    };

    // Sticky per-target blessing preferences for paladin bots, serialized as
    // "name=spell;..." (see GetBlessingOverride). Deliberately outlives the
    // directive TTL: a human's "kings on me" holds until countermanded.
    class BlessingOverridesValue : public ManualSetValue<std::string>
    {
    public:
        BlessingOverridesValue(PlayerbotAI* ai, std::string name = "blessing overrides")
            : ManualSetValue<std::string>(ai, "", name) {}
    };

    // First still-valid kill-order entry, with the same legality checks the
    // rti (skull) targeting applies. Returns nullptr when the directive is
    // absent, expired, or none of its targets are currently attackable.
    Unit* GetDirectiveKillTarget(PlayerbotAI* ai, AiObjectContext* context);
}
