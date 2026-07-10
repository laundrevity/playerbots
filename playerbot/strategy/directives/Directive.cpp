#include "playerbot/strategy/directives/Directive.h"

#include "playerbot/thirdparty/nlohmann/json.hpp"

#include <cstdlib>

using nlohmann::json;

namespace
{
    std::string GetString(const json& j, const char* key, const std::string& def = "")
    {
        auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
    }

    int64_t GetInt(const json& j, const char* key, int64_t def = 0)
    {
        auto it = j.find(key);
        return (it != j.end() && it->is_number()) ? it->get<int64_t>() : def;
    }

    double GetDouble(const json& j, const char* key, double def = 0.0)
    {
        auto it = j.find(key);
        return (it != j.end() && it->is_number()) ? it->get<double>() : def;
    }

    ObjectGuid ParseGuid(const std::string& text)
    {
        if (text.size() < 3 || text.compare(0, 2, "0x") != 0)
            return ObjectGuid();
        return ObjectGuid(uint64(strtoull(text.c_str() + 2, nullptr, 16)));
    }
}

namespace ai
{

const char* DirectiveSourceTag(DirectiveSource src)
{
    switch (src)
    {
        case DirectiveSource::Stub: return "stub";
        case DirectiveSource::Llm: return "llm";
        default: return "script";
    }
}

bool ParseDirective(const std::string& text, Directive& out, std::string& error)
{
    if (text.size() > 8192)
    {
        error = "document too large";
        return false;
    }

    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        error = "malformed json";
        return false;
    }

    if (j.contains("v") && GetInt(j, "v", 0) != 0)
    {
        error = "unsupported schema version";
        return false;
    }

    out = Directive();
    out.id = GetString(j, "id");
    if (out.id.empty())
    {
        error = "missing id";
        return false;
    }
    out.plan = GetString(j, "plan");

    std::string src = GetString(j, "src", "script");
    out.src = src == "stub" ? DirectiveSource::Stub
            : src == "llm" ? DirectiveSource::Llm
            : DirectiveSource::Script;

    out.ttlMs = uint32(GetInt(j, "ttl_ms", 4000));

    auto killOrder = j.find("kill_order");
    if (killOrder != j.end() && killOrder->is_array())
    {
        for (const json& entry : *killOrder)
        {
            if (!entry.is_object())
                continue;
            DirectiveTargetRef ref;
            ref.guid = ParseGuid(GetString(entry, "guid"));
            ref.name = GetString(entry, "name");
            if (ref.guid || !ref.name.empty())
                out.requestedKillOrder.push_back(ref);
            if (out.requestedKillOrder.size() >= 8)
                break;
        }
    }

    auto anchor = j.find("anchor");
    if (anchor != j.end() && anchor->is_object())
    {
        out.anchor.name = GetString(*anchor, "name");
        if (anchor->contains("x") && anchor->contains("y") && anchor->contains("z") && anchor->contains("map"))
        {
            out.anchor.hasCoords = true;
            out.anchor.mapId = uint32(GetInt(*anchor, "map"));
            out.anchor.x = float(GetDouble(*anchor, "x"));
            out.anchor.y = float(GetDouble(*anchor, "y"));
            out.anchor.z = float(GetDouble(*anchor, "z"));
            out.anchor.radius = float(GetDouble(*anchor, "r", 5.0));
        }
    }

    std::string cooldowns = GetString(j, "cooldowns", "normal");
    out.cooldowns = cooldowns == "hold" ? CooldownPolicy::Hold
                  : cooldowns == "burn" ? CooldownPolicy::Burn
                  : CooldownPolicy::Normal;

    auto cc = j.find("cc");
    if (cc != j.end() && cc->is_array())
    {
        out.ccCount = uint32(cc->size());
        for (const json& entry : *cc)
        {
            if (!entry.is_object())
                continue;
            Directive::CcAssignment assignment;
            auto target = entry.find("target");
            if (target != entry.end() && target->is_object())
            {
                assignment.guid = ParseGuid(GetString(*target, "guid"));
                assignment.name = GetString(*target, "name");
            }
            assignment.spell = GetString(entry, "spell");
            if ((assignment.guid || !assignment.name.empty()) && !assignment.spell.empty())
                out.requestedCc.push_back(assignment);
            if (out.requestedCc.size() >= 4)
                break;
        }
    }

    auto retreat = j.find("retreat");
    out.hasRetreat = retreat != j.end() && retreat->is_object();

    auto chat = j.find("chat");
    out.hadChat = chat != j.end() && !chat->is_null();
    if (out.hadChat && chat->is_object())
    {
        out.chatSay = GetString(*chat, "say");
        if (out.chatSay.size() > 140)
            out.chatSay.resize(140);
    }

    return true;
}

}
