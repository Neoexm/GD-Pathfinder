#include "Snapshot.hpp"

#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/GameObject.hpp>

#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_set>

#include "Engine.hpp"
#include "Profile.hpp"

using namespace geode::prelude;

namespace gdpf::engine {

namespace {
    SnapshotStats s_stats;

    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
}

SnapshotStats& snapshotStats() { return s_stats; }

namespace { bool s_fastRestore = false; bool s_poseOverride = true; }
void setFastRestore(bool on) { s_fastRestore = on; }
bool fastRestore() { return s_fastRestore; }
void setPoseOverride(bool on) { s_poseOverride = on; }
bool poseOverride() { return s_poseOverride; }

void PlayerSnap::capture(PlayerObject* p) {
    if (!p) return;
    nodePosition = p->getPosition();
    nodeRotation = p->getRotation();
    nodeScaleX = p->getScaleX();
    nodeScaleY = p->getScaleY();
    nodeVisible = p->isVisible();
    positionX = p->m_positionX;
    positionY = p->m_positionY;
    positionXOffset = p->m_positionXOffset;
    positionYOffset = p->m_positionYOffset;
    unmodifiedX = p->m_unmodifiedPositionX;
    unmodifiedY = p->m_unmodifiedPositionY;
    lastPosition = p->m_lastPosition;
#define GDPF_SAVE(name) this->name = p->name;
    GDPF_PLAYER_POD_FIELDS(GDPF_SAVE)
    GDPF_PLAYER_CONTAINER_FIELDS(GDPF_SAVE)
#undef GDPF_SAVE
}

void PlayerSnap::apply(PlayerObject* p) const {
    if (!p) return;
    p->setPosition(nodePosition);
    p->setRotation(nodeRotation);
    p->setScaleX(nodeScaleX);
    p->setScaleY(nodeScaleY);
    p->setVisible(nodeVisible);
    p->m_positionX = positionX;
    p->m_positionY = positionY;
    p->m_positionXOffset = positionXOffset;
    p->m_positionYOffset = positionYOffset;
    p->m_unmodifiedPositionX = unmodifiedX;
    p->m_unmodifiedPositionY = unmodifiedY;
    p->m_lastPosition = lastPosition;
#define GDPF_LOAD(name) p->name = this->name;
    GDPF_PLAYER_POD_FIELDS(GDPF_LOAD)
    GDPF_PLAYER_CONTAINER_FIELDS(GDPF_LOAD)
#undef GDPF_LOAD
}

std::string PlayerSnap::diff(PlayerSnap const& o) const {
    std::string out;
    auto add = [&](char const* name) { if (!out.empty()) out += " "; out += name; };
    if (nodePosition.x != o.nodePosition.x || nodePosition.y != o.nodePosition.y) add("nodePosition");
    if (nodeRotation != o.nodeRotation) add("nodeRotation");
    if (positionX != o.positionX || positionY != o.positionY) add("m_positionX/Y");
    if (positionXOffset != o.positionXOffset || positionYOffset != o.positionYOffset) add("m_positionXOffset/YOffset");
    if (unmodifiedX != o.unmodifiedX || unmodifiedY != o.unmodifiedY) add("m_unmodifiedPosition");
    if (lastPosition.x != o.lastPosition.x || lastPosition.y != o.lastPosition.y) add("m_lastPosition");
#define GDPF_DIFF(name) if (std::memcmp(&this->name, &o.name, sizeof(this->name)) != 0) add(#name);
    GDPF_PLAYER_POD_FIELDS(GDPF_DIFF)
#undef GDPF_DIFF
    if (m_potentialSlopeMap != o.m_potentialSlopeMap) add("m_potentialSlopeMap");
    if (m_ringRelatedSet != o.m_ringRelatedSet) add("m_ringRelatedSet");
    if (m_touchedRings != o.m_touchedRings) add("m_touchedRings");
    if (m_playerFollowFloats != o.m_playerFollowFloats) add("m_playerFollowFloats");
    if (m_jumpPadRelated != o.m_jumpPadRelated) add("m_jumpPadRelated");
    if (m_holdingButtons != o.m_holdingButtons) add("m_holdingButtons");
    return out;
}

namespace {
    std::vector<GameObject*> s_dynamic;
    std::unordered_set<GameObject*> s_tracked;
    std::vector<EffectGameObject*> s_effect;
    std::vector<PoseState> s_basePose;
    std::vector<uint8_t> s_baseFlags;
    std::vector<int> s_baseCounter;

