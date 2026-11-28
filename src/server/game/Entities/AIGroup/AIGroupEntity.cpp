/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AIGroupEntity.h"
#include "Containers.h"
#include "Creature.h"
#include "GameObject.h"
#include "Map.h"
#include "Random.h"
#include "AIGroupMgr.h"
#include "ObjectMgr.h"
#include "ByteBuffer.h"
#include <unordered_map>

namespace
{
    using AIGroupMap = std::unordered_multimap<ObjectGuid::LowType, std::unique_ptr<AIGroup>>;
    AIGroupMap AIGroupMaps;
}

static void TriggerSpawnEvents(WorldObject* unit, AIGroupScript* script)
{
    if (!unit)
        return;

    Creature* creature = unit->ToCreature();
    GameObject* gameObject = unit->ToGameObject();
    if (!creature && !gameObject)
        return;

    ObjectGuid::LowType spawnId = creature ? creature->GetSpawnId() : gameObject->GetSpawnId();
    if (!spawnId)
        return;

    AIGroupMemberType memberType = creature ? AIGroupMemberType::Creature : AIGroupMemberType::GameObject;
    AIGroupSpawnEventList const* spawnEvents = sAIGroupMgr->GetSpawnEventList(spawnId, memberType);
    if (!spawnEvents)
        return;

    for (SpawnEventListHolder const& event : *spawnEvents)
        if (event.EventType == EventTypes::Spawn && roll_chance_i(event.Chance))
            script->PerformActionSet(event.ActionSetId, unit->GetGUID());
}

AIGroup::AIGroup() { }

AIGroup* AIGroup::GetAIGroup(Creature* creature)
{
    if (!creature)
        return nullptr;

    if (AIGroup* group = GetAIGroupByMainMemberGuid(creature->GetMap(), creature->GetSpawnId(), AIGroupMemberType::Creature))
        return group;

    AIGroupMainMemberData mainMember = sAIGroupMgr->GetMainMember(creature->GetSpawnId(), AIGroupMemberType::Creature);
    return mainMember.Guid ? GetAIGroupByMainMemberGuid(creature->GetMap(), mainMember.Guid, mainMember.Type) : nullptr;
}

AIGroup* AIGroup::GetAIGroup(GameObject* gameObject)
{
    if (!gameObject)
        return nullptr;

    if (AIGroup* group = GetAIGroupByMainMemberGuid(gameObject->GetMap(), gameObject->GetSpawnId(), AIGroupMemberType::GameObject))
        return group;

    AIGroupMainMemberData mainMember = sAIGroupMgr->GetMainMember(gameObject->GetSpawnId(), AIGroupMemberType::GameObject);
    return mainMember.Guid ? GetAIGroupByMainMemberGuid(gameObject->GetMap(), mainMember.Guid, mainMember.Type) : nullptr;
}

AIGroup* AIGroup::GetAIGroupByMainMemberGuid(Map* map, ObjectGuid::LowType mainMemberGuid, AIGroupMemberType mainMemberType)
{
    if (!map)
        return nullptr;

    auto const [groupBegin, groupEnd] = AIGroupMaps.equal_range(mainMemberGuid);
    for (auto groupItr = groupBegin; groupItr != groupEnd; ++groupItr)
        if (groupItr->second->GetMap() == map && groupItr->second->GetMainMemberType() == mainMemberType)
            return groupItr->second.get();

    return nullptr;
}

void AIGroup::AddAIGroup(std::unique_ptr<AIGroup> group)
{
    AIGroupMaps.emplace(group->GetMainMemberGuid(), std::move(group));
}

void AIGroup::AddUnitToAIGroup(WorldObject* unit)
{
    if (!unit || !unit->GetMap())
        return;

    Creature* creature = unit->ToCreature();
    GameObject* gameObject = unit->ToGameObject();
    if (!creature && !gameObject)
        return;

    ObjectGuid::LowType spawnId = creature ? creature->GetSpawnId() : gameObject->GetSpawnId();
    if (!spawnId)
        return;

    AIGroupMemberType memberType = creature ? AIGroupMemberType::Creature : AIGroupMemberType::GameObject;
    AIGroupMainMemberData mainMember = sAIGroupMgr->GetMainMember(spawnId, memberType);
    if (!mainMember.Guid)
        mainMember = { spawnId, memberType };

    if (AIGroup* group = GetAIGroupByMainMemberGuid(unit->GetMap(), mainMember.Guid, mainMember.Type))
    {
        group->GetScript()->SetActor(unit);
        TriggerSpawnEvents(unit, group->GetScript());
        return;
    }

    bool hasAIGroupAI = (creature && creature->GetAIName() == "AIGroup") || (gameObject && gameObject->GetAIName() == "AIGroup");
    if (!sAIGroupMgr->HasGroupMembers(spawnId, memberType) && !hasAIGroupAI && (!creature || !creature->GetNativeTriggersId()))
        return;

    auto group = std::make_unique<AIGroup>();
    if (!group->Create(unit))
        return;

    group->GetScript()->SetActor(unit);
    TriggerSpawnEvents(unit, group->GetScript());
    AddAIGroup(std::move(group));
}

