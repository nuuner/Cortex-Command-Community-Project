#include "MovableMan.h"

#include "PrimitiveMan.h"
#include "PostProcessMan.h"
#include "PerformanceMan.h"
#include "PresetMan.h"
#include "AEmitter.h"
#include "AHuman.h"
#include "MOPixel.h"
#include "HeldDevice.h"
#include "SLTerrain.h"
#include "Controller.h"
#include "AtomGroup.h"
#include "Actor.h"
#include "ADoor.h"
#include "Atom.h"
#include "Scene.h"
#include "FrameMan.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "LuaMan.h"
#include "ThreadMan.h"
#include "DeterminismChecker.h"

#include <cstring>
#include <map>
#include <sstream>
#include "InputRecorder.h"
#include "System.h"

#include "tracy/Tracy.hpp"

#include <execution>

using namespace RTE;

/// Runs a function for each of the threaded Lua states, and waits for them all to be done.
/// The states normally get a thread each. Their scripts can read and change what the scripts of the other states are working on though, which then comes out differently from run to run,
/// so they are run one after the other instead when the simulation has to play out the same elsewhere.
/// @param luaStateCount How many threaded Lua states there are.
/// @param function The function to run, which takes the range of states to run for.
template <typename Function>
static void RunOnEachThreadedLuaState(size_t luaStateCount, Function&& function) {
	if (InputRecorder::IsActive()) {
		for (size_t luaState = 0; luaState < luaStateCount; ++luaState) {
			function(static_cast<int>(luaState), static_cast<int>(luaState) + 1);
		}
	} else {
		g_ThreadMan.GetPriorityThreadPool().parallelize_loop(luaStateCount, function).wait();
	}
}

AlarmEvent::AlarmEvent(const Vector& pos, int team, float range) :
    m_ScenePos(pos),
    m_Team((Activity::Teams)team),
    m_Range(range * g_FrameMan.GetPlayerScreenWidth() * 0.51F) {}

const std::string MovableMan::c_ClassName = "MovableMan";

// Comparison functor for sorting movable objects by their X position using STL's sort
struct MOXPosComparison {
	bool operator()(MovableObject* pRhs, MovableObject* pLhs) { return pRhs->GetPos().m_X < pLhs->GetPos().m_X; }
};

MovableMan::MovableMan() {
	Clear();
}

MovableMan::~MovableMan() {
	Destroy();
}

void MovableMan::Clear() {
	m_Actors.clear();
	m_ContiguousActorIDs.clear();
	m_Items.clear();
	m_Particles.clear();
	m_AddedActors.clear();
	m_AddedItems.clear();
	m_AddedParticles.clear();
	m_ValidActors.clear();
	m_ValidItems.clear();
	m_ValidParticles.clear();
	m_ActorRoster[Activity::TeamOne].clear();
	m_ActorRoster[Activity::TeamTwo].clear();
	m_ActorRoster[Activity::TeamThree].clear();
	m_ActorRoster[Activity::TeamFour].clear();
	m_SortTeamRoster[Activity::TeamOne] = false;
	m_SortTeamRoster[Activity::TeamTwo] = false;
	m_SortTeamRoster[Activity::TeamThree] = false;
	m_SortTeamRoster[Activity::TeamFour] = false;
	m_AddedAlarmEvents.clear();
	m_AlarmEvents.clear();
	m_MOIDIndex.clear();
	m_SplashRatio = 0.75;
	m_MaxDroppedItems = 100;
	m_SettlingEnabled = true;
	m_MOSubtractionEnabled = true;
	// HitWhatMOID / HitWhatTerrMaterial compare against this each tick; it's otherwise only incremented.
	m_SimUpdateFrameNumber = 0;
}

int MovableMan::Initialize() {
	// TODO: Increase this number, or maybe only for certain classes?
	Entity::ClassInfo::FillAllPools();

	return 0;
}

int MovableMan::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("AddEffect", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("AddAmmo", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("AddDevice", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("AddActor", { g_PresetMan.GetEntityPreset(reader); });
	MatchProperty("SplashRatio", { reader >> m_SplashRatio; });

	EndPropertyList;
}

int MovableMan::Save(Writer& writer) const {
	Serializable::Save(writer);

	writer << m_Actors.size();
	for (std::deque<Actor*>::const_iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr)
		writer << **itr;

	writer << m_Particles.size();
	for (std::deque<MovableObject*>::const_iterator itr2 = m_Particles.begin(); itr2 != m_Particles.end(); ++itr2)
		writer << **itr2;

	return 0;
}

void MovableMan::Destroy() {
	for (std::deque<Actor*>::iterator it1 = m_Actors.begin(); it1 != m_Actors.end(); ++it1)
		delete (*it1);
	for (std::deque<MovableObject*>::iterator it2 = m_Items.begin(); it2 != m_Items.end(); ++it2)
		delete (*it2);
	for (std::deque<MovableObject*>::iterator it3 = m_Particles.begin(); it3 != m_Particles.end(); ++it3)
		delete (*it3);
	for (std::vector<AlarmEvent*>::iterator it4 = m_AlarmEvents.begin(); it4 != m_AlarmEvents.end(); ++it4)
		delete (*it4);
	for (std::vector<AlarmEvent*>::iterator it5 = m_AddedAlarmEvents.begin(); it5 != m_AddedAlarmEvents.end(); ++it5)
		delete (*it5);

	Clear();
}

MovableObject* MovableMan::GetMOFromID(MOID whichID) {
	if (whichID != g_NoMOID && whichID != 0 && whichID < m_MOIDIndex.size()) {
		// This is really, really awful
		// But, Lua scripts can take ownership of an MO which exists in this list
		// And then the MO can be deallocated by Lua GC
		// Meaning that this ptr points to stale memory.
		// Due to our pooled memory system, we can avoid a crash by just... checking that the MO isn't NoMOID
		// But this is still atrociously awful and terrible and this can point to a newly-allocated object that just so happens to be allocated the same place.
		// Anyways, until we can fix the god-awful abomination that is this game's memory ownership semantics, we're stuck with this
		// Which is also technically undefined behaviour
		MovableObject* candidate = m_MOIDIndex[whichID];
		if (candidate->GetID() != whichID) {
			return nullptr;
		}

		return candidate;
	}
	return nullptr;
}

MOID MovableMan::GetMOIDPixel(int pixelX, int pixelY, const std::vector<int>& moidList) {
	// Note - We loop through the MOs in reverse to make sure that the topmost (last drawn) MO that overlaps the specified coordinates is the one returned.
	for (auto itr = moidList.rbegin(), itrEnd = moidList.rend(); itr < itrEnd; ++itr) {
		MOID moid = *itr;
		const MovableObject* mo = GetMOFromID(moid);

		// Commented... see MovableMan::GetMOFromID
		// RTEAssert(mo, "Null MO found in MOID list!");
		if (mo == nullptr) {
			continue;
		}

		if (mo->GetScale() == 0.0f) {
			return g_NoMOID;
		} else if (mo->HitTestAtPixel(pixelX, pixelY)) {
			return moid;
		}
	}

	return g_NoMOID;
}

void MovableMan::RegisterObject(MovableObject* mo) {
	if (!mo) {
		return;
	}

	std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
	m_KnownObjects[mo->GetUniqueID()] = mo;
}

void MovableMan::UnregisterObject(MovableObject* mo) {
	if (!mo) {
		return;
	}

	std::lock_guard<std::mutex> guard(m_ObjectRegisteredMutex);
	m_KnownObjects.erase(mo->GetUniqueID());
}

const std::vector<MovableObject*>* MovableMan::GetMOsInBox(const Box& box, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsInBox(box, ignoreTeam, getsHitByMOsOnly));
	return vectorForLua;
}

const std::vector<MovableObject*>* MovableMan::GetMOsInRadius(const Vector& centre, float radius, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsInRadius(centre, radius, ignoreTeam, getsHitByMOsOnly));
	return vectorForLua;
}

const std::vector<MovableObject*>* MovableMan::GetMOsAtPosition(int pixelX, int pixelY, int ignoreTeam, bool getsHitByMOsOnly) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();
	*vectorForLua = std::move(g_SceneMan.GetMOIDGrid().GetMOsAtPosition(pixelX, pixelY, ignoreTeam, getsHitByMOsOnly));
	return vectorForLua;
}

void MovableMan::PurgeAllMOs() {
	for (std::deque<Actor*>::iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr) {
		(*itr)->DestroyScriptState();
	}
	for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
		(*itr)->DestroyScriptState();
	}
	for (std::deque<MovableObject*>::iterator itr = m_Particles.begin(); itr != m_Particles.end(); ++itr) {
		(*itr)->DestroyScriptState();
	}

	for (std::deque<Actor*>::iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr) {
		delete (*itr);
	}
	for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
		delete (*itr);
	}
	for (std::deque<MovableObject*>::iterator itr = m_Particles.begin(); itr != m_Particles.end(); ++itr) {
		delete (*itr);
	}

	m_Actors.clear();
	m_Items.clear();
	m_Particles.clear();
	m_AddedActors.clear();
	m_AddedItems.clear();
	m_AddedParticles.clear();
	m_ValidActors.clear();
	m_ValidItems.clear();
	m_ValidParticles.clear();
	m_ActorRoster[Activity::TeamOne].clear();
	m_ActorRoster[Activity::TeamTwo].clear();
	m_ActorRoster[Activity::TeamThree].clear();
	m_ActorRoster[Activity::TeamFour].clear();
	m_SortTeamRoster[Activity::TeamOne] = false;
	m_SortTeamRoster[Activity::TeamTwo] = false;
	m_SortTeamRoster[Activity::TeamThree] = false;
	m_SortTeamRoster[Activity::TeamFour] = false;
	m_AddedAlarmEvents.clear();
	m_AlarmEvents.clear();
	m_MOIDIndex.clear();
	// We want to keep known objects around, 'cause these can exist even when not in the simulation (they're here from creation till deletion, regardless of whether they are in sim)
	// m_KnownObjects.clear();
}

Actor* MovableMan::GetNextActorInGroup(std::string group, Actor* pAfterThis) {
	if (group.empty())
		return 0;

	// Begin at the beginning
	std::deque<Actor*>::const_iterator aIt = m_Actors.begin();

	// Search for the actor to start search from, if specified
	if (pAfterThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_Actors.end() && !((*aIt)->IsInGroup(group) && *aIt == pAfterThis); ++aIt)
			;

		// If we couldn't find the one to search for,
		// then just start at the beginning again and get the first actor at the next step
		if (aIt == m_Actors.end())
			aIt = m_Actors.begin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else
			++aIt;
	}

	// Now search for the first actor of the team from the search point (beginning or otherwise)
	for (; aIt != m_Actors.end() && !(*aIt)->IsInGroup(group); ++aIt)
		;

	// If nothing found between a specified actor and the end,
	// then restart and see if there's anything between beginning and that specified actor
	if (pAfterThis && aIt == m_Actors.end()) {
		for (aIt = m_Actors.begin(); aIt != m_Actors.end() && !(*aIt)->IsInGroup(group); ++aIt)
			;

		// Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
		//        RTEAssert(aIt != m_Actors.end(), "Search for something after specified actor, and didn't even find the specified actor!?");
	}

	// Still nothing, so return nothing
	if (aIt == m_Actors.end())
		return 0;

	if ((*aIt)->IsInGroup(group))
		return *aIt;

	return 0;
}

