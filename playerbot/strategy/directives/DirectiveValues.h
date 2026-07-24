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

    // Sticky pull pacing for the tank: "" (normal), "hold", or "fast". Set by
    // the pulling directive verb; deliberately outlives the directive TTL —
    // "stop pulling" means until countermanded, not for six seconds.
    class PullPolicyValue : public ManualSetValue<std::string>
    {
    public:
        PullPolicyValue(PlayerbotAI* ai, std::string name = "pull policy")
            : ManualSetValue<std::string>(ai, "", name) {}
    };

    // First still-valid kill-order entry, with the same legality checks the
    // rti (skull) targeting applies. Returns nullptr when the directive is
    // absent, expired, or none of its targets are currently attackable.
    Unit* GetDirectiveKillTarget(PlayerbotAI* ai, AiObjectContext* context);

    // Nearby group member whose blessing override names a blessing this
    // paladin has NOT put on them (a present-but-different blessing counts as
    // a mismatch — that's exactly the "kings plz while Might is up" case).
    Unit* FindBlessingOverrideMismatch(PlayerbotAI* ai, AiObjectContext* context, Player* bot);
}
