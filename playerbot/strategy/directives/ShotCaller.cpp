#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/ShotCaller.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/directives/DirectiveMgr.h"
#include "playerbot/thirdparty/nlohmann/json.hpp"

#include "Groups/Group.h"
#include "Util/Timer.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

using namespace ai;
using nlohmann::json;

namespace
{
    constexpr size_t MAX_QUEUE = 2;          // latest intent wins
    constexpr size_t MAX_TARGETS = 12;
    constexpr uint32 DIRECTIVE_TTL_MS = 6000;

    const char* MARK_NAMES[8] = {"star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull"};

    const char* SYSTEM_PROMPT =
        "You are the shot-caller for a party of World of Warcraft TBC (2.4.3) bots that play "
        "alongside one human. You receive the fight state and the human's party chat line, and "
        "you emit directives (desires with a TTL, the executor validates them) for the bots.\n"
        "Rules:\n"
        "- kill_order and cc targets use candidate_id values from the visible target list. "
        "Never invent an ID; use an empty kill_order when no listed target fits.\n"
        "- Obey the human's chat above everything else; otherwise apply real TBC judgment "
        "(dangerous totems and healers die first, a mob on the healer is an emergency for the tank, "
        "never pull a patrolling boss into an add fight, execute phase = burn).\n"
        "- Give each bot only what it needs; an empty kill_order means 'carry on as you were'.\n"
        "- cooldowns: hold = save everything, normal = standard, burn = use offensive cooldowns.\n"
        "- cc only when asked or clearly right (mage=Polymorph, rogue=Sap before combat).\n"
        "- if the human asks for a healthstone, set give_healthstone_to to the HUMAN'S name "
        "on the warlock bot's directive (omit the field otherwise).\n"
        "- if the human asks a paladin bot for a specific blessing ('kings on me', 'gimme "
        "wisdom'), set blessing {target, spell} on THAT PALADIN'S directive; target is who "
        "receives the buff (the human is a valid target here). It sticks until changed.\n"
        "- if the human asks for something NONE of your directive fields can express, say so "
        "plainly in the reply ('can't steer that yet') instead of agreeing.\n"
        "- directives may only name the LISTED BOTS — never the human.\n"
        "- sometimes you get a Situation line instead of chat: that is you noticing the fight "
        "state on your own — make the call unprompted.\n"
        "- dungeon Situations are emergencies (someone critical or dead, a huge pull): set "
        "cooldowns=burn when the fix is killing faster, and set use on the ONE bot whose "
        "defensive saves the moment: tank warrior = shield wall or last stand (or challenging "
        "shout when a pile of mobs is eating the party), rogue = evasion (or vanish to dump "
        "aggro), paladin = lay on hands to save a dying ally, hunter = feign death, druid = "
        "barkskin, mage = ice block. Only a spell that bot's class has.\n"
        "- use fires ONCE, immediately — omit it unless the moment needs it RIGHT NOW.\n"
        "- come_to_me: set true on EVERY bot when the human wants the party AT their "
        "position ('come to me', 'get inside', 'everyone in here'). The executor keeps "
        "them moving until they truly stand beside the human with line of sight — never "
        "answer such a request with words alone.\n"
        "- pulling goes on the TANK's directive and STICKS until changed: hold = park/follow "
        "without pulling ('stop pulling', 'hold', 'afk', 'brb', 'need mana'); steer = bypass "
        "the boss route and pull strictly where the human faces ('wrong way', 'turn around', "
        "'this way', 'follow me', 'let me lead'); fast = chain-pull with looser hp/mana waits; "
        "normal = resume the configured boss route. Never claim a direction change without "
        "setting pulling=steer. Acknowledge the change in the reply.\n"
        "- reply: ONE short casual party-chat answer, like a terse guildmate (max 12 words, "
        "no roleplay flourishes, no emoji).";

    bool SameNoCase(const char* a, const char* b)
    {
        while (*a && *b)
        {
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
            ++a;
            ++b;
        }
        return !*a && !*b;
    }

    struct LocalPullIntent
    {
        const char* policy = nullptr;
        const char* reply = nullptr;
    };

