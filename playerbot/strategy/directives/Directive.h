#pragma once

// Directive seam, schema v0 (bot-brains P2). A directive is a desire with a
// TTL, never a command: brains (script | stub | llm) emit them, the executor
// validates on the bot's own tick and may reject. See
// wow-tbc-local/docs/design/directive-seam.md.

#include "Entities/ObjectGuid.h"

#include <string>
#include <vector>

namespace ai
{
    enum class DirectiveSource { Script, Stub, Llm };

    enum class CooldownPolicy { Hold, Normal, Burn };

    struct DirectiveTargetRef
    {
        ObjectGuid guid;      // resolved from "guid" (hex string), may be empty
        std::string name;     // "name" fallback (LLM tier thinks in names)
    };

    struct DirectiveAnchor
    {
        bool hasCoords = false;
        std::string name;
        uint32 mapId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float radius = 5.0f;
    };

    struct Directive
    {
        bool valid = false;               // set on acceptance only
        std::string id;
        std::string plan;
        DirectiveSource src = DirectiveSource::Script;
        uint32 ttlMs = 0;
        uint32 expiresAtMs = 0;           // server-uptime ms, set on acceptance

        std::vector<DirectiveTargetRef> requestedKillOrder;   // as sent
        std::vector<ObjectGuid> killOrder;                    // validated subset

        struct CcAssignment
        {
            ObjectGuid guid;      // validated target
            std::string name;     // as sent (name fallback)
            std::string spell;    // e.g. "polymorph" — legality checked at cast time
        };
        std::vector<CcAssignment> requestedCc;
        std::vector<CcAssignment> cc;                         // validated subset
        DirectiveAnchor anchor;
        CooldownPolicy cooldowns = CooldownPolicy::Normal;
        uint32 ccCount = 0;               // parsed, not executed in P2
        bool hasRetreat = false;          // parsed, not executed in P2
        bool hadChat = false;
        std::string chatSay;              // spoken in party on acceptance (shot-caller reply)
        std::string giveHealthstoneTo;    // warlock: initiate trade with this member, stone rides the trade hook

        struct BlessingAssignment
        {
            std::string targetName;       // party member, matched case-insensitively
            std::string spell;            // short name: kings|might|wisdom|light|salvation|sanctuary
        };
        std::vector<BlessingAssignment> blessings;   // paladin: sticky per-target preference

        bool IsActiveNow(uint32 nowMs) const { return valid && nowMs < expiresAtMs; }
    };

    // Wire form -> unvalidated Directive. Game-state validation happens in
    // ApplyDirectiveAction on the bot's own tick.
    bool ParseDirective(const std::string& text, Directive& out, std::string& error);

    const char* DirectiveSourceTag(DirectiveSource src);

    // "blessing overrides" blackboard string: "name=spell;name=spell;" with
    // lowercase names. Sticky: outlives directive TTL by design (a buff wish
    // is not a 6-second desire).
    std::string GetBlessingOverride(const std::string& serialized, const char* targetName);
    void SetBlessingOverride(std::string& serialized, const std::string& targetName, const std::string& spell);
}