    std::vector<uint32_t> s_byX;

    template <typename F>
    inline void forEachLive(F&& fn) {
        for (auto i : s_byX) fn(i);
    }

    inline bool samePose(GameObject* o, PoseState const& b) {
        return o->m_positionX == b.px && o->m_positionY == b.py
            && o->m_positionXOffset == b.offX && o->m_positionYOffset == b.offY
            && o->getRotation() == b.rotation && o->getScaleX() == b.scaleX && o->getScaleY() == b.scaleY
            && o->m_lastPosition.x == b.lastX && o->m_lastPosition.y == b.lastY
            && o->m_varianceIndex == b.varianceIndex;
    }

    inline PoseState poseOf(GameObject* o, uint32_t index) {
        return PoseState{o->m_positionX, o->m_positionY, index, o->m_positionXOffset, o->m_positionYOffset,
                         o->getRotation(), o->getScaleX(), o->getScaleY(),
                         o->m_lastPosition.x, o->m_lastPosition.y, o->m_varianceIndex};
    }

    static constexpr size_t kGroupCounterOffset = 1216;
    inline int groupCounterOf(GameObject* o) { return o->m_enabledGroupsCounter; }
    inline void setGroupCounter(GameObject* o, int v) { o->m_enabledGroupsCounter = v; }

    inline uint8_t flagsOf(GameObject* o, EffectGameObject* e) {
        uint8_t f = 0;
        if (o->m_isGroupDisabled) f |= kFlagGroupDisabled;
        if (o->m_isDisabled) f |= kFlagDisabled;
        if (o->m_isDisabled2) f |= kFlagDisabled2;
        if (e && e->m_activatedByPlayer1) f |= kFlagActP1;
        if (e && e->m_activatedByPlayer2) f |= kFlagActP2;
        return f;
    }

    inline void applyPose(PlayLayer* pl, GameObject* o, PoseState const& st) {
        bool moved = std::fabs((o->m_positionX + o->m_positionXOffset) - (st.px + st.offX)) > 1e-4
                  || std::fabs((o->m_positionY + o->m_positionYOffset) - (st.py + st.offY)) > 1e-4;
        if (moved || o->m_positionX != st.px || o->m_positionY != st.py
            || o->m_positionXOffset != st.offX || o->m_positionYOffset != st.offY) {
            o->m_positionX = st.px;
            o->m_positionY = st.py;
            o->m_positionXOffset = st.offX;
            o->m_positionYOffset = st.offY;
            o->setPosition(o->getRealPosition());
            o->m_isObjectRectDirty = true;
            o->m_isOrientedBoxDirty = true;
            moved = true;
        }
        if (o->getRotation() != st.rotation) { o->setRotation(st.rotation); o->m_isObjectRectDirty = true; o->m_isOrientedBoxDirty = true; moved = true; }
        if (o->getScaleX() != st.scaleX) { o->setScaleX(st.scaleX); o->m_isObjectRectDirty = true; o->m_isOrientedBoxDirty = true; moved = true; }
        if (o->getScaleY() != st.scaleY) { o->setScaleY(st.scaleY); o->m_isObjectRectDirty = true; o->m_isOrientedBoxDirty = true; moved = true; }
        if (moved && pl) pl->updateObjectSection(o);
        o->m_lastPosition.x = st.lastX;
        o->m_lastPosition.y = st.lastY;
        o->m_varianceIndex = st.varianceIndex;
    }

