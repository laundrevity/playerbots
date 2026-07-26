#include "playerbot/playerbot.h"
#include "playerbot/strategy/directives/DungeonRoutePlanner.h"

#include "Maps/Map.h"
#include "playerbot/thirdparty/nlohmann/json.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

using namespace ai;
using nlohmann::json;

namespace
{
    struct RouteNode
    {
        std::string id;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float reach = 12.0f;
        uint32 bossEntry = 0;
        ObjectGuid bossGuid;
    };

    struct RoutePath
    {
        std::string id;
        float entryX = 0.0f;
        float entryY = 0.0f;
        float entryZ = 0.0f;
        std::vector<RouteNode> nodes;
    };

    struct RouteProgress
    {
        uint32 mapId = 0;
        size_t pathIndex = 0;
        size_t nodeIndex = 0;
    };

    std::map<uint32, std::vector<RoutePath>> s_routes;
    std::map<uint32, RouteProgress> s_progress;
    bool s_loaded = false;

    bool ReadPoint(const json& point, float& x, float& y, float& z)
    {
        if (!point.is_array() || point.size() != 3 ||
            !point[0].is_number() || !point[1].is_number() || !point[2].is_number())
            return false;
        x = point[0].get<float>();
        y = point[1].get<float>();
        z = point[2].get<float>();
        return true;
    }

    bool ResolveBoss(uint32 mapId, uint32 entry, RouteNode& node)
    {
        auto result = WorldDatabase.PQuery(
            "SELECT guid, position_x, position_y, position_z FROM creature "
            "WHERE id = %u AND map = %u LIMIT 1", entry, mapId);
        if (!result)
        {
            sLog.outError("DungeonRoutePlanner: map %u boss entry %u has no spawn", mapId, entry);
            return false;
        }

        Field* fields = result->Fetch();
        uint32 dbGuid = fields[0].GetUInt32();
        node.bossEntry = entry;
        node.bossGuid = ObjectGuid(HIGHGUID_UNIT, entry, dbGuid);
        node.x = fields[1].GetFloat();
        node.y = fields[2].GetFloat();
        node.z = fields[3].GetFloat();
        return true;
    }

    float DistanceSquared(Player* player, float x, float y, float z)
    {
        float dx = player->GetPositionX() - x;
        float dy = player->GetPositionY() - y;
        float dz = player->GetPositionZ() - z;
        return dx * dx + dy * dy + dz * dz;
    }
}

DungeonRoutePlanner& DungeonRoutePlanner::instance()
{
    static DungeonRoutePlanner planner;
    return planner;
}

void DungeonRoutePlanner::Load()
{
    if (s_loaded)
        return;
    s_loaded = true;

    std::ifstream in("../etc/party_routes.json");
    if (!in.is_open())
        in.open("party_routes.json");
    if (!in.is_open())
        return;

    json doc = json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.contains("routes") || !doc["routes"].is_object())
    {
        sLog.outError("DungeonRoutePlanner: party_routes.json is unparseable");
        return;
    }

    for (const auto& routeItem : doc["routes"].items())
    {
        uint32 mapId = uint32(std::strtoul(routeItem.key().c_str(), nullptr, 10));
        const json& route = routeItem.value();

        if (route.contains("paths") && route["paths"].is_array())
        {
            for (const json& pathNode : route["paths"])
            {
                if (!pathNode.is_object() || !pathNode.contains("nodes") ||
                    !pathNode["nodes"].is_array())
                    continue;

                RoutePath path;
                path.id = pathNode.value("id", "");
                if (path.id.empty() ||
                    !ReadPoint(pathNode.value("entry", json::array()),
                               path.entryX, path.entryY, path.entryZ))
                    continue;

                for (const json& rawNode : pathNode["nodes"])
                {
                    if (!rawNode.is_object())
                        continue;
                    RouteNode node;
                    node.id = rawNode.value("id", "");
                    node.reach = rawNode.value("reach", 12.0f);
                    bool valid = false;
                    if (rawNode.contains("boss") && rawNode["boss"].is_number_unsigned())
                        valid = ResolveBoss(mapId, rawNode["boss"].get<uint32>(), node);
                    else if (rawNode.contains("position"))
                        valid = ReadPoint(rawNode["position"], node.x, node.y, node.z);
                    if (valid && !node.id.empty())
                        path.nodes.push_back(node);
                }

                if (!path.nodes.empty())
                    s_routes[mapId].push_back(path);
            }
            continue;
        }

        // Compatibility for the existing route files outside Stratholme.
        if (!route.contains("bosses") || !route["bosses"].is_array())
            continue;
        RoutePath path;
        path.id = "legacy";
        for (const json& entryNode : route["bosses"])
        {
            if (!entryNode.is_number_unsigned())
                continue;
            RouteNode node;
            node.id = "boss-" + std::to_string(entryNode.get<uint32>());
            node.reach = 18.0f;
            if (!ResolveBoss(mapId, entryNode.get<uint32>(), node))
                continue;
            if (path.nodes.empty())
            {
                path.entryX = node.x;
                path.entryY = node.y;
                path.entryZ = node.z;
            }
            path.nodes.push_back(node);
        }
        if (!path.nodes.empty())
            s_routes[mapId].push_back(path);
    }

    for (const auto& route : s_routes)
        sLog.outBasic("DungeonRoutePlanner: loaded map %u with %u path(s)",
                      route.first, uint32(route.second.size()));
}