Actor* MovableMan::GetPrevActorInGroup(std::string group, Actor* pBeforeThis) {
	if (group.empty())
		return 0;

	// Begin at the reverse beginning
	std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin();

	// Search for the actor to start search from, if specified
	if (pBeforeThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_Actors.rend() && !((*aIt)->IsInGroup(group) && *aIt == pBeforeThis); ++aIt)
			;

		// If we couldn't find the one to search for,
		// then just start at the beginning again and get the first actor at the next step
		if (aIt == m_Actors.rend())
			aIt = m_Actors.rbegin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else
			++aIt;
	}

	// Now search for the first actor of the team from the search point (beginning or otherwise)
	for (; aIt != m_Actors.rend() && !(*aIt)->IsInGroup(group); ++aIt)
		;

	// If nothing found between a specified actor and the end,
	// then restart and see if there's anything between beginning and that specified actor
	if (pBeforeThis && aIt == m_Actors.rend()) {
		for (aIt = m_Actors.rbegin(); aIt != m_Actors.rend() && !(*aIt)->IsInGroup(group); ++aIt)
			;

		// Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
		//        RTEAssert(aIt != m_Actors.rend(), "Search for something after specified actor, and didn't even find the specified actor!?");
	}

	// Still nothing, so return nothing
	if (aIt == m_Actors.rend())
		return 0;

	if ((*aIt)->IsInGroup(group))
		return *aIt;

	return 0;
}

Actor* MovableMan::GetNextTeamActor(int team, Actor* pAfterThis) {
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount || m_ActorRoster[team].empty())
		return 0;
	/*
	    // Begin at the beginning
	    std::deque<Actor *>::const_iterator aIt = m_Actors.begin();

	    // Search for the actor to start search from, if specified
	    if (pAfterThis)
	    {
	        // Make the iterator point to the specified starting point actor
	        for (; aIt != m_Actors.end() && !((*aIt)->GetTeam() == team && *aIt == pAfterThis); ++aIt)
	            ;

	        // If we couldn't find the one to search for,
	        // then just start at the beginning again and get the first actor at the next step
	        if (aIt == m_Actors.end())
	            aIt = m_Actors.begin();
	        // Go one more step so we're not pointing at the one we're not supposed to get
	        else
	            ++aIt;
	    }

	    // Now search for the first actor of the team from the search point (beginning or otherwise)
	    for (; aIt != m_Actors.end() && (*aIt)->GetTeam() != team; ++aIt)
	        ;

	    // If nothing found between a specified actor and the end,
	    // then restart and see if there's anything between beginning and that specified actor
	    if (pAfterThis && aIt == m_Actors.end())
	    {
	        for (aIt = m_Actors.begin(); aIt != m_Actors.end() && (*aIt)->GetTeam() != team; ++aIt)
	            ;

	        // Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
	//        RTEAssert(aIt != m_Actors.end(), "Search for something after specified actor, and didn't even find the specified actor!?");
	    }

	    // Still nothing, so return nothing
	    if (aIt == m_Actors.end())
	        return 0;

	    if ((*aIt)->GetTeam() == team)
	        return *aIt;

	    return 0;
	*/
	// First sort the roster
	m_ActorRoster[team].sort(MOXPosComparison());

	// Begin at the beginning
	std::list<Actor*>::const_iterator aIt = m_ActorRoster[team].begin();

	// Search for the actor to start search from, if specified
	if (pAfterThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_ActorRoster[team].end() && *aIt != pAfterThis; ++aIt)
			;

		// If we couldn't find the one to search for, then just return the first one
		if (aIt == m_ActorRoster[team].end())
			aIt = m_ActorRoster[team].begin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else {
			++aIt;
			// If that was the last one, then return the first in the list
			if (aIt == m_ActorRoster[team].end())
				aIt = m_ActorRoster[team].begin();
		}
	}

	RTEAssert((*aIt)->GetTeam() == team, "Actor of wrong team found in the wrong roster!");
	return *aIt;
}

Actor* MovableMan::GetPrevTeamActor(int team, Actor* pBeforeThis) {
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount || m_Actors.empty() || m_ActorRoster[team].empty())
		return 0;
	/* Obsolete, now uses team rosters which are sorted
	    // Begin at the reverse beginning
	    std::deque<Actor *>::const_reverse_iterator aIt = m_Actors.rbegin();

	    // Search for the actor to start search from, if specified
	    if (pBeforeThis)
	    {
	        // Make the iterator point to the specified starting point actor
	        for (; aIt != m_Actors.rend() && !((*aIt)->GetTeam() == team && *aIt == pBeforeThis); ++aIt)
	            ;

	        // If we couldn't find the one to search for,
	        // then just start at the beginning again and get the first actor at the next step
	        if (aIt == m_Actors.rend())
	            aIt = m_Actors.rbegin();
	        // Go one more step so we're not pointing at the one we're not supposed to get
	        else
	            ++aIt;
	    }

	    // Now search for the first actor of the team from the search point (beginning or otherwise)
	    for (; aIt != m_Actors.rend() && (*aIt)->GetTeam() != team; ++aIt)
	        ;

	    // If nothing found between a specified actor and the end,
	    // then restart and see if there's anything between beginning and that specified actor
	    if (pBeforeThis && aIt == m_Actors.rend())
	    {
	        for (aIt = m_Actors.rbegin(); aIt != m_Actors.rend() && (*aIt)->GetTeam() != team; ++aIt)
	            ;

	        // Still nothing?? Should at least get the specified actor and return it! - EDIT No becuase it just may not be there!
	//        RTEAssert(aIt != m_Actors.rend(), "Search for something after specified actor, and didn't even find the specified actor!?");
	    }

	    // Still nothing, so return nothing
	    if (aIt == m_Actors.rend())
	        return 0;

	    if ((*aIt)->GetTeam() == team)
	        return *aIt;

	    return 0;
	*/
	// First sort the roster
	m_ActorRoster[team].sort(MOXPosComparison());

	// Begin at the reverse beginning of roster
	std::list<Actor*>::reverse_iterator aIt = m_ActorRoster[team].rbegin();

	// Search for the actor to start search from, if specified
	if (pBeforeThis) {
		// Make the iterator point to the specified starting point actor
		for (; aIt != m_ActorRoster[team].rend() && *aIt != pBeforeThis; ++aIt)
			;

		// If we couldn't find the one to search for, then just return the one at the end
		if (aIt == m_ActorRoster[team].rend())
			aIt = m_ActorRoster[team].rbegin();
		// Go one more step so we're not pointing at the one we're not supposed to get
		else {
			++aIt;
			// If that was the first one, then return the last in the list
			if (aIt == m_ActorRoster[team].rend())
				aIt = m_ActorRoster[team].rbegin();
		}
	}

	RTEAssert((*aIt)->GetTeam() == team, "Actor of wrong team found in the wrong roster!");
	return *aIt;
}

Actor* MovableMan::GetClosestTeamActor(int team, int player, const Vector& scenePoint, int maxRadius, Vector& getDistance, bool onlyPlayerControllableActors, const Actor* excludeThis) {
	if (team < Activity::NoTeam || team >= Activity::MaxTeamCount || m_Actors.empty() || m_ActorRoster[team].empty())
		return 0;

	Activity* pActivity = g_ActivityMan.GetActivity();

	float sqrShortestDistance = static_cast<float>(maxRadius * maxRadius);
	Actor* pClosestActor = 0;

	// If we're looking for a noteam actor, then go through the entire actor list instead
	if (team == Activity::NoTeam) {
		for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
			if ((*aIt) == excludeThis || (*aIt)->GetTeam() != Activity::NoTeam || (onlyPlayerControllableActors && !(*aIt)->IsPlayerControllable())) {
				continue;
			}

			// Check if even within search radius
			float sqrDistance = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY()).GetSqrMagnitude();
			if (sqrDistance < sqrShortestDistance) {
				sqrShortestDistance = sqrDistance;
				pClosestActor = *aIt;
			}
		}
	}
	// A specific team, so use the rosters instead
	else {
		for (std::list<Actor*>::iterator aIt = m_ActorRoster[team].begin(); aIt != m_ActorRoster[team].end(); ++aIt) {
			if ((*aIt) == excludeThis || (onlyPlayerControllableActors && !(*aIt)->IsPlayerControllable()) || (player != NoPlayer && ((*aIt)->GetController()->IsPlayerControlled(player) || (pActivity && pActivity->IsOtherPlayerBrain(*aIt, player))))) {
				continue;
			}

			Vector distanceVec = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY());

			// Check if even within search radius
			float sqrDistance = distanceVec.GetSqrMagnitude();
			if (sqrDistance < sqrShortestDistance) {
				sqrShortestDistance = sqrDistance;
				pClosestActor = *aIt;
				getDistance.SetXY(distanceVec.GetX(), distanceVec.GetY());
			}
		}
	}

	return pClosestActor;
}

Actor* MovableMan::GetClosestEnemyActor(int team, const Vector& scenePoint, int maxRadius, Vector& getDistance) {
	if (team < Activity::NoTeam || team >= Activity::MaxTeamCount || m_Actors.empty())
		return 0;

	Activity* pActivity = g_ActivityMan.GetActivity();

	float sqrShortestDistance = static_cast<float>(maxRadius * maxRadius);
	Actor* pClosestActor = 0;

	for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
		if ((*aIt)->GetTeam() == team)
			continue;

		Vector distanceVec = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY());

		// Check if even within search radius
		float sqrDistance = distanceVec.GetSqrMagnitude();
		if (sqrDistance < sqrShortestDistance) {
			sqrShortestDistance = sqrDistance;
			pClosestActor = *aIt;
			getDistance.SetXY(distanceVec.GetX(), distanceVec.GetY());
		}
	}

	return pClosestActor;
}

Actor* MovableMan::GetClosestActor(const Vector& scenePoint, int maxRadius, Vector& getDistance, const Actor* pExcludeThis) {
	if (m_Actors.empty())
		return 0;

	Activity* pActivity = g_ActivityMan.GetActivity();

	float sqrShortestDistance = static_cast<float>(maxRadius * maxRadius);
	Actor* pClosestActor = 0;

	for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
		if ((*aIt) == pExcludeThis)
			continue;

		Vector distanceVec = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY());

		// Check if even within search radius
		float sqrDistance = distanceVec.GetSqrMagnitude();
		if (sqrDistance < sqrShortestDistance) {
			sqrShortestDistance = sqrDistance;
			pClosestActor = *aIt;
			getDistance.SetXY(distanceVec.GetX(), distanceVec.GetY());
		}
	}

	return pClosestActor;
}