    LocalPullIntent ParseLocalPullIntent(const std::string& text)
    {
        std::string lower;
        lower.reserve(text.size());
        for (char c : text)
            lower += char(std::tolower(static_cast<unsigned char>(c)));

        auto has = [&lower](const char* phrase) {
            return lower.find(phrase) != std::string::npos;
        };

        if (has("wrong way") || has("other way") || has("turn around") ||
            has("this way") || has("follow me") || has("follow my lead") ||
            has("let me lead") || has("not crusader") || has("tank steer"))
            return {"steer", "Following your lead."};

        // spatial recall: a true movement intent, not a policy — the party
        // moves to the human until 3D proximity AND line of sight hold
        if (has("come to me") || has("come here") || has("come inside") ||
            has("in here") || has("get in here") || has("everyone in"))
            return {"come", "Coming to you."};

        if (has("come back") || has("tank hold"))
            return {"hold", "Coming back and holding."};

        if (has("resume route") || has("take point") || has("you lead") ||
            has("tank route") || has("route on"))
            return {"normal", "Taking the route again."};

        return {};
    }

    bool DispatchLocalPullIntent(Player* master, const std::string& text, uint32 now)
    {
        LocalPullIntent intent = ParseLocalPullIntent(text);
        Group* group = master ? master->GetGroup() : nullptr;
        if (!intent.policy || !group)
            return false;

        uint32 dispatched = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->IsInWorld() || !member->GetPlayerbotAI())
                continue;

            json directive = {
                {"v", 0},
                {"id", "local-pull-" + std::to_string(now) + "-" + std::to_string(dispatched)},
                {"src", "script"},
                {"ttl_ms", 1500},
                {"kill_order", json::array()},
            };
            if (strcmp(intent.policy, "come") == 0)
                directive["come_to_me"] = true;   // movement intent, not a pace
            else
                directive["pulling"] = intent.policy;
            if (dispatched == 0)
                directive["chat"] = {{"say", intent.reply}};
            sDirectiveMgr.Push(member->GetObjectGuid(), directive.dump(), DirectiveSource::Script,
                               ObjectGuid());
            ++dispatched;
        }

        if (dispatched)
            sLog.outBasic("ShotCaller: local pull intent '%s' -> %u directive(s)",
                          intent.policy, dispatched);
        return dispatched != 0;
    }

    std::string ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR: return "warrior";
            case CLASS_PALADIN: return "paladin";
            case CLASS_HUNTER: return "hunter";
            case CLASS_ROGUE: return "rogue";
            case CLASS_PRIEST: return "priest";
            case CLASS_SHAMAN: return "shaman";
            case CLASS_MAGE: return "mage";
            case CLASS_WARLOCK: return "warlock";
            case CLASS_DRUID: return "druid";
            default: return "unknown";
        }
    }

    std::string RoleName(Player* bot)
    {
        if (PlayerbotAI::IsTank(bot)) return "TANK";
        if (PlayerbotAI::IsHeal(bot)) return "HEALER";
        PlayerbotAI* botAi = bot->GetPlayerbotAI();
        return (botAi && botAi->IsRanged(bot)) ? "RANGED DPS" : "MELEE DPS";
    }

    json ResponseSchema(const std::vector<std::string>& botNames,
                        const std::vector<std::string>& targetIds,
                        const std::vector<std::string>& partyNames)
    {
        std::vector<std::string> boundedTargets = targetIds;
        if (boundedTargets.empty())
            boundedTargets.push_back("no-targets-available");
        json target = {
            {"type", "object"},
            {"properties",
             {{"candidate_id", {{"type", "string"}, {"enum", boundedTargets}}}}},
            {"required", json::array({"candidate_id"})},
        };
        json partyTarget = {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"enum", partyNames}}}}},
            {"required", json::array({"name"})},
        };
        json directive = {
            {"type", "object"},
            {"properties",
             {{"bot", {{"type", "string"}, {"enum", botNames}}},
              {"kill_order", {{"type", "array"}, {"items", target}, {"maxItems", 4}}},
              {"give_healthstone_to", {{"type", "string"}, {"enum", partyNames}}},
              {"blessing",
               {{"type", "object"},
                {"properties",
                 {{"target", partyTarget},
                  {"spell", {{"type", "string"},
                             {"enum", json::array({"kings", "might", "wisdom", "light", "salvation", "sanctuary"})}}}}},
                {"required", json::array({"target", "spell"})}}},
              {"use", {{"type", "string"},
                       {"enum", json::array({"shield wall", "last stand", "shield block", "evasion",
                                             "vanish", "feign death", "divine protection", "divine shield",
                                             "lay on hands", "barkskin", "frenzied regeneration", "ice block"})}}},
              {"cooldowns", {{"type", "string"}, {"enum", json::array({"hold", "normal", "burn"})}}},
              {"pulling", {{"type", "string"}, {"enum", json::array({"hold", "normal", "fast", "steer"})}}},
              {"come_to_me", {{"type", "boolean"}}},
              {"cc",
               {{"type", "array"},
                {"items",
                 {{"type", "object"},
                  {"properties", {{"target", target}, {"spell", {{"type", "string"}}}}},
                  {"required", json::array({"target"})}}}}}}},
            {"required", json::array({"bot", "kill_order", "cooldowns"})}};
        return {{"type", "object"},
                {"properties",
                 {{"directives", {{"type", "array"}, {"items", directive}}},
                  {"reply", {{"type", "string"}}}}},
                {"required", json::array({"directives", "reply"})}};    // silent calls read as bugs
    }
}

