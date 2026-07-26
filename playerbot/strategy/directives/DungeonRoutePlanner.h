#pragma once

#include "Entities/ObjectGuid.h"

#include <string>

class Player;

namespace ai
{
    struct DungeonRouteObjective
    {
        std::string pathId;
        std::string nodeId;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float reach = 12.0f;
        uint32 bossEntry = 0;
        ObjectGuid bossGuid;
    };

    class DungeonRoutePlanner
    {
    public:
        static DungeonRoutePlanner& instance();

        bool HasRoute(uint32 mapId);
        bool NextObjective(Player* tank, DungeonRouteObjective& objective,
                           std::string& state);
        void Reset(Player* tank);
        void Reload();

    private:
        DungeonRoutePlanner() = default;
        void Load();
    };
}

#define sDungeonRoutePlanner ai::DungeonRoutePlanner::instance()