Actor* MovableMan::GetClosestBrainActor(int team, const Vector& scenePoint) const {
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount || m_ActorRoster[team].empty())
		return 0;

	float sqrShortestDistance = std::numeric_limits<float>::infinity();
	sqrShortestDistance *= sqrShortestDistance;

	Actor* pClosestBrain = 0;

	for (std::list<Actor*>::const_iterator aIt = m_ActorRoster[team].begin(); aIt != m_ActorRoster[team].end(); ++aIt) {
		if (!(*aIt)->HasObjectInGroup("Brains"))
			continue;

		// Check if closer than best so far
		float sqrDistance = g_SceneMan.ShortestDistance((*aIt)->GetPos(), scenePoint, g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY()).GetSqrMagnitude();
		if (sqrDistance < sqrShortestDistance) {
			sqrShortestDistance = sqrDistance;
			pClosestBrain = *aIt;
		}
	}

	return pClosestBrain;
}

Actor* MovableMan::GetClosestOtherBrainActor(int notOfTeam, const Vector& scenePoint) const {
	if (notOfTeam < Activity::TeamOne || notOfTeam >= Activity::MaxTeamCount || m_Actors.empty())
		return 0;

	float sqrShortestDistance = std::numeric_limits<float>::infinity();
	sqrShortestDistance *= sqrShortestDistance;

	Actor* pClosestBrain = 0;
	Actor* pContenderBrain = 0;

	for (int t = Activity::TeamOne; t < g_ActivityMan.GetActivity()->GetTeamCount(); ++t) {
		if (t != notOfTeam) {
			pContenderBrain = GetClosestBrainActor(t, scenePoint);
			float sqrDistance = (pContenderBrain->GetPos() - scenePoint).GetSqrMagnitude();
			if (sqrDistance < sqrShortestDistance) {
				sqrShortestDistance = sqrDistance;
				pClosestBrain = pContenderBrain;
			}
		}
	}
	return pClosestBrain;
}

Actor* MovableMan::GetUnassignedBrain(int team) const {
	if (/*m_Actors.empty() || */ m_ActorRoster[team].empty())
		return 0;

	for (std::list<Actor*>::const_iterator aIt = m_ActorRoster[team].begin(); aIt != m_ActorRoster[team].end(); ++aIt) {
		if ((*aIt)->HasObjectInGroup("Brains") && !g_ActivityMan.GetActivity()->IsAssignedBrain(*aIt))
			return *aIt;
	}

	// Also need to look through all the actors added this frame, one might be a brain.
	int actorTeam = Activity::NoTeam;
	for (std::deque<Actor*>::const_iterator aaIt = m_AddedActors.begin(); aaIt != m_AddedActors.end(); ++aaIt) {
		int actorTeam = (*aaIt)->GetTeam();
		// Accept no-team brains too - ACTUALLY, DON'T
		if ((actorTeam == team /* || actorTeam == Activity::NoTeam*/) && (*aaIt)->HasObjectInGroup("Brains") && !g_ActivityMan.GetActivity()->IsAssignedBrain(*aaIt))
			return *aaIt;
	}

	return 0;
}

namespace {
	/// What an MO is compared by between machines: which of the lists it is in, in the order HashState goes through them, which MO it is, and a hash of all that is known about it.
	// shortcut: raw bytes, so both machines have to be of the same architecture. Give it a byte order once that isn't a given.
	#pragma pack(push, 1)
	struct SyncDigestEntry {
		uint8_t list;
		int64_t uniqueID;
		uint64_t hash;
	};
	#pragma pack(pop)

	constexpr uint8_t c_FirstNonActorSyncList = 2; //!< The first of the lists that hold other things than Actors.

	void HashMovableObject(StateHasher& hasher, const MovableObject* movableObject) {
		hasher.Add(movableObject->GetUniqueID());
		hasher.Add(movableObject->GetPos());
		hasher.Add(movableObject->GetVel());
		hasher.Add(movableObject->GetRotAngle());
		hasher.Add(movableObject->GetAngularVel());
		hasher.Add(movableObject->IsHFlipped());
		if (const Actor* actor = dynamic_cast<const Actor*>(movableObject)) {
			hasher.Add(actor->GetTeam());
			hasher.Add(actor->GetStatus());
			hasher.Add(actor->GetHealth());
		}
	}

	std::vector<SyncDigestEntry> ReadSyncDigest(std::string_view digest) {
		std::vector<SyncDigestEntry> entries(digest.size() / sizeof(SyncDigestEntry));
		std::memcpy(entries.data(), digest.data(), entries.size() * sizeof(SyncDigestEntry));
		return entries;
	}

	/// All there is to an MOPixel that wasn't made from a preset, like the ones that terrain breaks up into. The text that other MOs are passed along as can't describe these.
	#pragma pack(push, 1)
	struct SyncedLooseMOPixel {
		float mass, posX, posY, velX, velY, sharpness;
		double age;
		uint32_t lifetime;
		int32_t team, color, trailColor, trailLength;
		uint8_t material, hitsMOs, getsHitByMOs;
	};
	#pragma pack(pop)

	constexpr char c_LooseMOPixelMark = '!'; //!< What the copy of such an MOPixel starts with, which the text of an MO never does.

	/// Gets things ready for an MO that was just read from another machine's copy, the way adding it to MovableMan would have, but without moving it.
	void FinishSyncedMO(MovableObject* movableObject) {
		movableObject->SetAsAddedToMovableMan();
		if (MOSRotating* mosRotating = dynamic_cast<MOSRotating*>(movableObject)) {
			mosRotating->CorrectAttachableAndWoundPositionsAndRotations();
		}
		movableObject->NotResting();
		movableObject->NewFrame();
	}
} // namespace

void MovableMan::HashState(StateHasher& hasher) const {
	for (const std::deque<Actor*>* actors: {&m_Actors, &m_AddedActors}) {
		for (const Actor* actor: *actors) {
			HashMovableObject(hasher, actor);
		}
	}
	for (const std::deque<MovableObject*>* movableObjects: {&m_Items, &m_AddedItems, &m_Particles, &m_AddedParticles}) {
		for (const MovableObject* movableObject: *movableObjects) {
			HashMovableObject(hasher, movableObject);
		}
	}
}

std::string MovableMan::GetSyncText(const MovableObject* movableObject) const {
	// shortcut: this is how a saved game writes an MO, which leaves out what scripts keep about it and some of its inner workings. Those start over for an MO that gets recreated from this. Write MOs out in full once that shows.
	if (const MOPixel* pixel = dynamic_cast<const MOPixel*>(movableObject); pixel && (pixel->GetPresetName().empty() || pixel->GetPresetName() == "None")) {
		const Atom* atom = pixel->GetAtom();
		SyncedLooseMOPixel loose{pixel->GetMass(), pixel->GetPos().m_X, pixel->GetPos().m_Y, pixel->GetVel().m_X, pixel->GetVel().m_Y, pixel->GetSharpness(),
		                         static_cast<double>(pixel->GetAge()), static_cast<uint32_t>(pixel->GetLifetime()), pixel->GetTeam(), pixel->GetColorIndex(), atom->GetTrailColor().GetIndex(), atom->GetTrailLength(),
		                         static_cast<uint8_t>(atom->GetMaterial()->GetIndex()), pixel->HitsMOs(), pixel->GetsHitByMOs()};
		return std::string(1, c_LooseMOPixelMark) + std::string(reinterpret_cast<const char*>(&loose), sizeof(loose));
	}
	auto stream = std::make_unique<std::ostringstream>();
	std::ostringstream* text = stream.get();
	Writer writer(std::move(stream));
	g_SceneMan.GetScene()->SaveSceneObject(writer, movableObject, false, true);
	return text->str();
}

void MovableMan::WriteSyncDigest(std::string& digest) const {
	uint8_t list = 0;
	auto writeEntry = [&](const MovableObject* movableObject) {
		StateHasher hasher;
		HashMovableObject(hasher, movableObject);
		hasher.Add(GetSyncText(movableObject));
		SyncDigestEntry entry{list, movableObject->GetUniqueID(), hasher.GetHash()};
		digest.append(reinterpret_cast<const char*>(&entry), sizeof(entry));
	};
	for (const std::deque<Actor*>* actors: {&m_Actors, &m_AddedActors}) {
		std::for_each(actors->begin(), actors->end(), writeEntry);
		++list;
	}
	for (const std::deque<MovableObject*>* movableObjects: {&m_Items, &m_AddedItems, &m_Particles, &m_AddedParticles}) {
		std::for_each(movableObjects->begin(), movableObjects->end(), writeEntry);
		++list;
	}
}

bool MovableMan::FindSyncDifferences(std::string_view otherDigest, std::vector<bool>& wanted) const {
	std::string ownDigest;
	WriteSyncDigest(ownDigest);
	std::vector<SyncDigestEntry> otherEntries = ReadSyncDigest(otherDigest);
	std::vector<SyncDigestEntry> ownEntries = ReadSyncDigest(ownDigest);

	std::map<int64_t, uint64_t> ownHashes;
	std::vector<SyncDigestEntry> ownActors;
	for (const SyncDigestEntry& entry: ownEntries) {
		if (entry.list < c_FirstNonActorSyncList) {
			ownActors.push_back(entry);
		} else {
			ownHashes[entry.uniqueID] = entry.hash;
		}
	}

	wanted.assign(otherEntries.size(), false);
	size_t actorIndex = 0;
	for (size_t i = 0; i < otherEntries.size(); ++i) {
		const SyncDigestEntry& entry = otherEntries[i];
		if (entry.list < c_FirstNonActorSyncList) {
			// shortcut: Actors only get recreated where they are, since the Activity and scripts keep track of them. An Actor that only one machine has needs the whole game rebuilt.
			if (actorIndex >= ownActors.size() || ownActors[actorIndex].list != entry.list || ownActors[actorIndex].uniqueID != entry.uniqueID) {
				return false;
			}
			wanted[i] = ownActors[actorIndex++].hash != entry.hash;
		} else {
			auto ownHash = ownHashes.find(entry.uniqueID);
			wanted[i] = ownHash == ownHashes.end() || ownHash->second != entry.hash;
		}
	}
	return actorIndex == ownActors.size();
}

void MovableMan::WriteSyncObjects(const std::vector<bool>& wanted, std::string& objects) const {
	uint32_t index = 0;
	auto writeObject = [&](const MovableObject* movableObject) {
		if (index < wanted.size() && wanted[index]) {
			std::string text = GetSyncText(movableObject);
			uint32_t size = static_cast<uint32_t>(text.size());
			objects.append(reinterpret_cast<const char*>(&index), sizeof(index));
			objects.append(reinterpret_cast<const char*>(&size), sizeof(size));
			objects += text;
		}
		++index;
	};
	std::for_each(m_Actors.begin(), m_Actors.end(), writeObject);
	std::for_each(m_AddedActors.begin(), m_AddedActors.end(), writeObject);
	for (const std::deque<MovableObject*>* movableObjects: {&m_Items, &m_AddedItems, &m_Particles, &m_AddedParticles}) {
		std::for_each(movableObjects->begin(), movableObjects->end(), writeObject);
	}
}