ShotCaller& ShotCaller::instance()
{
    static ShotCaller caller;
    return caller;
}

void ShotCaller::OnPartyChat(Player* master, uint32 type, const std::string& text)
{
    if (!sPlayerbotAIConfig.shotCallerEnabled || !sPlayerbotAIConfig.directiveEnabled)
        return;

    if (type != CHAT_MSG_PARTY && type != CHAT_MSG_RAID && type != CHAT_MSG_RAID_LEADER)
        return;

    if (!master || text.empty() || text.size() > 400)
        return;

    // every bot in the party relays the same line: one call only
    uint32 now = WorldTimer::getMSTime();
    if (master->GetObjectGuid() == m_lastMaster && text == m_lastText &&
        WorldTimer::getMSTimeDiff(m_lastMs, now) < 3000)
        return;
    m_lastMaster = master->GetObjectGuid();
    m_lastText = text;
    m_lastMs = now;

    // Navigation is a control-plane action, so common phrases take a fast,
    // deterministic path and cannot be lost to inference latency or schema
    // failure. The generated directives still use the same validated seam.
    if (DispatchLocalPullIntent(master, text, now))
        return;

    Submit(master, text, false);
}

void ShotCaller::ArenaTick(PlayerbotAI* ai, Player* bot)
{
    if (!sPlayerbotAIConfig.shotCallerEnabled || !sPlayerbotAIConfig.directiveEnabled)
        return;
    // autonomous ("Situation") calls run in arenas and in mastered dungeon
    // runs — chat-driven calls have no such gate
    bool arena = bot->InArena();
    if (!arena && (bot->InBattleGround() || !bot->GetMap()->IsDungeon()))
        return;

    Player* master = ai->GetMaster();
    if (!master || master == bot || !master->IsInWorld())
        return;

    constexpr uint32 AUTO_CALL_GAP_MS = 8000;       // one autonomous call per gap
    constexpr uint32 AUTO_RECALL_GAP_MS = 20000;    // same subject not re-called sooner
    constexpr uint32 SUBJECT_PACK = 0xFFFFFFF0;     // sentinel: big-pull trigger (no guid counter up here)

    uint32 now = WorldTimer::getMSTime();
    uint32 last = m_lastAutoMs.load();
    if (last && WorldTimer::getMSTimeDiff(last, now) < AUTO_CALL_GAP_MS)
        return;

    // edge triggers, most urgent first
    std::ostringstream reason;
    uint32 subject = 0;
    uint32 recallGapMs = AUTO_RECALL_GAP_MS;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (arena)
    {
        // arena: a kill window, then an ally folding
        std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
        for (const ObjectGuid& guid : possible)
        {
            Unit* enemy = ai->GetUnit(guid);
            if (!enemy || !enemy->IsPlayer() || !enemy->IsAlive())
                continue;
            if (enemy->GetHealthPercent() < 35.0f)
            {
                reason << "Enemy " << enemy->GetName() << " is at " << uint32(enemy->GetHealthPercent())
                       << "% hp — kill window.";
                subject = guid.GetCounter();
                break;
            }
        }
        if (!subject)
        {
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->getSource();
                    if (!member || !member->IsInWorld() || !member->IsAlive())
                        continue;
                    if (member->GetHealthPercent() < 40.0f)
                    {
                        reason << "Our " << member->GetName() << " is at " << uint32(member->GetHealthPercent())
                               << "% hp and under pressure — call the response.";
                        subject = member->GetObjectGuid().GetCounter();
                        break;
                    }
                }
            }
        }
    }
    else if (bot->IsInCombat())
    {
        // dungeon "oh shit" detection: a death mid-fight, someone critical,
        // or a pull too big for the normal rotation
        Group* group = bot->GetGroup();
        if (!group)
            return;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr && !subject; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->IsInWorld())
                continue;
            if (!member->IsAlive())
            {
                reason << "Situation: " << member->GetName()
                       << " just DIED mid-fight — call the recovery (burn, defensives, retarget).";
                subject = member->GetObjectGuid().GetCounter();
                recallGapMs = 60000;    // a corpse stays down; don't re-call it every 20s
            }
            else if (member->GetHealthPercent() < 30.0f)
            {
                reason << "Situation: " << member->GetName() << " is at "
                       << uint32(member->GetHealthPercent())
                       << "% hp and falling — emergency, call the save.";
                subject = member->GetObjectGuid().GetCounter();
            }
        }
        if (!subject)
        {
            std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("attackers")->Get();
            if (attackers.size() >= 5)
            {
                reason << "Situation: big pull — " << uint32(attackers.size())
                       << " mobs on the party. Call cooldowns and the kill order.";
                subject = SUBJECT_PACK;
                recallGapMs = 45000;
            }
        }
    }
    if (!subject)
        return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint32& stamp = m_recentAutoCalls[subject];
        if (stamp && WorldTimer::getMSTimeDiff(stamp, now) < recallGapMs)
            return;
        stamp = now;
    }
    m_lastAutoMs.store(now);

    Submit(master, reason.str(), true);
}

