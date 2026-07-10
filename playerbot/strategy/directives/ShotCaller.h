#pragma once

// The fast-tier shot-caller (bot-brains P6 fast tier, pulled forward).
//
// One LLM call per master party-chat line: the world thread snapshots the
// fight (party roster, visible targets with raid marks, the chat line) into
// plain strings, a worker thread POSTs to the local llama-server
// (constrained by a json_schema), and the resulting per-bot directives are
// pushed into the DirectiveMgr mailbox exactly like any other brain. The
// player never sees any of this: their interface is typing normal party
// chat. An optional short "reply" rides on the first bot's directive so the
// party answers like guildmates.

#include "Entities/ObjectGuid.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class Player;

namespace ai
{
    class ShotCaller
    {
    public:
        static ShotCaller& instance();

        // world thread only: snapshot + enqueue (cheap; no network). Called
        // once per receiving bot (the core hands party chat to each bot's
        // HandleCommand) — deduped internally to one LLM call per line.
        void OnPartyChat(Player* master, uint32 type, const std::string& text);

    private:
        ShotCaller() = default;

        struct Job
        {
            std::string userPrompt;
            std::vector<std::pair<ObjectGuid, std::string>> bots;   // guid, name
            std::string masterName;
        };

        void EnsureWorker();
        void WorkerLoop();
        void ProcessJob(const Job& job);
        static std::string HttpPostLocal(const std::string& host, uint32 port,
                                         const std::string& path, const std::string& body,
                                         uint32 timeoutMs, std::string& error);

        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::deque<Job> m_queue;
        bool m_workerStarted = false;

        // world-thread-only dedupe (every party bot relays the same line)
        ObjectGuid m_lastMaster;
        std::string m_lastText;
        uint32 m_lastMs = 0;
    };
}

#define sShotCaller ai::ShotCaller::instance()