bool MovableMan::ApplySyncObjects(std::string_view digest, std::string_view objects, int& recreatedCount, int& removedCount) {
	recreatedCount = 0;
	removedCount = 0;
	std::vector<SyncDigestEntry> entries = ReadSyncDigest(digest);

	std::map<uint32_t, std::string> texts;
	while (!objects.empty()) {
		uint32_t index = 0;
		uint32_t size = 0;
		if (objects.size() < sizeof(index) + sizeof(size)) {
			return false;
		}
		std::memcpy(&index, objects.data(), sizeof(index));
		std::memcpy(&size, objects.data() + sizeof(index), sizeof(size));
		objects.remove_prefix(sizeof(index) + sizeof(size));
		if (objects.size() < size || index >= entries.size()) {
			return false;
		}
		texts[index] = std::string(objects.substr(0, size));
		objects.remove_prefix(size);
	}

	// Reads an MO from the other machine's copy of it: into the MO that this machine has in its place, which keeps everything that points to it working, or into a new one.
	auto readObject = [&](const std::string& text, MovableObject* inPlaceOf) -> MovableObject* {
		if (text.starts_with(c_LooseMOPixelMark)) {
			SyncedLooseMOPixel loose;
			if (text.size() != 1 + sizeof(loose)) {
				return nullptr;
			}
			std::memcpy(&loose, text.data() + 1, sizeof(loose));
			// These are small enough to make anew, and the one that this machine had gets removed with the rest that are left over.
			MOPixel* pixel = new MOPixel(Color(loose.color), loose.mass, Vector(loose.posX, loose.posY), Vector(loose.velX, loose.velY), new Atom(Vector(), loose.material, nullptr, Color(loose.trailColor), loose.trailLength), loose.lifetime);
			pixel->SetSharpness(loose.sharpness);
			pixel->SetTeam(loose.team);
			pixel->SetToHitMOs(loose.hitsMOs != 0);
			pixel->SetToGetHitByMOs(loose.getsHitByMOs != 0);
			FinishSyncedMO(pixel);
			pixel->SetAge(loose.age);
			++recreatedCount;
			return pixel;
		}
		Reader reader(std::make_unique<std::istringstream>(text), g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/Sync.ini", true, nullptr, false);
		std::string className;
		reader >> className;
		MovableObject* movableObject = nullptr;
		if (inPlaceOf && inPlaceOf->GetClassName() == className) {
			movableObject = inPlaceOf;
			Actor* actor = dynamic_cast<Actor*>(movableObject);
			Controller::InputMode inputMode = actor ? actor->GetController()->GetInputMode() : Controller::CIM_DISABLED;
			int player = actor ? actor->GetController()->GetPlayer() : -1;
			if (actor) {
				RemoveActorFromTeamRoster(actor);
			}
			movableObject->Destroy();
			if (static_cast<Entity*>(movableObject)->Create(reader, false) < 0) {
				return nullptr;
			}
			if (actor) {
				actor->SetControllerMode(inputMode, player);
				AddActorToTeamRoster(actor);
			}
		} else {
			const Entity::ClassInfo* classInfo = Entity::ClassInfo::GetClass(className);
			Entity* entity = classInfo && classInfo->IsConcrete() ? classInfo->NewInstance() : nullptr;
			movableObject = dynamic_cast<MovableObject*>(entity);
			if (!movableObject || dynamic_cast<Actor*>(movableObject) || entity->Create(reader, false) < 0) {
				delete entity;
				return nullptr;
			}
		}
		FinishSyncedMO(movableObject);
		++recreatedCount;
		return movableObject;
	};

	std::vector<Actor*> ownActors(m_Actors.begin(), m_Actors.end());
	ownActors.insert(ownActors.end(), m_AddedActors.begin(), m_AddedActors.end());
	std::deque<MovableObject*>* nonActorLists[] = {&m_Items, &m_AddedItems, &m_Particles, &m_AddedParticles};
	std::map<int64_t, MovableObject*> ownNonActors;
	for (std::deque<MovableObject*>* movableObjects: nonActorLists) {
		for (MovableObject* movableObject: *movableObjects) {
			ownNonActors[movableObject->GetUniqueID()] = movableObject;
		}
		movableObjects->clear();
	}

	// The lists are put back together in the other machine's order, out of the MOs that this machine already has as they are there, and the ones read from the other machine's copies.
	bool allThere = true;
	size_t actorIndex = 0;
	for (uint32_t i = 0; i < entries.size(); ++i) {
		const SyncDigestEntry& entry = entries[i];
		auto text = texts.find(i);
		if (entry.list < c_FirstNonActorSyncList) {
			if (actorIndex >= ownActors.size()) {
				allThere = false;
			} else if (Actor* actor = ownActors[actorIndex++]; text != texts.end() && readObject(text->second, actor) != actor) {
				allThere = false;
			}
			continue;
		}
		MovableObject* movableObject = nullptr;
		if (auto own = ownNonActors.find(entry.uniqueID); own != ownNonActors.end()) {
			movableObject = own->second;
			if (text != texts.end()) {
				movableObject = readObject(text->second, own->second);
			}
			if (movableObject == own->second) {
				ownNonActors.erase(own);
			}
		} else if (text != texts.end()) {
			movableObject = readObject(text->second, nullptr);
		}
		if (movableObject && entry.list < c_FirstNonActorSyncList + std::size(nonActorLists)) {
			nonActorLists[entry.list - c_FirstNonActorSyncList]->push_back(movableObject);
		} else {
			allThere = false;
		}
	}

	// What is left over are MOs that the other machine doesn't have.
	for (auto& [uniqueID, movableObject]: ownNonActors) {
		delete movableObject;
		++removedCount;
	}
	m_ValidItems.clear();
	m_ValidItems.insert(m_Items.begin(), m_Items.end());
	m_ValidItems.insert(m_AddedItems.begin(), m_AddedItems.end());
	m_ValidParticles.clear();
	m_ValidParticles.insert(m_Particles.begin(), m_Particles.end());
	m_ValidParticles.insert(m_AddedParticles.begin(), m_AddedParticles.end());

	// The MOIDs still lead to parts of MOs that are gone now, and anything hit this update would be looked up through them.
	UpdateDrawMOIDs();
	return allThere && actorIndex == ownActors.size();
}

bool MovableMan::AddMO(MovableObject* movableObjectToAdd) {
	if (!movableObjectToAdd) {
		return false;
	}

	if (Actor* actorToAdd = dynamic_cast<Actor*>(movableObjectToAdd)) {
		AddActor(actorToAdd);
		return true;
	} else if (HeldDevice* heldDeviceToAdd = dynamic_cast<HeldDevice*>(movableObjectToAdd)) {
		AddItem(heldDeviceToAdd);
		return true;
	}
	AddParticle(movableObjectToAdd);

	return true;
}

void MovableMan::AddActor(Actor* actorToAdd) {
	if (actorToAdd && g_ActivityMan.GetActivity()) {
		actorToAdd->SetAsAddedToMovableMan();
		actorToAdd->CorrectAttachableAndWoundPositionsAndRotations();

		if (actorToAdd->IsTooFast()) {
			actorToAdd->SetToDelete(true);
		} else {
			if (!dynamic_cast<ADoor*>(actorToAdd)) {
				actorToAdd->MoveOutOfTerrain(g_MaterialGrass);
			}
			if (actorToAdd->IsStatus(Actor::INACTIVE)) {
				actorToAdd->SetStatus(Actor::STABLE);
			}
			actorToAdd->NotResting();
			actorToAdd->NewFrame();
			actorToAdd->SetAge(0);
		}

		{
			std::lock_guard<std::mutex> lock(m_AddedActorsMutex);
			m_AddedActors.push_back(actorToAdd);
			m_ValidActors.insert(actorToAdd);

			// This will call SetTeam and subsequently force the team as active.
			AddActorToTeamRoster(actorToAdd);
		}
	}
}

void MovableMan::AddItem(HeldDevice* itemToAdd) {
	if (itemToAdd && g_ActivityMan.GetActivity()) {
		g_ActivityMan.GetActivity()->ForceSetTeamAsActive(itemToAdd->GetTeam());

		itemToAdd->SetAsAddedToMovableMan();
		itemToAdd->CorrectAttachableAndWoundPositionsAndRotations();

		if (itemToAdd->IsTooFast()) {
			itemToAdd->SetToDelete(true);
		} else {
			if (!itemToAdd->IsSetToDelete()) {
				itemToAdd->MoveOutOfTerrain(g_MaterialGrass);
			}
			itemToAdd->NotResting();
			itemToAdd->NewFrame();
			itemToAdd->SetAge(0);
		}

		std::lock_guard<std::mutex> lock(m_AddedItemsMutex);
		m_AddedItems.push_back(itemToAdd);
		m_ValidItems.insert(itemToAdd);
	}
}

void MovableMan::AddParticle(MovableObject* particleToAdd) {
	if (particleToAdd && g_ActivityMan.GetActivity()) {
		g_ActivityMan.GetActivity()->ForceSetTeamAsActive(particleToAdd->GetTeam());

		particleToAdd->SetAsAddedToMovableMan();
		if (MOSRotating* particleToAddAsMOSRotating = dynamic_cast<MOSRotating*>(particleToAdd)) {
			particleToAddAsMOSRotating->CorrectAttachableAndWoundPositionsAndRotations();
		}

		if (particleToAdd->IsTooFast()) {
			particleToAdd->SetToDelete(true);
		} else {
			// TODO consider moving particles out of grass. It's old code that was removed because it's slow to do this for every particle.
			particleToAdd->NotResting();
			particleToAdd->NewFrame();
			particleToAdd->SetAge(0);
		}
		if (particleToAdd->IsDevice()) {
			std::lock_guard<std::mutex> lock(m_AddedItemsMutex);
			m_AddedItems.push_back(particleToAdd);
			m_ValidItems.insert(particleToAdd);
		} else {
			std::lock_guard<std::mutex> lock(m_AddedParticlesMutex);
			m_AddedParticles.push_back(particleToAdd);
			m_ValidParticles.insert(particleToAdd);
		}
	}
}

Actor* MovableMan::RemoveActor(MovableObject* pActorToRem) {
	Actor* removed = nullptr;

	if (pActorToRem) {
		for (std::deque<Actor*>::iterator itr = m_Actors.begin(); itr != m_Actors.end(); ++itr) {
			if (*itr == pActorToRem) {
				std::lock_guard<std::mutex> lock(m_ActorsMutex);
				removed = *itr;
				m_ValidActors.erase(*itr);
				m_Actors.erase(itr);
				break;
			}
		}
		// Try the newly added actors if we couldn't find it in the regular deque
		if (!removed) {
			for (std::deque<Actor*>::iterator itr = m_AddedActors.begin(); itr != m_AddedActors.end(); ++itr) {
				if (*itr == pActorToRem) {
					std::lock_guard<std::mutex> lock(m_AddedActorsMutex);
					removed = *itr;
					m_ValidActors.erase(*itr);
					m_AddedActors.erase(itr);
					break;
				}
			}
		}
		RemoveActorFromTeamRoster(dynamic_cast<Actor*>(pActorToRem));
		pActorToRem->SetAsAddedToMovableMan(false);
	}
	return removed;
}

MovableObject* MovableMan::RemoveItem(MovableObject* pItemToRem) {
	MovableObject* removed = nullptr;

	if (pItemToRem) {
		for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
			if (*itr == pItemToRem) {
				std::lock_guard<std::mutex> lock(m_ItemsMutex);
				removed = *itr;
				m_ValidItems.erase(*itr);
				m_Items.erase(itr);
				break;
			}
		}
		// Try the newly added items if we couldn't find it in the regular deque
		if (!removed) {
			for (std::deque<MovableObject*>::iterator itr = m_AddedItems.begin(); itr != m_AddedItems.end(); ++itr) {
				if (*itr == pItemToRem) {
					std::lock_guard<std::mutex> lock(m_AddedItemsMutex);
					removed = *itr;
					m_ValidItems.erase(*itr);
					m_AddedItems.erase(itr);
					break;
				}
			}
		}
		pItemToRem->SetAsAddedToMovableMan(false);
	}
	return removed;
}