void AIGroup::RemoveUnitFromAIGroup(WorldObject* unit)
{
    if (!unit || !unit->FindMap())
        return;

    ObjectGuid::LowType spawnId = 0;
    AIGroupMemberType memberType;
    Creature* creature = unit->ToCreature();
    if (creature)
    {
        spawnId = creature->GetSpawnId();
        memberType = AIGroupMemberType::Creature;
    }
    else if (GameObject* gameObject = unit->ToGameObject())
    {
        spawnId = gameObject->GetSpawnId();
        memberType = AIGroupMemberType::GameObject;
    }

    if (!spawnId)
        return;

    AIGroupMainMemberData mainMember = sAIGroupMgr->GetMainMember(spawnId, memberType);
    if (!mainMember.Guid)
        mainMember = { spawnId, memberType };

    auto [groupBegin, groupEnd] = AIGroupMaps.equal_range(mainMember.Guid);
    for (auto groupItr = groupBegin; groupItr != groupEnd; ++groupItr)
    {
        AIGroup* group = groupItr->second.get();
        if (group->GetMap() != unit->FindMap() || group->GetMainMemberType() != mainMember.Type)
            continue;

        bool hasRemainingMembers = false;
        if (group->GetMainMemberGuid() != spawnId || group->GetMainMemberType() != memberType)
        {
            if (group->GetMainMemberType() == AIGroupMemberType::Creature)
                hasRemainingMembers = group->GetMap()->GetCreatureBySpawnId(group->GetMainMemberGuid());
            else
                hasRemainingMembers = group->GetMap()->GetGameObjectBySpawnId(group->GetMainMemberGuid());
        }

        std::vector<AIGroupMemberData> const* memberData = sAIGroupMgr->GetGroupMembers(group->GetMainMemberGuid());
        if (memberData)
            for (AIGroupMemberData const& member : *memberData)
            {
                if (member.MemberType != group->GetMainMemberType())
                    continue;

                if (member.MemberGuid == spawnId && member.MemberType == memberType)
                    continue;

                if (member.MemberType == AIGroupMemberType::Creature && group->GetMap()->GetCreatureBySpawnId(member.MemberGuid))
                    hasRemainingMembers = true;
                else if (member.MemberType == AIGroupMemberType::GameObject && group->GetMap()->GetGameObjectBySpawnId(member.MemberGuid))
                    hasRemainingMembers = true;
            }

        if (!hasRemainingMembers)
        {
            AIGroupMaps.erase(groupItr);
            return;
        }
    }
}

bool AIGroup::Create(WorldObject* mainMember)
{
    if (!mainMember || !mainMember->GetMap())
        return false;

    Creature* creature = mainMember->ToCreature();
    GameObject* gameObject = mainMember->ToGameObject();
    if (!creature && !gameObject)
        return false;

    _mainMemberGuid = creature ? creature->GetSpawnId() : gameObject->GetSpawnId();
    if (!_mainMemberGuid)
        return false;

    _mainMemberType = creature ? AIGroupMemberType::Creature : AIGroupMemberType::GameObject;
    SetMap(mainMember->GetMap());
    _currentActionSet = 0;
    _script.SetActor(mainMember);
    _script.OnInitialize(this);
    return true;
}

void AIGroup::Update(uint32 diff)
{
    if (!GetMap())
        return;

    WorldObject* mainMember = _mainMemberType == AIGroupMemberType::Creature ?
        static_cast<WorldObject*>(GetMap()->GetCreatureBySpawnId(_mainMemberGuid)) :
        static_cast<WorldObject*>(GetMap()->GetGameObjectBySpawnId(_mainMemberGuid));
    if (mainMember)
    {
        _script.SetActor(mainMember);
        _script.OnUpdate(diff);
    }
}

void AIGroup::SetCurrentActionSet(uint32 actionSetId)
{
    _currentActionSet = actionSetId;
}

std::vector<WorldObject*> AIGroup::GetMembers(AIGroupUnit unit, AIGroupMemberType memberType) const
{
    std::vector<WorldObject*> members;
    std::vector<AIGroupMemberData> const* memberData = sAIGroupMgr->GetGroupMembers(_mainMemberGuid);
    if (!memberData)
        return members;

    for (AIGroupMemberData const& member : *memberData)
    {
        if (unit != AIGroupUnit::AllUnits && unit != AIGroupUnit::RandomUnit && member.Unit != unit)
            continue;

        if (member.MemberType != memberType)
            continue;

        if (memberType == AIGroupMemberType::Creature)
        {
            if (Creature* creature = GetMap()->GetCreatureBySpawnId(member.MemberGuid))
                members.push_back(creature);
        }
        else
        {
            if (GameObject* gameObject = GetMap()->GetGameObjectBySpawnId(member.MemberGuid))
                members.push_back(gameObject);
        }
    }

    if (unit == AIGroupUnit::RandomUnit)
        Trinity::Containers::RandomResize(members, 1);

    return members;
}
