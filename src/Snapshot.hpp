#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/TimerItem.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "PlayerFields.hpp"

namespace gdpf::engine {

struct PlayerSnap {
    cocos2d::CCPoint nodePosition;
    float nodeRotation = 0.f;
    float nodeScaleX = 1.f;
    float nodeScaleY = 1.f;
    bool nodeVisible = true;
    double positionX = 0.0, positionY = 0.0;
    float positionXOffset = 0.f, positionYOffset = 0.f;
    float unmodifiedX = 0.f, unmodifiedY = 0.f;
    cocos2d::CCPoint lastPosition;
#define GDPF_DECL(name) decltype(PlayerObject::name) name;
    GDPF_PLAYER_POD_FIELDS(GDPF_DECL)
    GDPF_PLAYER_CONTAINER_FIELDS(GDPF_DECL)
#undef GDPF_DECL

    void capture(PlayerObject* p);
    void apply(PlayerObject* p) const;
    std::string diff(PlayerSnap const& other) const;
};

struct PoseState {
    double px, py;
    uint32_t index;
    float offX, offY;
    float rotation, scaleX, scaleY;
    float lastX, lastY;
    int varianceIndex;
};

using FlagState = uint32_t;
enum : uint32_t {
    kFlagGroupDisabled = 1,
    kFlagDisabled = 2,
    kFlagDisabled2 = 4,
    kFlagActP1 = 8,
    kFlagActP2 = 16,
    kFlagBits = 5,
    kFlagMask = (1u << kFlagBits) - 1,
};

struct Snapshot {
    geode::Ref<CheckpointObject> checkpoint;
    PlayerSnap p1;
    PlayerSnap p2;
    bool hasP2 = false;
    std::vector<PoseState> poses;
    std::vector<FlagState> flags;
    std::vector<std::pair<uint32_t, int>> groupCounters;
    uint64_t seed = 0;
    uint64_t activationHash = 0;
    uint32_t activationCount = 0;
    int tick = 0;
    double extraDelta = 0.0;
    std::unordered_map<int, int> itemCounts, persistentCounts;
    std::unordered_map<int, TimerItem> timers;
    bool holding[2] = {false, false};
    bool holdingLeft[2] = {false, false};
    bool holdingRight[2] = {false, false};
    float x = 0.f;
    float y = 0.f;
    unsigned progress = 0;
    double levelTime = 0.0;
    bool practiceMode = false;
};

using SnapshotPtr = std::shared_ptr<Snapshot>;

void setPoseOverride(bool on);
bool poseOverride();
void setDynamicObjects(std::vector<GameObject*> objects);
std::vector<GameObject*> const& dynamicObjects();

std::vector<GameObject*> collectSnapshotObjects(PlayLayer* pl);

SnapshotPtr takeSnapshot(PlayLayer* pl);
// Empty both players' per-frame collision logs. The game does this at the top of every
// update, a restore has to do it too, or entries from the abandoned timeline survive.
void clearCollisionLogs(PlayLayer* pl);

// How many times a restore found a collision log that outlived its timeline.
uint64_t staleCollisionLogs();

bool restoreSnapshot(PlayLayer* pl, Snapshot const& snap);

void setFastRestore(bool on);
bool fastRestore();

struct SnapshotStats {
    uint64_t taken = 0;
    uint64_t restored = 0;
    double takeSeconds = 0.0;
    double restoreSeconds = 0.0;
    uint64_t restoreMismatches = 0;
    uint64_t poseStates = 0;
    uint64_t flagStates = 0;
};
SnapshotStats& snapshotStats();

}