MovableObject* MovableMan::RemoveParticle(MovableObject* pMOToRem) {
	MovableObject* removed = nullptr;

	if (pMOToRem) {
		for (std::deque<MovableObject*>::iterator itr = m_Particles.begin(); itr != m_Particles.end(); ++itr) {
			if (*itr == pMOToRem) {
				std::lock_guard<std::mutex> lock(m_ParticlesMutex);
				removed = *itr;
				m_ValidParticles.erase(*itr);
				m_Particles.erase(itr);
				break;
			}
		}
		// Try the newly added particles if we couldn't find it in the regular deque
		if (!removed) {
			for (std::deque<MovableObject*>::iterator itr = m_AddedParticles.begin(); itr != m_AddedParticles.end(); ++itr) {
				if (*itr == pMOToRem) {
					std::lock_guard<std::mutex> lock(m_AddedParticlesMutex);
					removed = *itr;
					m_ValidParticles.erase(*itr);
					m_AddedParticles.erase(itr);
					break;
				}
			}
		}
		pMOToRem->SetAsAddedToMovableMan(false);
	}
	return removed;
}

void MovableMan::AddActorToTeamRoster(Actor* pActorToAdd) {
	if (!pActorToAdd) {
		return;
	}

	// Add to the team roster and then sort it too
	int team = pActorToAdd->GetTeam();
	// Also re-set the TEam so that the Team Icons get set up properly
	pActorToAdd->SetTeam(team);
	// Only add to a roster if it's on a team AND is controllable (eg doors are not)
	if (team >= Activity::TeamOne && team < Activity::MaxTeamCount && pActorToAdd->IsControllable()) {
		std::lock_guard<std::mutex> lock(m_ActorRosterMutex);
		m_ActorRoster[pActorToAdd->GetTeam()].push_back(pActorToAdd);
		m_ActorRoster[pActorToAdd->GetTeam()].sort(MOXPosComparison());
	}
}

void MovableMan::RemoveActorFromTeamRoster(Actor* pActorToRem) {
	if (!pActorToRem) {
		return;
	}

	int team = pActorToRem->GetTeam();

	// Remove from roster as well
	if (team >= Activity::TeamOne && team < Activity::MaxTeamCount) {
		std::lock_guard<std::mutex> lock(m_ActorRosterMutex);
		m_ActorRoster[team].remove(pActorToRem);
	}
}

void MovableMan::ChangeActorTeam(Actor* pActor, int team) {
	if (!pActor) {
		return;
	}

	if (pActor->IsPlayerControlled()) {
		g_ActivityMan.GetActivity()->LoseControlOfActor(pActor->GetController()->GetPlayer());
	}

	RemoveActorFromTeamRoster(pActor);
	pActor->SetTeam(team);
	AddActorToTeamRoster(pActor);

	// Because doors affect the team-based pathfinders, we need to tell them there's been a change.
	// This is hackily done by erasing the door material, updating the pathfinders, then redrawing it and updating them again so they properly account for the door's new team.
	if (ADoor* actorAsADoor = dynamic_cast<ADoor*>(pActor); actorAsADoor && actorAsADoor->GetDoorMaterialDrawn()) {
		actorAsADoor->TempEraseOrRedrawDoorMaterial(true);
		g_SceneMan.GetTerrain()->AddUpdatedMaterialArea(actorAsADoor->GetBoundingBox());
		g_SceneMan.GetScene()->UpdatePathFinding();
		actorAsADoor->TempEraseOrRedrawDoorMaterial(false);
		g_SceneMan.GetTerrain()->AddUpdatedMaterialArea(actorAsADoor->GetBoundingBox());
		g_SceneMan.GetScene()->UpdatePathFinding();
	}
}

bool MovableMan::ValidateMOIDs() {
#ifdef DEBUG_BUILD
	for (const MovableObject* mo: m_MOIDIndex) {
		RTEAssert(mo, "Null MO found!");
	}
#endif
	return true;
}

bool MovableMan::ValidMO(const MovableObject* pMOToCheck) {
	bool exists = m_ValidActors.find(pMOToCheck) != m_ValidActors.end() ||
	              m_ValidItems.find(pMOToCheck) != m_ValidItems.end() ||
	              m_ValidParticles.find(pMOToCheck) != m_ValidParticles.end();

	return pMOToCheck && exists;
}

bool MovableMan::IsActor(const MovableObject* pMOToCheck) {
	return pMOToCheck && m_ValidActors.find(pMOToCheck) != m_ValidActors.end();
}

bool MovableMan::IsDevice(const MovableObject* pMOToCheck) {
	return pMOToCheck && m_ValidItems.find(pMOToCheck) != m_ValidItems.end();
}

bool MovableMan::IsParticle(const MovableObject* pMOToCheck) {
	return pMOToCheck && m_ValidParticles.find(pMOToCheck) != m_ValidParticles.end();
}

bool MovableMan::IsOfActor(MOID checkMOID) {
	if (checkMOID == g_NoMOID)
		return false;

	bool found = false;
	MovableObject* pMO = GetMOFromID(checkMOID);

	if (pMO) {
		MOID rootMOID = pMO->GetRootID();
		if (checkMOID != g_NoMOID) {
			for (std::deque<Actor*>::iterator itr = m_Actors.begin(); !found && itr != m_Actors.end(); ++itr) {
				if ((*itr)->GetID() == checkMOID || (*itr)->GetID() == rootMOID) {
					found = true;
					break;
				}
			}
			// Check actors just added this frame
			if (!found) {
				for (std::deque<Actor*>::iterator itr = m_AddedActors.begin(); !found && itr != m_AddedActors.end(); ++itr) {
					if ((*itr)->GetID() == checkMOID || (*itr)->GetID() == rootMOID) {
						found = true;
						break;
					}
				}
			}
		}
	}
	return found;
}

int MovableMan::GetContiguousActorID(const Actor* actor) const {
	auto itr = m_ContiguousActorIDs.find(actor);
	if (itr == m_ContiguousActorIDs.end()) {
		return -1;
	}

	return itr->second;
}

MOID MovableMan::GetRootMOID(MOID checkMOID) {
	MovableObject* pMO = GetMOFromID(checkMOID);
	if (pMO)
		return pMO->GetRootID();

	return g_NoMOID;
}

bool MovableMan::RemoveMO(MovableObject* pMOToRem) {
	if (pMOToRem) {
		if (RemoveActor(pMOToRem))
			return true;
		if (RemoveItem(pMOToRem))
			return true;
		if (RemoveParticle(pMOToRem))
			return true;
	}

	return false;
}

int MovableMan::KillAllTeamActors(int teamToKill) const {
	int killCount = 0;

	for (std::deque<Actor*> actorList: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorList) {
			if (actor->GetTeam() == teamToKill) {
				const AHuman* actorAsHuman = dynamic_cast<AHuman*>(actor);
				if (actorAsHuman && actorAsHuman->GetHead()) {
					actorAsHuman->GetHead()->GibThis();
				} else {
					actor->GibThis();
				}
				killCount++;
			}
		}
	}

	return killCount;
}

int MovableMan::KillAllEnemyActors(int teamNotToKill) const {
	int killCount = 0;

	for (std::deque<Actor*> actorList: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorList) {
			if (actor->GetTeam() != teamNotToKill) {
				const AHuman* actorAsHuman = dynamic_cast<AHuman*>(actor);
				if (actorAsHuman && actorAsHuman->GetHead()) {
					actorAsHuman->GetHead()->GibThis();
				} else {
					actor->GibThis();
				}
				killCount++;
			}
		}
	}

	return killCount;
}

int MovableMan::GetAllActors(bool transferOwnership, std::list<SceneObject*>& actorList, int onlyTeam, bool noBrains) {
	int addedCount = 0;

	// Add all regular Actors
	for (std::deque<Actor*>::iterator aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
		Actor* actor = *aIt;
		// Only grab ones of a specific team; delete all others
		if ((onlyTeam == Activity::NoTeam || actor->GetTeam() == onlyTeam) && (!noBrains || !actor->HasObjectInGroup("Brains"))) {
			actorList.push_back(actor);
			addedCount++;
		} else if (transferOwnership) {
			delete actor;
		}
	}

	// Add all Actors added this frame
	for (std::deque<Actor*>::iterator aIt = m_AddedActors.begin(); aIt != m_AddedActors.end(); ++aIt) {
		Actor* actor = *aIt;
		// Only grab ones of a specific team; delete all others
		if ((onlyTeam == Activity::NoTeam || actor->GetTeam() == onlyTeam) && (!noBrains || !actor->HasObjectInGroup("Brains"))) {
			actorList.push_back(actor);
			addedCount++;
		} else if (transferOwnership) {
			delete actor;
		}
	}

	if (transferOwnership) {
		// Clear the internal Actor lists; we transferred the ownership of them
		m_Actors.clear();
		m_AddedActors.clear();
		m_ValidActors.clear();

		// Also clear the actor rosters
		for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
			m_ActorRoster[team].clear();
		}
	}

	return addedCount;
}

int MovableMan::GetAllItems(bool transferOwnership, std::list<SceneObject*>& itemList) {
	int addedCount = 0;

	// Add all regular Items
	for (std::deque<MovableObject*>::iterator iIt = m_Items.begin(); iIt != m_Items.end(); ++iIt) {
		itemList.push_back((*iIt));
		addedCount++;
	}

	// Add all Items added this frame
	for (std::deque<MovableObject*>::iterator iIt = m_AddedItems.begin(); iIt != m_AddedItems.end(); ++iIt) {
		itemList.push_back((*iIt));
		addedCount++;
	}

	if (transferOwnership) {
		// Clear the internal Item list; we transferred the ownership of them
		m_Items.clear();
		m_AddedItems.clear();
		m_ValidItems.clear();
	}

	return addedCount;
}

