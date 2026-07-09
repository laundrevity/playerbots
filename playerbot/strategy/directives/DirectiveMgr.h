#pragma once

// Per-bot directive mailbox: the single ingress for every brain tier
// (script whisper, in-process stub, later the LLM client thread). Writers
// only enqueue raw JSON; nothing here touches game state. The bot's own
// ApplyDirectiveAction pops, parses and validates on its update tick.

#include "Entities/ObjectGuid.h"
#include "playerbot/strategy/directives/Directive.h"

#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace ai
{
    struct PendingDirective
    {
        std::string json;
        DirectiveSource source = DirectiveSource::Script;
        ObjectGuid requester;
    };

    class DirectiveMgr
    {
    public:
        static DirectiveMgr& instance()
        {
            static DirectiveMgr mgr;
            return mgr;
        }

        void Push(ObjectGuid bot, std::string json, DirectiveSource source, ObjectGuid requester)
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::deque<PendingDirective>& queue = queues[bot];
            if (queue.size() >= MAX_PENDING)
                queue.pop_front();
            queue.push_back(PendingDirective{std::move(json), source, requester});
        }

        bool Pop(ObjectGuid bot, PendingDirective& out)
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = queues.find(bot);
            if (it == queues.end() || it->second.empty())
                return false;
            out = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty())
                queues.erase(it);
            return true;
        }

        bool HasPending(ObjectGuid bot)
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = queues.find(bot);
            return it != queues.end() && !it->second.empty();
        }

    private:
        static constexpr size_t MAX_PENDING = 8;
        std::mutex mutex;
        std::map<ObjectGuid, std::deque<PendingDirective>> queues;
    };
}

#define sDirectiveMgr ai::DirectiveMgr::instance()