void ShotCaller::Submit(Player* master, const std::string& text, bool synthetic)
{
    Group* group = master->GetGroup();
    if (!group)
        return;

    // ---- snapshot on the world thread: strings only cross to the worker ----
    Job job;
    job.masterName = master->GetName();
    job.partyNames.push_back(job.masterName);

    std::ostringstream roster;
    std::vector<Player*> bots;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsInWorld())
            continue;
        if (std::find(job.partyNames.begin(), job.partyNames.end(), member->GetName()) ==
            job.partyNames.end())
            job.partyNames.push_back(member->GetName());
        if (!member->GetPlayerbotAI())
            continue;   // humans are not directed
        bots.push_back(member);
        job.bots.emplace_back(member->GetObjectGuid(), member->GetName());
        roster << member->GetName() << " (" << ClassName(member->getClass()) << ", "
               << RoleName(member) << ", " << uint32(member->GetHealthPercent()) << "% hp)\n";
    }
    if (bots.empty())
        return;

    // visible targets: union of the bots' possible targets, marks attached
    std::ostringstream targets;
    std::vector<ObjectGuid> seen;
    uint32 count = 0;
    for (Player* bot : bots)
    {
        if (count >= MAX_TARGETS)
            break;
        AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
        std::list<ObjectGuid> possible = context->GetValue<std::list<ObjectGuid>>("possible targets")->Get();
        for (const ObjectGuid& guid : possible)
        {
            if (count >= MAX_TARGETS)
                break;
            if (std::find(seen.begin(), seen.end(), guid) != seen.end())
                continue;
            Unit* unit = bot->GetPlayerbotAI()->GetUnit(guid);
            if (!unit || !unit->IsAlive())
                continue;
            seen.push_back(guid);
            std::string id = "t" + std::to_string(count);
            job.targets.push_back({id, guid, unit->GetName()});
            targets << id << " | " << unit->GetName() << " ("
                    << uint32(unit->GetHealthPercent()) << "% hp";
            for (int icon = 0; icon < 8; ++icon)
                if (ObjectGuid(group->GetTargetIcon(icon)) == guid)
                    targets << ", marked " << MARK_NAMES[icon];
            if (Unit* victim = unit->GetVictim())
                targets << ", attacking " << victim->GetName();
            targets << ")\n";
            ++count;
        }
    }

    std::ostringstream prompt;
    prompt << "Party bots:\n" << roster.str();
    prompt << "Human: " << job.masterName << " (" << ClassName(master->getClass()) << ", healer)\n";
    prompt << "Visible targets:\n" << (count ? targets.str() : "(none)\n");
    if (master->InArena())
        prompt << "Context: RATED ARENA — the listed targets are enemy players. Call kill-target "
                  "swaps, cc and cooldown burns; killing any one enemy usually wins the match.\n";
    if (synthetic)
        prompt << "Situation (no chat from the human — you noticed this yourself): \"" << text << "\"\n";
    else
        prompt << "Party chat from " << job.masterName << ": \"" << text << "\"\n";
    prompt << "Emit directives for the bots (skip bots that should just carry on) and a reply.";
    job.userPrompt = prompt.str();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (m_queue.size() >= MAX_QUEUE)
            m_queue.pop_front();
        m_queue.push_back(std::move(job));
        EnsureWorker();
    }
    m_wake.notify_one();
}