int MovableMan::GetAllParticles(bool transferOwnership, std::list<SceneObject*>& particleList) {
	int addedCount = 0;

	// Add all regular particles
	for (std::deque<MovableObject*>::iterator iIt = m_Particles.begin(); iIt != m_Particles.end(); ++iIt) {
		particleList.push_back((*iIt));
		addedCount++;
	}

	// Add all particles added this frame
	for (std::deque<MovableObject*>::iterator iIt = m_AddedParticles.begin(); iIt != m_AddedParticles.end(); ++iIt) {
		particleList.push_back((*iIt));
		addedCount++;
	}

	if (transferOwnership) {
		// Clear the internal Particle list; we transferred the ownership of them
		m_Particles.clear();
		m_AddedParticles.clear();
		m_ValidParticles.clear();
	}

	return addedCount;
}

int MovableMan::GetTeamMOIDCount(int team) const {
	if (team > Activity::NoTeam && team < Activity::MaxTeamCount)
		return m_TeamMOIDCount[team];
	else
		return 0;
}

void MovableMan::OpenAllDoors(bool open, int team) const {
	for (std::deque<Actor*> actorDeque: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorDeque) {
			if (ADoor* actorAsADoor = dynamic_cast<ADoor*>(actor); actorAsADoor && actorAsADoor->GetTeam() == team) {
				if (actorAsADoor->GetDoorState() != (open ? ADoor::DoorState::OPEN : ADoor::DoorState::CLOSED)) {
					actorAsADoor->Update();
					actorAsADoor->SetClosedByDefault(!open);
				}
				actorAsADoor->ResetSensorTimer();
				if (open) {
					actorAsADoor->OpenDoor();
				} else {
					actorAsADoor->CloseDoor();
				}
			}
		}
	}
}

// TODO: Completely tear out and delete this.
// It shouldn't belong to MovableMan, instead it probably ought to be on the pathfinder. On that note, pathfinders shouldn't be part of the scene!
// AIMan? PathingMan? Something like that. Ideally, we completely tear out this hack, and allow for doors in a completely different way.
void MovableMan::OverrideMaterialDoors(bool eraseDoorMaterial, int team) const {
	for (std::deque<Actor*> actorDeque: {m_Actors, m_AddedActors}) {
		for (Actor* actor: actorDeque) {
			if (ADoor* actorAsDoor = dynamic_cast<ADoor*>(actor); actorAsDoor && (team == Activity::NoTeam || actorAsDoor->GetTeam() == team)) {
				actorAsDoor->TempEraseOrRedrawDoorMaterial(eraseDoorMaterial);
			}
		}
	}
}

void MovableMan::RegisterAlarmEvent(const AlarmEvent& newEvent) {
	std::lock_guard<std::mutex> lock(m_AddedAlarmEventsMutex);
	m_AddedAlarmEvents.push_back(new AlarmEvent(newEvent));
}