bool DungeonRoutePlanner::HasRoute(uint32 mapId)
{
    Load();
    auto route = s_routes.find(mapId);
    return route != s_routes.end() && !route->second.empty();
}

bool DungeonRoutePlanner::NextObjective(Player* tank, DungeonRouteObjective& objective,
                                        std::string& state)
{
    Load();
    if (!tank)
        return false;

    auto route = s_routes.find(tank->GetMapId());
    if (route == s_routes.end() || route->second.empty())
    {
        state = "no-route";
        return false;
    }

    uint32 counter = tank->GetObjectGuid().GetCounter();
    auto progressItr = s_progress.find(counter);
    if (progressItr == s_progress.end() || progressItr->second.mapId != tank->GetMapId())
    {
        RouteProgress progress;
        progress.mapId = tank->GetMapId();
        float closest = 0.0f;
        for (size_t i = 0; i < route->second.size(); ++i)
        {
            const RoutePath& path = route->second[i];
            float distance = DistanceSquared(tank, path.entryX, path.entryY, path.entryZ);
            if (i == 0 || distance < closest)
            {
                closest = distance;
                progress.pathIndex = i;
            }
        }
        progressItr = s_progress.insert(std::make_pair(counter, progress)).first;
        sLog.outBasic("DungeonRoutePlanner: %s selected path '%s'",
                      tank->GetName(), route->second[progress.pathIndex].id.c_str());
    }

    RouteProgress& progress = progressItr->second;
    if (progress.pathIndex >= route->second.size())
    {
        state = "invalid-progress";
        return false;
    }
    RoutePath& path = route->second[progress.pathIndex];
    Map* map = tank->GetMap();

    while (progress.nodeIndex < path.nodes.size())
    {
        RouteNode& node = path.nodes[progress.nodeIndex];
        Creature* boss = node.bossGuid && map ? map->GetCreature(node.bossGuid) : nullptr;
        if (boss && boss->IsDead())
        {
            ++progress.nodeIndex;
            continue;
        }

        float x = boss ? boss->GetPositionX() : node.x;
        float y = boss ? boss->GetPositionY() : node.y;
        float z = boss ? boss->GetPositionZ() : node.z;
        if (DistanceSquared(tank, x, y, z) <= node.reach * node.reach)
        {
            // Position nodes are complete on arrival. A missing boss at its
            // loaded spawn is already dead/despawned for this instance.
            if (!node.bossEntry || !boss)
            {
                ++progress.nodeIndex;
                continue;
            }
        }

        objective.pathId = path.id;
        objective.nodeId = node.id;
        objective.x = x;
        objective.y = y;
        objective.z = z;
        objective.reach = node.reach;
        objective.bossEntry = node.bossEntry;
        objective.bossGuid = node.bossGuid;
        state = "active";
        return true;
    }

    state = "route-complete";
    return false;
}

void DungeonRoutePlanner::Reset(Player* tank)
{
    if (tank)
        s_progress.erase(tank->GetObjectGuid().GetCounter());
}

void DungeonRoutePlanner::Reload()
{
    s_routes.clear();
    s_progress.clear();
    s_loaded = false;
}