    inline void applyFlags(GameObject* o, EffectGameObject* e, uint8_t f) {
        bool v;
        if (o->m_isGroupDisabled != (v = (f & kFlagGroupDisabled) != 0)) o->m_isGroupDisabled = v;
        if (o->m_isDisabled != (v = (f & kFlagDisabled) != 0)) o->m_isDisabled = v;
        if (o->m_isDisabled2 != (v = (f & kFlagDisabled2) != 0)) o->m_isDisabled2 = v;
        if (e) {
            if (e->m_activatedByPlayer1 != (v = (f & kFlagActP1) != 0)) e->m_activatedByPlayer1 = v;
            if (e->m_activatedByPlayer2 != (v = (f & kFlagActP2) != 0)) e->m_activatedByPlayer2 = v;
        }
    }

    inline void applyObjectStates(PlayLayer* pl, Snapshot const& snap) {
        forEachLive([&](uint32_t i) {
            auto o = s_dynamic[i];
            if (!o) return;
            applyPose(pl, o, s_basePose[i]);
            applyFlags(o, s_effect[i], s_baseFlags[i]);
            setGroupCounter(o, s_baseCounter[i]);
        });
        for (auto const& ps : snap.poses) {
            if (ps.index < s_dynamic.size() && s_dynamic[ps.index]) applyPose(pl, s_dynamic[ps.index], ps);
        }
        for (auto fs : snap.flags) {
            uint32_t i = fs >> kFlagBits;
            if (i < s_dynamic.size() && s_dynamic[i]) applyFlags(s_dynamic[i], s_effect[i], (uint8_t) (fs & kFlagMask));
        }
        for (auto const& gc : snap.groupCounters) {
            if (gc.first < s_dynamic.size() && s_dynamic[gc.first]) setGroupCounter(s_dynamic[gc.first], gc.second);
        }
    }
}

namespace {
    uint64_t s_staleCollisionLogs = 0;
    void clearCollisionLogs(PlayerObject* p) {
        if (!p) return;
        cocos2d::CCDictionary* logs[] = {
            p->m_collisionLogTop, p->m_collisionLogBottom, p->m_collisionLogLeft, p->m_collisionLogRight,
        };
        for (auto* d : logs) {
            if (!d) continue;
            if (d->count() > 0) s_staleCollisionLogs++;
            d->removeAllObjects();
        }
    }

}

void clearCollisionLogs(PlayLayer* pl) {
    if (!pl) return;
    clearCollisionLogs(pl->m_player1);
    clearCollisionLogs(pl->m_player2);
}

uint64_t staleCollisionLogs() { return s_staleCollisionLogs; }

namespace { std::vector<GameObject*> s_pendingExtras, s_extraReset; }

std::vector<GameObject*> collectSnapshotObjects(PlayLayer* pl) {
    std::vector<GameObject*> out;
    s_pendingExtras.clear();
    if (!pl || !pl->m_objects) return out;
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj) continue;
        bool enhanced = typeinfo_cast<EnhancedGameObject*>(obj) != nullptr;
        if (obj->m_objectType == GameObjectType::Decoration) {
            if (enhanced) s_pendingExtras.push_back(obj);
            continue;
        }
        bool grouped = obj->m_groups && obj->m_groupCount > 0;
        if (!grouped && !typeinfo_cast<EffectGameObject*>(obj)) {
            switch (obj->m_objectType) {
                case GameObjectType::Solid:
                case GameObjectType::Hazard:
                case GameObjectType::Slope:
                case GameObjectType::Breakable:
                    if (enhanced) s_pendingExtras.push_back(obj);
                    break;
                default:
                    s_pendingExtras.push_back(obj);
                    break;
            }
            continue;
        }
        out.push_back(obj);
    }
    return out;
}