void callLuaFunctionOnMORecursive(MovableObject* mo, const std::string& functionName, const std::vector<const Entity*>& functionEntityArguments, const std::vector<std::string_view>& functionLiteralArguments, const std::vector<LuaObjectWrapper*>& functionObjectArguments) {
	if (MOSRotating* mosr = dynamic_cast<MOSRotating*>(mo)) {
		for (auto attachablrItr = mosr->GetAttachableList().begin(); attachablrItr != mosr->GetAttachableList().end();) {
			Attachable* attachable = *attachablrItr;
			++attachablrItr;

			attachable->RunScriptedFunctionInAppropriateScripts(functionName, false, false, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
			callLuaFunctionOnMORecursive(attachable, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}

		for (auto woundItr = mosr->GetWoundList().begin(); woundItr != mosr->GetWoundList().end();) {
			AEmitter* wound = *woundItr;
			++woundItr;

			wound->RunScriptedFunctionInAppropriateScripts(functionName, false, false, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
			callLuaFunctionOnMORecursive(wound, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}
	}

	mo->RunScriptedFunctionInAppropriateScripts(functionName, false, false, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
};

void MovableMan::RunLuaFunctionOnAllMOs(const std::string& functionName, bool includeAdded, const std::vector<const Entity*>& functionEntityArguments, const std::vector<std::string_view>& functionLiteralArguments, const std::vector<LuaObjectWrapper*>& functionObjectArguments) {
	if (includeAdded) {
		for (Actor* actor: m_AddedActors) {
			callLuaFunctionOnMORecursive(actor, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}

		for (MovableObject* item: m_AddedItems) {
			callLuaFunctionOnMORecursive(item, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}

		for (MovableObject* particle: m_AddedParticles) {
			callLuaFunctionOnMORecursive(particle, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
		}
	}

	for (Actor* actor: m_Actors) {
		callLuaFunctionOnMORecursive(actor, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
	}

	for (MovableObject* item: m_Items) {
		callLuaFunctionOnMORecursive(item, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
	}

	for (MovableObject* particle: m_Particles) {
		callLuaFunctionOnMORecursive(particle, functionName, functionEntityArguments, functionLiteralArguments, functionObjectArguments);
	}
}

void reloadLuaFunctionsOnMORecursive(MovableObject* mo) {
	if (MOSRotating* mosr = dynamic_cast<MOSRotating*>(mo)) {
		for (auto attachablrItr = mosr->GetAttachableList().begin(); attachablrItr != mosr->GetAttachableList().end();) {
			Attachable* attachable = *attachablrItr;
			++attachablrItr;

			attachable->ReloadScripts();
			reloadLuaFunctionsOnMORecursive(attachable);
		}

		for (auto woundItr = mosr->GetWoundList().begin(); woundItr != mosr->GetWoundList().end();) {
			AEmitter* wound = *woundItr;
			++woundItr;

			wound->ReloadScripts();
			reloadLuaFunctionsOnMORecursive(wound);
		}
	}

	mo->ReloadScripts();
};

void MovableMan::ReloadLuaScripts() {
	for (Actor* actor: m_AddedActors) {
		reloadLuaFunctionsOnMORecursive(actor);
	}

	for (MovableObject* item: m_AddedItems) {
		reloadLuaFunctionsOnMORecursive(item);
	}

	for (MovableObject* particle: m_AddedParticles) {
		reloadLuaFunctionsOnMORecursive(particle);
	}

	for (Actor* actor: m_Actors) {
		reloadLuaFunctionsOnMORecursive(actor);
	}

	for (MovableObject* item: m_Items) {
		reloadLuaFunctionsOnMORecursive(item);
	}

	for (MovableObject* particle: m_Particles) {
		reloadLuaFunctionsOnMORecursive(particle);
	}
}

void MovableMan::Update() {
	ZoneScoped;

	// Don't update if paused
	if (g_ActivityMan.GetActivity() && g_ActivityMan.ActivityPaused()) {
		return;
	}

	m_SimUpdateFrameNumber++;

	// ---TEMP ---
	// These are here for multithreaded AI, but will be unnecessary when multithreaded-sim-and-render is in!
	// Clear the MO color layer only if this is a drawn update. Nothing gets drawn when headless, so neither is the MO color layer.
	if (g_TimerMan.DrawnSimUpdate() && !System::IsInHeadlessMode()) {
		g_SceneMan.ClearMOColorLayer();
	}

	// If this is the first sim update since a drawn one, then clear the post effects
	if (g_TimerMan.SimUpdatesSinceDrawn() == 0) {
		g_PostProcessMan.ClearScenePostEffects();
	}
	// ---TEMP---

	// Reset the draw HUD roster line settings
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		m_SortTeamRoster[team] = false;
	}

	// Move all last frame's alarm events into the proper buffer, and clear out the new one to fill up with this frame's
	for (AlarmEvent* alarmEvent: m_AlarmEvents) {
		delete alarmEvent;
	}
	m_AlarmEvents.clear();
	for (std::vector<AlarmEvent*>::iterator aeItr = m_AddedAlarmEvents.begin(); aeItr != m_AddedAlarmEvents.end(); ++aeItr) {
		m_AlarmEvents.push_back(*aeItr);
	}
	m_AddedAlarmEvents.clear();

	// Travel MOs
	Travel();

	// If our debug settings switch is forcing all pathing requests to immediately complete, make sure they're done here
	if (g_SettingsMan.GetForceImmediatePathingRequestCompletion() && g_SceneMan.GetScene()) {
		g_SceneMan.GetScene()->BlockUntilAllPathingRequestsComplete();
	}

	// Finish our Seeing rays from last frame
	m_ActorsSeeFuture.wait();

	DeterminismCheckSimState("MOs travelled");

	// Prior to controller/AI update, execute lua callbacks
	g_LuaMan.ExecuteLuaScriptCallbacks();

	// Updates everything needed prior to AI/user input being processed
	// Fugly hack to keep backwards compat with scripts that rely on weird frame-delay-ordering behaviours
	// TODO, cleanup the pre-controller update and post-controller updates to have some consistent logic of what goes where
	PreControllerUpdate();

	// Updates AI/user input
	UpdateControllers();

	DeterminismCheckSimState("Controllers updated");

	// Will use some common iterators
	std::deque<Actor*>::iterator aIt;
	std::deque<Actor*>::iterator amidIt;
	std::deque<MovableObject*>::iterator iIt;
	std::deque<MovableObject*>::iterator imidIt;
	std::deque<MovableObject*>::iterator parIt;
	std::deque<MovableObject*>::iterator midIt;

	// Update all multithreaded scripts for all objects
	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ScriptsUpdate);
	{
		ZoneScopedN("Multithreaded Scripts Update");

		const std::string threadedUpdate = "ThreadedUpdate"; // avoid string reconstruction

		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		for (MovableObject* mo: g_LuaMan.GetMasterScriptState().GetRegisteredMOs()) {
			if (ValidMO(mo->GetRootParent())) {
				mo->RunScriptedFunctionInAppropriateScripts(threadedUpdate, false, false, {}, {}, {});
			}
		}
		g_LuaMan.SetThreadLuaStateOverride(nullptr);

		LuaStatesArray& luaStates = g_LuaMan.GetThreadedScriptStates();
		RunOnEachThreadedLuaState(luaStates.size(), [&](int start, int end) {
			RTEAssert(start + 1 == end, "Threaded script state being updated across multiple threads!");
			LuaStateWrapper& luaState = luaStates[start];
			g_LuaMan.SetThreadLuaStateOverride(&luaState);
			luaState.SetRunningThreadedUpdate(true);

			for (MovableObject* mo: luaState.GetRegisteredMOs()) {
				if (ValidMO(mo->GetRootParent())) {
					mo->RunScriptedFunctionInAppropriateScripts(threadedUpdate, false, false, {}, {}, {});
				}
			}

			luaState.SetRunningThreadedUpdate(false);
			g_LuaMan.SetThreadLuaStateOverride(nullptr);
		});

		SortAddedMOs();
	}

	{
		ZoneScopedN("Multithreaded Scripts SyncedUpdate");

		const std::string syncedUpdate = "SyncedUpdate"; // avoid string reconstruction

		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		for (MovableObject* mo: g_LuaMan.GetMasterScriptState().GetRegisteredMOs()) {
			if (ValidMO(mo->GetRootParent())) {
				mo->RunScriptedFunctionInAppropriateScripts(syncedUpdate, false, false, {}, {}, {});
			}
		}
		g_LuaMan.SetThreadLuaStateOverride(nullptr);

		for (LuaStateWrapper& luaState: g_LuaMan.GetThreadedScriptStates()) {
			g_LuaMan.SetThreadLuaStateOverride(&luaState);

			for (MovableObject* mo: luaState.GetRegisteredMOs()) {
				if (mo->HasRequestedSyncedUpdate()) {
					mo->RunScriptedFunctionInAppropriateScripts(syncedUpdate, false, false, {}, {}, {});
					mo->ResetRequestedSyncedUpdateFlag();
				}
			}

			g_LuaMan.SetThreadLuaStateOverride(nullptr);
		}
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ScriptsUpdate);

	DeterminismCheckSimState("Threaded scripts updated");

	{
		{
			ZoneScopedN("Actors Update");

			g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsUpdate);
			for (Actor* actor: m_Actors) {
				actor->Update();

				g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ScriptsUpdate);
				actor->UpdateScripts();
				g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ScriptsUpdate);

				actor->ApplyImpulses();
			}
			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsUpdate);
		}

		{
			ZoneScopedN("Items Update");

			int count = 0;
			int itemLimit = m_Items.size() - m_MaxDroppedItems;
			for (iIt = m_Items.begin(); iIt != m_Items.end(); ++iIt, ++count) {
				(*iIt)->Update();

				g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ScriptsUpdate);
				(*iIt)->UpdateScripts();
				g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ScriptsUpdate);

				(*iIt)->ApplyImpulses();
				if (count <= itemLimit) {
					(*iIt)->SetToSettle(true);
				}
			}
		}

		{
			ZoneScopedN("Particles Update");

			g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
			for (MovableObject* particle: m_Particles) {
				particle->Update();

				g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ScriptsUpdate);
				particle->UpdateScripts();
				g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ScriptsUpdate);

				particle->ApplyImpulses();
				particle->RestDetection();
				// Copy particles that are at rest to the terrain and mark them for deletion.
				if (particle->IsAtRest()) {
					particle->SetToSettle(true);
				}
			}
			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
		}

		{
			ZoneScopedN("Post Update");

			for (Actor* actor: m_Actors) {
				actor->PostUpdate();
			}

			for (MovableObject* item: m_Items) {
				item->PostUpdate();
			}

			for (MovableObject* particle: m_Particles) {
				particle->PostUpdate();
			}
		}
	} // namespace RTE

	DeterminismCheckSimState("MOs updated");

	//////////////////////////////////////////////////////////////////////
	// TRANSFER ALL MOs ADDED THIS FRAME
	// All Actors, Items, and Particles added this frame now are officially added

	{
		ZoneScopedN("MO Transfer and Deletion");

		{
			// Actors
			for (aIt = m_AddedActors.begin(); aIt != m_AddedActors.end(); ++aIt) {
				// Delete instead if it's marked for it
				if (!(*aIt)->IsSetToDelete())
					m_Actors.push_back(*aIt);
				else {
					// Also remove actor from the roster
					if ((*aIt)->GetTeam() >= 0) {
						// m_ActorRoster[(*aIt)->GetTeam()].remove(*aIt);
						RemoveActorFromTeamRoster(*aIt);
					}

					(*aIt)->DestroyScriptState();
					delete (*aIt);

					m_ValidActors.erase(*aIt);
				}
			}
			m_AddedActors.clear();

			// Items
			for (iIt = m_AddedItems.begin(); iIt != m_AddedItems.end(); ++iIt) {
				// Delete instead if it's marked for it
				if (!(*iIt)->IsSetToDelete()) {
					m_Items.push_back(*iIt);
				} else {
					(*iIt)->DestroyScriptState();
					delete (*iIt);
					m_ValidItems.erase(*iIt);
				}
			}
			m_AddedItems.clear();

			// Particles
			for (parIt = m_AddedParticles.begin(); parIt != m_AddedParticles.end(); ++parIt) {
				// Delete instead if it's marked for it
				if (!(*parIt)->IsSetToDelete()) {
					m_Particles.push_back(*parIt);
				} else {
					(*parIt)->DestroyScriptState();
					delete (*parIt);
					m_ValidParticles.erase(*parIt);
				}
			}
			m_AddedParticles.clear();
		}

		////////////////////////////////////////////////////////////////////////////
		// Copy (Settle) Pass

		{
			// DEATH //////////////////////////////////////////////////////////
			// Transfer dead actors from Actor list to particle list
			aIt = partition(m_Actors.begin(), m_Actors.end(), std::not_fn(std::mem_fn(&Actor::IsDead)));
			amidIt = aIt;

			// Move dead Actor to particles list
			if (amidIt != m_Actors.end() /* && m_Actors.size() > 1*/) {
				while (aIt != m_Actors.end()) {
					// Report the death of the actor to the game
					g_ActivityMan.GetActivity()->ReportDeath((*aIt)->GetTeam());

					// Add to the particles list
					m_Particles.push_back(*aIt);
					m_ValidParticles.insert(*aIt);
					// Remove from the team roster

					if ((*aIt)->GetTeam() >= 0) {
						// m_ActorRoster[(*aIt)->GetTeam()].remove(*aIt);
						RemoveActorFromTeamRoster(*aIt);
					}

					m_ValidActors.erase(*aIt);
					aIt++;
				}
				// Try to set the existing iterator to a safer value, erase can crash in debug mode otherwise?
				aIt = m_Actors.begin();
				m_Actors.erase(amidIt, m_Actors.end());
			}

			// ITEM SETTLE //////////////////////////////////////////////////////////
			// Transfer excess items to particle list - use stable partition, item orde is important
			iIt = stable_partition(m_Items.begin(), m_Items.end(), std::not_fn(std::mem_fn(&MovableObject::ToSettle)));
			imidIt = iIt;

			// Move force-settled items to particles list
			if (imidIt != m_Items.end() /* && m_Items.size() > 1*/) {
				while (iIt != m_Items.end()) {
					(*iIt)->SetToSettle(false);
					// Disable TDExplosive's immunity to settling
					if ((*iIt)->GetRestThreshold() < 0) {
						(*iIt)->SetRestThreshold(500);
					}
					m_Particles.push_back(*iIt);
					m_ValidItems.erase(*iIt);
					iIt++;
				}
				m_Items.erase(imidIt, m_Items.end());
			}

			// DELETE //////////////////////////////////////////////////////////
			// Only delete after all travels & updates are done
			// Actors
			aIt = partition(m_Actors.begin(), m_Actors.end(), std::not_fn(std::mem_fn(&MovableObject::ToDelete)));
			amidIt = aIt;

			while (aIt != m_Actors.end()) {
				// Set brain to 0 to avoid crashes due to brain deletion
				Activity* pActivity = g_ActivityMan.GetActivity();
				if (pActivity) {
					if (pActivity->IsAssignedBrain(*aIt))
						pActivity->SetPlayerBrain(0, pActivity->IsBrainOfWhichPlayer(*aIt));

					pActivity->ReportDeath((*aIt)->GetTeam());
				}

				// Remove from team rosters
				if ((*aIt)->GetTeam() >= Activity::TeamOne && (*aIt)->GetTeam() < Activity::MaxTeamCount)
					// m_ActorRoster[(*aIt)->GetTeam()].remove(*aIt);
					RemoveActorFromTeamRoster(*aIt);

				// Delete
				(*aIt)->DestroyScriptState();
				delete (*aIt);
				m_ValidActors.erase(*aIt);
				aIt++;
			}
			// Try to set the existing iterator to a safer value, erase can crash in debug mode otherwise?
			aIt = m_Actors.begin();
			m_Actors.erase(amidIt, m_Actors.end());

			// Items
			iIt = stable_partition(m_Items.begin(), m_Items.end(), std::not_fn(std::mem_fn(&MovableObject::ToDelete)));
			imidIt = iIt;

			while (iIt != m_Items.end()) {
				(*iIt)->DestroyScriptState();
				delete (*iIt);
				m_ValidItems.erase(*iIt);
				iIt++;
			}
			m_Items.erase(imidIt, m_Items.end());

			// Particles
			parIt = partition(m_Particles.begin(), m_Particles.end(), std::not_fn(std::mem_fn(&MovableObject::ToDelete)));
			midIt = parIt;

			while (parIt != m_Particles.end()) {
				(*parIt)->DestroyScriptState();
				delete (*parIt);
				m_ValidParticles.erase(*parIt);
				parIt++;
			}
			m_Particles.erase(midIt, m_Particles.end());
		}

		// SETTLE PARTICLES //////////////////////////////////////////////////
		// Only settle after all updates and deletions are done
		if (m_SettlingEnabled) {
			parIt = partition(m_Particles.begin(), m_Particles.end(), std::not_fn(std::mem_fn(&MovableObject::ToSettle)));
			midIt = parIt;

			while (parIt != m_Particles.end()) {
				Vector parPos((*parIt)->GetPos());
				Material const* terrMat = g_SceneMan.GetMaterialFromID(g_SceneMan.GetTerrain()->GetMaterialPixel(parPos.GetFloorIntX(), parPos.GetFloorIntY()));
				int piling = (*parIt)->GetMaterial()->GetPiling();
				if (piling > 0) {
					for (int s = 0; s < piling && (terrMat->GetIndex() == (*parIt)->GetMaterial()->GetIndex() || terrMat->GetIndex() == (*parIt)->GetMaterial()->GetSettleMaterial()); ++s) {
						if ((piling - s) % 2 == 0) {
							parPos.m_Y -= 1.0F;
						} else {
							parPos.m_X += (RandomNum() >= 0.5F ? 1.0F : -1.0F);
						}
						terrMat = g_SceneMan.GetMaterialFromID(g_SceneMan.GetTerrain()->GetMaterialPixel(parPos.GetFloorIntX(), parPos.GetFloorIntY()));
					}
					(*parIt)->SetPos(parPos.GetFloored());
				}
				if ((*parIt)->GetDrawPriority() >= terrMat->GetPriority()) {
					(*parIt)->DrawToTerrain(g_SceneMan.GetTerrain());
				}
				(*parIt)->DestroyScriptState();
				delete (*parIt);
				m_ValidParticles.erase(*parIt);
				parIt++;
			}
			m_Particles.erase(midIt, m_Particles.end());
		}
	}

	DeterminismCheckSimState("MOs added and removed");

	// Run seeing rays for all actors
	m_ActorsSeeFuture = g_ThreadMan.GetPriorityThreadPool().parallelize_loop(m_Actors.size(),
	                                                                         [&](int start, int end) {
		                                                                         ZoneScopedN("Actors See");
		                                                                         for (int i = start; i < end; ++i) {
			                                                                         m_Actors[i]->CastSeeRays();
		                                                                         }
	                                                                         });

	// We've finished stuff that can interact with lua script, so it's the ideal time to start a gc run
	g_LuaMan.StartAsyncGarbageCollection();

	// Draw the MO matter and IDs to their layers for next frame
	m_DrawMOIDsTask = g_ThreadMan.GetPriorityThreadPool().submit([this]() {
		UpdateDrawMOIDs();
	});

	////////////////////////////////////////////////////////////////////
	// Draw the MO colors ONLY if this is a drawn update! Nothing gets drawn when headless, so the MO colors would never be seen.

	if (g_TimerMan.DrawnSimUpdate() && !System::IsInHeadlessMode()) {
		Draw(g_SceneMan.GetMOColorBitmap());
	}

	// Sort team rosters if necessary
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		if (m_SortTeamRoster[Activity::TeamOne]) {
			m_ActorRoster[team].sort(MOXPosComparison());
		}
	}
}

