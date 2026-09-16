#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <string>

#include "Session.hpp"

namespace gdpf::engine {

constexpr float kStepDt = 1.f / 240.f;
constexpr uint64_t kRandomSeed = 0x0DDBA11ull;

struct State {
    bool takeover = false;
    bool internalStep = false;
    bool injecting = false;
    bool internalReset = false;
    bool internalAction = false;
    bool diedThisStep = false;
    bool reachedEndThisStep = false;
    int deathObjectID = 0;
    GameObject* deathObject = nullptr;
    std::string deathCallers;
    float deathPlayerRect[4] = {0.f, 0.f, 0.f, 0.f};
    float deathVehicleSize = 0.f;
    int deathModeFlags = 0;
    bool traceActivations = false;
    int lastTracedRing = -1;
    int tick = 0;
    uint64_t totalSteps = 0;
    int commandsThisStep = 0;
    uint64_t activationHash = 0;
    uint32_t activationCount = 0;
    bool holding[2] = {false, false};
    bool holdingLeft[2] = {false, false};
    bool holdingRight[2] = {false, false};
};
inline cocos2d::CCRect peekRect(GameObject* o) {
    bool rd = o->m_isObjectRectDirty, od = o->m_isOrientedBoxDirty;
    auto r = o->getObjectRect();
    o->m_isObjectRectDirty = rd;
    o->m_isOrientedBoxDirty = od;
    return r;
}

State& state();

void beginTakeover(PlayLayer* pl);
bool checkHeap(char const* where, int tick);
void setHeapCheckEvery(int ticks);
int heapCheckEvery();
void setFastActions(bool on);
void setItemBaseReset(bool on);
void resetAreaEffectState(PlayLayer* pl);
void setAreaReset(bool on);
void setStartPlayerApply(bool on);
void endTakeover(PlayLayer* pl);

void applyButton(PlayLayer* pl, bool down, int button, bool player2);
void syncHolding(bool p1Down, bool p2Down);
void releaseAll(PlayLayer* pl);
void applyDueInputs(PlayLayer* pl, InputList const& inputs, size_t& cursor);
float perTickForSpeed(float playerSpeed);
void setTraceWindow(float lo, float hi);

void step(PlayLayer* pl);
void seedVarianceRandom();

uint32_t* eclipseExpectedTicks();

bool playerReversed(cocos2d::CCNode* player);
float playerDirection(cocos2d::CCNode* player);

void resetToStart(PlayLayer* pl, bool hard = false);
void restoreStart(PlayLayer* pl);
void onAfterReset(PlayLayer* pl);
void resetPersistentItems(PlayLayer* pl);

void seedRandom(uint64_t seed);
uint64_t currentSeed();

float playerX(PlayLayer* pl);
float playerY(PlayLayer* pl);
float percent(PlayLayer* pl);
bool isDead(PlayLayer* pl);

void hashState(PlayLayer* pl, uint64_t& hash);

void noteActivation(GameObject* object, uint32_t kind);

}