void setDynamicObjects(std::vector<GameObject*> objects) {
    s_dynamic = std::move(objects);
    s_tracked.clear();
    s_tracked.insert(s_dynamic.begin(), s_dynamic.end());
    s_extraReset = s_dynamic.empty() ? std::vector<GameObject*>{} : std::move(s_pendingExtras);
    s_pendingExtras.clear();
    if (!s_dynamic.empty()) log::info("[snapshot] tracking {} objects, {} extra reset", s_dynamic.size(), s_extraReset.size());
    s_effect.clear();
    s_effect.reserve(s_dynamic.size());
    for (auto o : s_dynamic) s_effect.push_back(typeinfo_cast<EffectGameObject*>(o));
    s_basePose.clear();
    s_basePose.reserve(s_dynamic.size());
    s_baseFlags.clear();
    s_baseFlags.reserve(s_dynamic.size());
    s_baseCounter.clear();
    s_baseCounter.reserve(s_dynamic.size());
    for (uint32_t i = 0; i < s_dynamic.size(); i++) {
        s_basePose.push_back(poseOf(s_dynamic[i], i));
        s_baseFlags.push_back(flagsOf(s_dynamic[i], s_effect[i]));
        s_baseCounter.push_back(groupCounterOf(s_dynamic[i]));
    }
    s_byX.clear();
    s_byX.reserve(s_dynamic.size());
    for (uint32_t i = 0; i < s_dynamic.size(); i++) s_byX.push_back(i);
    std::sort(s_byX.begin(), s_byX.end(), [](uint32_t a, uint32_t b) { return s_basePose[a].px < s_basePose[b].px; });
    if (s_dynamic.size() >= (1u << (32 - kFlagBits)))
        log::error("[snapshot] {} dynamic objects exceed the flag-state index range", s_dynamic.size());
    if (!s_dynamic.empty() && s_dynamic[0]) {
        auto o = s_dynamic[0];
        auto off = reinterpret_cast<char*>(&o->m_lastPosition) - reinterpret_cast<char*>(o);
        if (off != 1232) log::error("[snapshot] GameObject::m_lastPosition is at {} (expected 1232)", off);
        auto gco = reinterpret_cast<char*>(&o->m_enabledGroupsCounter) - reinterpret_cast<char*>(o);
        if (gco != (long long) kGroupCounterOffset)
            log::error("[snapshot] GameObject::m_enabledGroupsCounter is at {} (expected {})", gco, kGroupCounterOffset);
    }
}

std::vector<GameObject*> const& dynamicObjects() { return s_dynamic; }

SnapshotPtr takeSnapshot(PlayLayer* pl) {
    if (!pl) return nullptr;
    double t0 = now();
    auto snap = std::make_shared<Snapshot>();
    auto cp = pl->createCheckpoint();
    if (!cp) {
        log::warn("[snapshot] createCheckpoint returned null at tick {}", state().tick);
        return nullptr;
    }
    snap->checkpoint = cp;
    forEachLive([&](uint32_t i) {
        auto o = s_dynamic[i];
        if (!o) return;
        if (!samePose(o, s_basePose[i])) snap->poses.push_back(poseOf(o, i));
        uint8_t f = flagsOf(o, s_effect[i]);
        if (f != s_baseFlags[i]) snap->flags.push_back((i << kFlagBits) | f);
        int gc = groupCounterOf(o);
        if (gc != s_baseCounter[i]) snap->groupCounters.emplace_back(i, gc);
    });
    std::sort(snap->poses.begin(), snap->poses.end(), [](PoseState const& a, PoseState const& b) { return a.index < b.index; });
    std::sort(snap->flags.begin(), snap->flags.end());
    snap->p1.capture(pl->m_player1);
    snap->hasP2 = pl->m_gameState.m_isDualMode && pl->m_player2;
    if (snap->hasP2) snap->p2.capture(pl->m_player2);
    snap->seed = currentSeed();
    snap->extraDelta = pl->m_extraDelta;
    auto& st = state();
    snap->tick = st.tick;
    snap->activationHash = st.activationHash;
    snap->activationCount = st.activationCount;
    for (int i = 0; i < 2; i++) {
        snap->holding[i] = st.holding[i];
        snap->holdingLeft[i] = st.holdingLeft[i];
        snap->holdingRight[i] = st.holdingRight[i];
    }
    snap->x = playerX(pl);
    snap->y = playerY(pl);
    if (auto em = pl->m_effectManager) {
        snap->itemCounts.insert(em->m_itemCountMap.begin(), em->m_itemCountMap.end());
        snap->persistentCounts.insert(em->m_persistentItemCountMap.begin(), em->m_persistentItemCountMap.end());
        snap->timers.insert(em->m_timerItemMap.begin(), em->m_timerItemMap.end());
    }
    snap->progress = pl->m_gameState.m_currentProgress;
    snap->levelTime = pl->m_gameState.m_levelTime;
    snap->practiceMode = pl->m_isPracticeMode;
    s_stats.taken++;
    s_stats.poseStates += snap->poses.size();
    s_stats.flagStates += snap->flags.size();
    s_stats.takeSeconds += now() - t0;
    return snap;
}