void MovableMan::SortAddedMOs() {
	std::sort(m_AddedActors.begin(), m_AddedActors.end(), LuaStateWrapper::UniqueIDOrder());
	std::sort(m_AddedItems.begin(), m_AddedItems.end(), LuaStateWrapper::UniqueIDOrder());
	std::sort(m_AddedParticles.begin(), m_AddedParticles.end(), LuaStateWrapper::UniqueIDOrder());
}

void MovableMan::Travel() {
	ZoneScoped;

	// Travel Actors
	{
		ZoneScopedN("Actors Travel");

		g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsTravel);
		for (auto aIt = m_Actors.begin(); aIt != m_Actors.end(); ++aIt) {
			if (!((*aIt)->IsUpdated())) {
				(*aIt)->ApplyForces();
				(*aIt)->PreTravel();
				(*aIt)->Travel();
				(*aIt)->PostTravel();
			}
			(*aIt)->NewFrame();
		}
		g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsTravel);
	}

	// Travel items
	{
		ZoneScopedN("Items Travel");

		for (auto iIt = m_Items.begin(); iIt != m_Items.end(); ++iIt) {
			if (!((*iIt)->IsUpdated())) {
				(*iIt)->ApplyForces();
				(*iIt)->PreTravel();
				(*iIt)->Travel();
				(*iIt)->PostTravel();
			}
			(*iIt)->NewFrame();
		}
	}

	// Travel particles
	{
		ZoneScopedN("Particles Travel");

		g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ParticlesTravel);
		for (auto parIt = m_Particles.begin(); parIt != m_Particles.end(); ++parIt) {
			if (!((*parIt)->IsUpdated())) {
				(*parIt)->ApplyForces();
				(*parIt)->PreTravel();
				(*parIt)->Travel();
				(*parIt)->PostTravel();
			}
			(*parIt)->NewFrame();
		}
		g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ParticlesTravel);
	}
}

void MovableMan::UpdateControllers() {
	ZoneScoped;

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsAI);
	{
		for (Actor* actor: m_Actors) {
			actor->GetController()->Update();
		}

		g_LuaMan.SetThreadLuaStateOverride(&g_LuaMan.GetMasterScriptState());
		g_LuaMan.GetMasterScriptState().SetRunningAI(true);
		for (Actor* actor: m_Actors) {
			if (actor->GetLuaState() == &g_LuaMan.GetMasterScriptState() && actor->GetController()->ShouldUpdateAIThisFrame()) {
				actor->RunScriptedFunctionInAppropriateScripts("ThreadedUpdateAI", false, true, {}, {}, {});
			}
		}
		g_LuaMan.GetMasterScriptState().SetRunningAI(false);
		g_LuaMan.SetThreadLuaStateOverride(nullptr);

		LuaStatesArray& luaStates = g_LuaMan.GetThreadedScriptStates();
		RunOnEachThreadedLuaState(luaStates.size(), [&](int start, int end) {
			RTEAssert(start + 1 == end, "Threaded script state being updated across multiple threads!");
			LuaStateWrapper& luaState = luaStates[start];
			g_LuaMan.SetThreadLuaStateOverride(&luaState);
			luaState.SetRunningAI(true);
			for (Actor* actor: m_Actors) {
				if (actor->GetLuaState() == &luaState && actor->GetController()->ShouldUpdateAIThisFrame()) {
					actor->RunScriptedFunctionInAppropriateScripts("ThreadedUpdateAI", false, true, {}, {}, {});
				}
			}
			luaState.SetRunningAI(false);
			g_LuaMan.SetThreadLuaStateOverride(nullptr);
		});

		SortAddedMOs();

		for (Actor* actor: m_Actors) {
			LuaStateWrapper* luaState = actor->GetLuaState();
			if (luaState && actor->GetController()->ShouldUpdateAIThisFrame()) {
				luaState->SetRunningAI(true);
				actor->RunScriptedFunctionInAppropriateScripts("UpdateAI", false, true, {}, {}, {});
				luaState->SetRunningAI(false);
			}
		}
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsAI);
}

void MovableMan::PreControllerUpdate() {
	ZoneScoped;

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActorsUpdate);
	for (Actor* actor: m_Actors) {
		actor->PreControllerUpdate();
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActorsUpdate);

	for (MovableObject* item: m_Items) {
		item->PreControllerUpdate();
	}

	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
	for (MovableObject* particle: m_Particles) {
		particle->PreControllerUpdate();
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ParticlesUpdate);
}

void MovableMan::DrawMatter(BITMAP* pTargetBitmap, Vector& targetPos) {
	// Draw objects to accumulation bitmap
	for (std::deque<Actor*>::iterator aIt = --m_Actors.end(); aIt != --m_Actors.begin(); --aIt)
		(*aIt)->Draw(pTargetBitmap, targetPos, g_DrawMaterial);

	for (std::deque<MovableObject*>::iterator parIt = --m_Particles.end(); parIt != --m_Particles.begin(); --parIt)
		(*parIt)->Draw(pTargetBitmap, targetPos, g_DrawMaterial);
}

void MovableMan::VerifyMOIDIndex() {
	int count = 0;
	for (std::vector<MovableObject*>::iterator aIt = m_MOIDIndex.begin(); aIt != m_MOIDIndex.end(); ++aIt) {
		if (*aIt) {
			RTEAssert((*aIt)->GetID() == g_NoMOID || (*aIt)->GetID() == count, "MOIDIndex broken!");
			RTEAssert((*aIt)->GetRootID() == g_NoMOID || ((*aIt)->GetRootID() >= 0 && (*aIt)->GetRootID() < g_MovableMan.GetMOIDCount()), "MOIDIndex broken!");
		}
		count++;
		if (count == g_NoMOID)
			count++;
	}

	for (std::deque<MovableObject*>::iterator itr = m_Items.begin(); itr != m_Items.end(); ++itr) {
		RTEAssert((*itr)->GetID() == g_NoMOID || (*itr)->GetID() < GetMOIDCount(), "MOIDIndex broken!");
		RTEAssert((*itr)->GetRootID() == g_NoMOID || ((*itr)->GetRootID() >= 0 && (*itr)->GetRootID() < g_MovableMan.GetMOIDCount()), "MOIDIndex broken!");
	}
	// Try the items just added this frame
	for (std::deque<MovableObject*>::iterator itr = m_AddedItems.begin(); itr != m_AddedItems.end(); ++itr) {
		RTEAssert((*itr)->GetID() == g_NoMOID || (*itr)->GetID() < GetMOIDCount(), "MOIDIndex broken!");
		RTEAssert((*itr)->GetRootID() == g_NoMOID || ((*itr)->GetRootID() >= 0 && (*itr)->GetRootID() < g_MovableMan.GetMOIDCount()), "MOIDIndex broken!");
	}
}

void MovableMan::UpdateDrawMOIDs() {
	ZoneScoped;

	///////////////////////////////////////////////////
	// Clear the MOID layer before starting to delete stuff which may be in the MOIDIndex
	g_SceneMan.ClearAllMOIDDrawings();

	// Clear the index each frame and do it over because MO's get added and deleted between each frame.
	m_MOIDIndex.clear();
	m_ContiguousActorIDs.clear();

	// Add a null and start counter at 1 because MOID == 0 means no MO.
	// - Update: This isnt' true anymore, but still keep 0 free just to be safe
	m_MOIDIndex.push_back(0);

	MOID currentMOID = 1;

	int actorID = 0;
	for (Actor* actor: m_Actors) {
		m_ContiguousActorIDs[actor] = actorID++;
		if (!actor->IsSetToDelete()) {
			actor->UpdateMOID(m_MOIDIndex);
			actor->Draw(nullptr, Vector(), g_DrawMOID, true);
			currentMOID = m_MOIDIndex.size();
		} else {
			actor->SetAsNoID();
		}
	}

	for (MovableObject* item: m_Items) {
		if (!item->IsSetToDelete()) {
			item->UpdateMOID(m_MOIDIndex);
			item->Draw(nullptr, Vector(), g_DrawMOID, true);
			currentMOID = m_MOIDIndex.size();
		} else {
			item->SetAsNoID();
		}
	}

	for (MovableObject* particle: m_Particles) {
		if (!particle->IsSetToDelete()) {
			particle->UpdateMOID(m_MOIDIndex);
			particle->Draw(nullptr, Vector(), g_DrawMOID, true);
			currentMOID = m_MOIDIndex.size();
		} else {
			particle->SetAsNoID();
		}
	}

	// COUNT MOID USAGE PER TEAM  //////////////////////////////////////////////////
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; team++) {
		m_TeamMOIDCount[team] = 0;
	}

	for (auto itr = m_MOIDIndex.begin(); itr != m_MOIDIndex.end(); ++itr) {
		if (*itr) {
			int team = (*itr)->GetTeam();
			if (team > Activity::NoTeam && team < Activity::MaxTeamCount) {
				m_TeamMOIDCount[team]++;
			}
		}
	}
}

void MovableMan::CompleteQueuedMOIDDrawings() {
	if (m_DrawMOIDsTask.valid()) {
		m_DrawMOIDsTask.wait();
	}
}

void MovableMan::Draw(BITMAP* pTargetBitmap, const Vector& targetPos) {
	ZoneScoped;

	// Draw objects to accumulation bitmap, in reverse order so actors appear on top.

	{
		ZoneScopedN("Particles Draw");

		for (std::deque<MovableObject*>::iterator parIt = m_Particles.begin(); parIt != m_Particles.end(); ++parIt) {
			(*parIt)->Draw(pTargetBitmap, targetPos);
		}
	}

	{
		ZoneScopedN("Items Draw");

		for (std::deque<MovableObject*>::reverse_iterator itmIt = m_Items.rbegin(); itmIt != m_Items.rend(); ++itmIt) {
			(*itmIt)->Draw(pTargetBitmap, targetPos);
		}
	}

	{
		ZoneScopedN("Actors Draw");

		for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt) {
			(*aIt)->Draw(pTargetBitmap, targetPos);
		}
	}
}

void MovableMan::DrawHUD(BITMAP* pTargetBitmap, const Vector& targetPos, int which, bool playerControlled) {
	ZoneScoped;

	// Draw HUD elements
	for (std::deque<MovableObject*>::reverse_iterator itmIt = m_Items.rbegin(); itmIt != m_Items.rend(); ++itmIt)
		(*itmIt)->DrawHUD(pTargetBitmap, targetPos, which);

	for (std::deque<Actor*>::reverse_iterator aIt = m_Actors.rbegin(); aIt != m_Actors.rend(); ++aIt)
		(*aIt)->DrawHUD(pTargetBitmap, targetPos, which);
}