void ShotCaller::EnsureWorker()
{
    if (m_workerStarted)
        return;
    m_workerStarted = true;
    std::thread(&ShotCaller::WorkerLoop, this).detach();
}

void ShotCaller::WorkerLoop()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return !m_queue.empty(); });
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        ProcessJob(job);
    }
}

void ShotCaller::ProcessJob(const Job& job)
{
    std::vector<std::string> botNames;
    for (const auto& bot : job.bots)
        botNames.push_back(bot.second);
    std::vector<std::string> targetIds;
    for (const TargetCandidate& target : job.targets)
        targetIds.push_back(target.id);

    json body = {
        {"messages",
         {{{"role", "system"}, {"content", SYSTEM_PROMPT}},
          {{"role", "user"}, {"content", job.userPrompt}}}},
        {"response_format",
         {{"type", "json_schema"},
          {"json_schema",
           {{"name", "shotcall"},
            {"schema", ResponseSchema(botNames, targetIds, job.partyNames)}}}}},
        {"max_tokens", 400},
        {"temperature", 0.3},
        {"top_p", 0.8},
    };

    uint32 started = WorldTimer::getMSTime();
    std::string error;
    std::string response = HttpPostLocal(sPlayerbotAIConfig.shotCallerHost, sPlayerbotAIConfig.shotCallerPort,
                                         "/v1/chat/completions", body.dump(),
                                         sPlayerbotAIConfig.shotCallerTimeoutMs, error);
    uint32 latency = WorldTimer::getMSTimeDiff(started, WorldTimer::getMSTime());

    if (response.empty())
    {
        sLog.outError("ShotCaller: llm call failed after %ums: %s", latency, error.c_str());
        return;
    }

    json payload = json::parse(response, nullptr, false);
    if (payload.is_discarded() || !payload.contains("choices") || payload["choices"].empty())
    {
        sLog.outError("ShotCaller: malformed llm response after %ums", latency);
        return;
    }

    json content = json::parse(payload["choices"][0]["message"].value("content", ""), nullptr, false);
    if (content.is_discarded() || !content.is_object() || !content.contains("directives"))
    {
        sLog.outError("ShotCaller: llm content did not match schema (%ums)", latency);
        return;
    }

    std::string reply = content.value("reply", "");
    if (reply.size() > 140)
        reply.resize(140);
    for (char& c : reply)
        if (static_cast<unsigned char>(c) < 0x20)
            c = ' ';

    auto resolveTarget = [&job](const json& target, json& resolved) {
        if (!target.is_object())
            return false;
        std::string id = target.value("candidate_id", "");
        for (const TargetCandidate& candidate : job.targets)
        {
            if (candidate.id != id)
                continue;
            std::ostringstream guid;
            guid << "0x" << std::hex << candidate.guid.GetRawValue();
            resolved = {{"guid", guid.str()}, {"name", candidate.name}};
            return true;
        }
        return false;
    };

    uint32 dispatched = 0;
    uint32 mappedTargets = 0;
    uint32 rejectedTargets = 0;
    for (const json& entry : content["directives"])
    {
        if (!entry.is_object() || !entry.contains("bot"))
            continue;
        std::string botName = entry.value("bot", "");
        ObjectGuid botGuid;
        for (const auto& candidate : job.bots)
            if (SameNoCase(candidate.second.c_str(), botName.c_str()))
                botGuid = candidate.first;
        if (!botGuid)
        {
            sLog.outBasic("ShotCaller: directive for unknown bot '%s' skipped", botName.c_str());
            continue;
        }

        json killOrder = json::array();
        for (const json& requested : entry.value("kill_order", json::array()))
        {
            json resolved;
            if (resolveTarget(requested, resolved))
            {
                killOrder.push_back(resolved);
                ++mappedTargets;
            }
            else
                ++rejectedTargets;
        }

        json directive = {
            {"v", 0},
            {"id", "llm-" + std::to_string(WorldTimer::getMSTime()) + "-" + std::to_string(dispatched)},
            {"src", "llm"},
            {"ttl_ms", DIRECTIVE_TTL_MS},
            {"kill_order", killOrder},
            {"cooldowns", entry.value("cooldowns", "normal")},
        };
        if (entry.contains("give_healthstone_to") && entry["give_healthstone_to"].is_string() &&
            !entry["give_healthstone_to"].get<std::string>().empty())
            directive["give_healthstone_to"] = entry["give_healthstone_to"];

        if (entry.contains("blessing") && entry["blessing"].is_object())
            directive["blessing"] = entry["blessing"];
        if (entry.contains("use") && entry["use"].is_string() &&
            !entry["use"].get<std::string>().empty())
            directive["use"] = entry["use"];
        if (entry.contains("pulling") && entry["pulling"].is_string() &&
            !entry["pulling"].get<std::string>().empty())
            directive["pulling"] = entry["pulling"];
        if (entry.contains("come_to_me") && entry["come_to_me"].is_boolean() &&
            entry["come_to_me"].get<bool>())
            directive["come_to_me"] = true;
        if (entry.contains("cc") && entry["cc"].is_array())
        {
            json cc = json::array();
            for (const json& assignment : entry["cc"])
            {
                if (!assignment.is_object() || !assignment.contains("target"))
                    continue;
                json resolved;
                if (!resolveTarget(assignment["target"], resolved))
                {
                    ++rejectedTargets;
                    continue;
                }
                json mapped = assignment;
                mapped["target"] = resolved;
                cc.push_back(mapped);
                ++mappedTargets;
            }
            directive["cc"] = cc;
        }
        if (dispatched == 0 && !reply.empty())
            directive["chat"] = {{"say", reply}};

        sDirectiveMgr.Push(botGuid, directive.dump(), DirectiveSource::Llm, ObjectGuid());
        ++dispatched;
    }

    sLog.outBasic("ShotCaller: '%s' candidates=%u mapped=%u rejected=%u -> %u directive(s), reply='%s', %ums",
                  job.masterName.c_str(), uint32(job.targets.size()), mappedTargets,
                  rejectedTargets, dispatched, reply.c_str(), latency);
}

std::string ShotCaller::HttpPostLocal(const std::string& host, uint32 port, const std::string& path,
                                      const std::string& body, uint32 timeoutMs, std::string& error)
{
#ifdef _WIN32
    error = "shot-caller http client not implemented on windows";
    return "";
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        error = "socket() failed";
        return "";
    }

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        error = "bad host (ipv4 only): " + host;
        close(sock);
        return "";
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        error = "connect failed (is llama-server up?)";
        close(sock);
        return "";
    }

    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\nHost: " << host << "\r\n"
            << "Content-Type: application/json\r\nContent-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n" << body;
    std::string data = request.str();

    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0)
        {
            error = "send failed";
            close(sock);
            return "";
        }
        sent += size_t(n);
    }

    std::string raw;
    char buffer[8192];
    while (true)
    {
        ssize_t n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0)
            break;
        raw.append(buffer, size_t(n));
        if (raw.size() > 1024 * 1024)
            break;
    }
    close(sock);

    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        error = "no http header terminator (timeout?)";
        return "";
    }
    return raw.substr(headerEnd + 4);
#endif
}