bool restoreSnapshot(PlayLayer* pl, Snapshot const& snap) {
    if (!pl || !snap.checkpoint) return false;
    clearCollisionLogs(pl);
    double t0 = now();
    auto& st = state();

    Ref<CheckpointObject> nowCp;
    if (s_fastRestore) { profile::Scope sc("fast: createCheckpoint (now)"); nowCp = pl->createCheckpoint(); }

    pl->m_checkpointArray->removeAllObjects();
    pl->m_checkpointArray->addObject(snap.checkpoint);
    pl->m_currentCheckpoint = snap.checkpoint;
    bool wasPractice = pl->m_isPracticeMode;
    pl->m_isPracticeMode = true;
    st.internalReset = true;
    seedRandom(kRandomSeed);
    pl->m_replayRandSeed = kRandomSeed;
    if (s_fastRestore) {
        seedVarianceRandom();
        { profile::Scope sc("fast: em reset + level vars"); if (auto em = pl->m_effectManager) em->reset(); pl->resetLevelVariables(); pl->resetActiveEnterEffects(); }
        pl->m_areaObjectsUpdated = true;
        pl->m_skipArtReload = false;
        pl->m_unk3251 = false;
        pl->m_currentStep = 0;
        pl->m_activeGravityEffects = 0;
        pl->m_gravityEffectIndex = 0;
        pl->m_hasJumped = false;
        pl->m_endLayerStars = false;
        pl->m_orbs = 0;
        pl->m_secretKey = false;
        pl->m_unk3900 = 0.f;
        pl->m_tryPlaceCheckpoint = false;
        pl->m_activatedCheckpoint = nullptr;
        {
            profile::Scope sc("fast: resetObject (tracked)");
            forEachLive([&](uint32_t i) {
                auto o = s_dynamic[i];
                if (!o) return;
                bool moved = !samePose(o, s_basePose[i]);
                o->resetObject();
                if (moved) pl->updateObjectSection(o);
            });
        }
        {
            profile::Scope sc("fast: resetObject (animated, untracked)");
            for (auto o : s_extraReset) o->resetObject();
        }
        if (nowCp) {
            profile::Scope sc("fast: resetObject (displaced, untracked)");
            for (auto const& r : nowCp->m_vectorSavedObjectStateRef) {
                auto o = r.m_gameObject;
                if (!o || s_tracked.count(o)) continue;
                o->resetObject();
                pl->updateObjectSection(o);
            }
        }
        { profile::Scope sc("fast: loadDefaultColors + resetPlayer"); pl->loadDefaultColors(); pl->resetPlayer(); }
        pl->loadFromCheckpoint(pl->m_currentCheckpoint);
        pl->resetRecord(pl->m_gameState.m_unkUint4, false);
        if (auto em = pl->m_effectManager) {
            static bool checked = false;
            if (!checked) {
                checked = true;
                auto base = reinterpret_cast<char*>(em);
                auto o1 = reinterpret_cast<char*>(&em->m_unk3f0) - base, o2 = reinterpret_cast<char*>(&em->m_unk430) - base;
                if (o1 != 1008 || o2 != 1072)
                    log::error("[snapshot] GJEffectManager layout: m_unk3f0 at {} m_unk430 at {} (expected 1008 / 1072)", o1, o2);
            }
            em->m_unk3f0.clear();
            em->m_unk430.clear();
        }
        { profile::Scope sc("fast: applyObjectStates"); applyObjectStates(pl, snap); }
        { profile::Scope sc("fast: collision blocks + spawn objects"); pl->updatePlayerCollisionBlocks(); pl->checkSpawnObjects(); }
        { profile::Scope sc("fast: camera + colors"); pl->updateLevelColors(); pl->updateCamera(0.f); }
        pl->updateVisibility(0.f);
        pl->removeReleasedButtons();
        { profile::Scope sc("fast: sortSectionVector"); pl->sortSectionVector(); }
        { profile::Scope sc("fast: onAfterReset"); onAfterReset(pl); }
    } else {
        pl->resetLevel();
        resetAreaEffectState(pl);
    }
    st.internalReset = false;
    pl->m_isPracticeMode = wasPractice;
    if (auto em = pl->m_effectManager) {
        em->m_itemCountMap.clear();
        em->m_itemCountMap.insert(snap.itemCounts.begin(), snap.itemCounts.end());
        em->m_persistentItemCountMap.clear();
        em->m_persistentItemCountMap.insert(snap.persistentCounts.begin(), snap.persistentCounts.end());
        em->m_timerItemMap.clear();
        em->m_timerItemMap.insert(snap.timers.begin(), snap.timers.end());
    }

    for (int i = 0; i < 2; i++) {
        if (snap.holding[i]) applyButton(pl, true, 1, i == 1);
        if (snap.holdingLeft[i]) applyButton(pl, true, 2, i == 1);
        if (snap.holdingRight[i]) applyButton(pl, true, 3, i == 1);
    }
    snap.p1.apply(pl->m_player1);
    if (snap.hasP2 && pl->m_player2) snap.p2.apply(pl->m_player2);

    if (s_fastRestore || s_poseOverride) applyObjectStates(pl, snap);

    {
        static int reported = 0;
        double drift = (double) pl->m_gameState.m_levelTime - (double) snap.levelTime;
        if (std::fabs(drift) > 0.0005 && reported < 5) {
            reported++;
            log::warn("[snapshot] level time drift at tick {}: restore gave {:.4f}, snapshot had {:.4f} (off by {:.4f}s)",
                snap.tick, pl->m_gameState.m_levelTime, snap.levelTime, drift);
        }
        pl->m_gameState.m_levelTime = snap.levelTime;
    }
    seedRandom(snap.seed);
    pl->m_extraDelta = snap.extraDelta;
    st.tick = snap.tick;
    st.activationHash = snap.activationHash;
    st.activationCount = snap.activationCount;
    st.diedThisStep = false;
    st.reachedEndThisStep = false;
    st.deathObjectID = 0;
    st.deathObject = nullptr;
    std::memset(st.deathPlayerRect, 0, sizeof(st.deathPlayerRect));
    for (int i = 0; i < 2; i++) {
        st.holding[i] = snap.holding[i];
        st.holdingLeft[i] = snap.holdingLeft[i];
        st.holdingRight[i] = snap.holdingRight[i];
    }
    pl->m_queuedButtons.clear();

    s_stats.restored++;
    s_stats.restoreSeconds += now() - t0;

    float dx = std::fabs(playerX(pl) - snap.x);
    float dy = std::fabs(playerY(pl) - snap.y);
    if (dx > 0.001f || dy > 0.001f || pl->m_gameState.m_currentProgress != snap.progress) {
        s_stats.restoreMismatches++;
        log::warn("[snapshot] restore mismatch: tick {} expected ({:.3f},{:.3f}) progress {} got ({:.3f},{:.3f}) progress {}",
            snap.tick, snap.x, snap.y, snap.progress, playerX(pl), playerY(pl), pl->m_gameState.m_currentProgress);
        return false;
    }
    return true;
}

}
