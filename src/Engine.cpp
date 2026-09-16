#include "Engine.hpp"

#include "Snapshot.hpp"

#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/TimerItem.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GameToolbox.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <malloc.h>
#endif

#include "Bot.hpp"

using namespace geode::prelude;

namespace gdpf::engine {

namespace {
    struct ActionManagerAccess : public cocos2d::CCActionManager {
        using cocos2d::CCActionManager::update;
    };

    constexpr size_t kCounterOwnerOffset = 2168;
    constexpr size_t kCounterFirstDword = 188;
    bool s_countersCaptured = false;
    float s_initialTimeWarp = 1.f;
    bool s_initialWarpCaptured = false;
    int32_t s_counters[3] = {0, 0, 0};
    int s_heapCheckEvery = 8192;
    bool s_heapCorrupt = false;
    bool s_fastActions = true;

    struct PersistentItems {
        std::unordered_map<int, int> counts;
        std::unordered_map<int, int> persistentCounts;
        std::unordered_map<int, TimerItem> timers;
    };
    std::map<int, PersistentItems> s_persistentBaseline;
    bool s_itemBaseReset = false;
    bool s_areaReset = true;
    bool s_startPlayerApply = true;
    std::filesystem::path itemBasePath(int id) {
        return geode::Mod::get()->getSaveDir() / "itembase" / fmt::format("{}.txt", id);
    }

    bool loadItemBaseline(int id, PersistentItems& b) {
        std::ifstream f(itemBasePath(id));
        if (!f) return false;
        char kind = 0;
        int key = 0, value = 0;
        bool any = false;
        while (f >> kind >> key >> value) {
            if (kind == 'c') b.counts[key] = value;
            else if (kind == 'p') b.persistentCounts[key] = value;
            else continue;
            any = true;
        }
        return any || f.eof();
    }

    void saveItemBaseline(int id, PersistentItems const& b) {
        std::error_code ec;
        std::filesystem::create_directories(itemBasePath(id).parent_path(), ec);
        std::ofstream f(itemBasePath(id));
        if (!f) return;
        for (auto const& [k, v] : b.counts) f << "c " << k << " " << v << "\n";
        for (auto const& [k, v] : b.persistentCounts) f << "p " << k << " " << v << "\n";
    }

    struct StartPlayers { bool captured = false; PlayerSnap p1, p2; bool hasP2 = false; };
    std::map<int, StartPlayers> s_startPlayers;

    SnapshotPtr s_startSnapshot;
    int s_startSnapshotLevel = -1;

    struct AttemptHome { bool captured = false; int attempts = 0, saved = 0; };
    std::map<int, AttemptHome> s_attemptHomes;

    int32_t* resetCounters(PlayLayer* pl) {
        if (!pl) return nullptr;
        auto base = reinterpret_cast<char*>(static_cast<GJBaseGameLayer*>(pl));
        auto owner = *reinterpret_cast<char**>(base + kCounterOwnerOffset);
        if (!owner) return nullptr;
        return reinterpret_cast<int32_t*>(owner + kCounterFirstDword * sizeof(int32_t));
    }

    void pinResetCounters(PlayLayer* pl) {
        auto c = resetCounters(pl);
        if (!c) return;
        if (!s_countersCaptured) {
            s_countersCaptured = true;
            s_counters[0] = c[0];
            s_counters[1] = c[1];
            s_counters[2] = c[2];
            log::debug("[engine] reset counters captured: {} {} {}", c[0], c[1], c[2]);
            return;
        }
        c[0] = s_counters[0];
        c[1] = s_counters[1];
        c[2] = s_counters[2];
    }
}

static State s_state;

State& state() { return s_state; }

namespace { void installFatalProbe(); }

void beginTakeover(PlayLayer* pl) {
    installFatalProbe();
    resetAreaEffectState(pl);
    s_state = State{};
    s_state.takeover = true;
    s_countersCaptured = false;
    s_initialWarpCaptured = false;
    if (pl) {
        auto base = reinterpret_cast<char*>(static_cast<GJBaseGameLayer*>(pl));
        log::debug("[engine] layer offsets: effectManager={} gameState={} objects={} counterOwner={}",
            reinterpret_cast<char*>(&pl->m_effectManager) - base,
            reinterpret_cast<char*>(&pl->m_gameState) - base,
            reinterpret_cast<char*>(&pl->m_objects) - base,
            fmt::ptr(*reinterpret_cast<void**>(base + kCounterOwnerOffset)));
    }
    if (pl && pl->m_effectManager && pl->m_level) {
        int id = pl->m_level->m_levelID;
        if (!s_persistentBaseline.count(id)) {
            auto em = pl->m_effectManager;
            PersistentItems b;
            b.timers.insert(em->m_timerItemMap.begin(), em->m_timerItemMap.end());
            bool pinned = !s_itemBaseReset && loadItemBaseline(id, b);
            if (!pinned) {
                b.counts.insert(em->m_itemCountMap.begin(), em->m_itemCountMap.end());
                b.persistentCounts.insert(em->m_persistentItemCountMap.begin(), em->m_persistentItemCountMap.end());
                saveItemBaseline(id, b);
            }
            log::info("[engine] item state {} for level {}: {} counters, {} persistent, {} timers", 
                pinned ? "loaded from the pinned baseline" : "captured and pinned to disk", id,
                b.counts.size(), b.persistentCounts.size(), b.timers.size());
            for (auto const& [k, v] : b.counts) if (k <= 4) log::info("[engine]   counter {} = {}", k, v);
            for (auto const& [k, v] : b.persistentCounts) if (k <= 4) log::info("[engine]   persistent {} = {}", k, v);
            s_persistentBaseline[id] = std::move(b);
        }
        auto& sp = s_startPlayers[id];
        if (!sp.captured && pl->m_player1) {
            sp.p1.capture(pl->m_player1);
            sp.hasP2 = pl->m_player2 != nullptr;
            if (sp.hasP2) sp.p2.capture(pl->m_player2);
            sp.captured = true;
            log::info("[engine] start player state captured for level {}", id);
        }
    }
    if (pl) {
        pl->m_queuedButtons.clear();
        pl->m_clickBetweenSteps = false;
        pl->m_clickOnSteps = false;
        s_initialTimeWarp = pl->m_gameState.m_timeWarp;
        if (!(s_initialTimeWarp > 0.f)) s_initialTimeWarp = 1.f;
        s_initialWarpCaptured = true;
        log::debug("[engine] level loads at time warp {:.4f}", s_initialTimeWarp);
    }
    if (pl) setDynamicObjects(collectSnapshotObjects(pl));
    log::info("[engine] takeover begin");
}

void endTakeover(PlayLayer* pl) {
    if (pl && s_state.takeover) releaseAll(pl);
    s_state.takeover = false;
    s_state.internalStep = false;
    s_state.injecting = false;
    s_state.internalReset = false;
    log::info("[engine] takeover end ({} total steps)", s_state.totalSteps);
}

void applyButton(PlayLayer* pl, bool down, int button, bool player2) {
    if (!pl) return;
    int idx = player2 ? 1 : 0;
    bool* slot = button == 2 ? s_state.holdingLeft : button == 3 ? s_state.holdingRight : s_state.holding;
    if (slot[idx] == down) return;
    slot[idx] = down;
    s_state.injecting = true;
    pl->handleButton(down, button, !player2);
    s_state.injecting = false;
}
namespace {
    void* s_veh = nullptr;

    long __stdcall fatalProbe(_EXCEPTION_POINTERS* info) {
        if (!info || !info->ExceptionRecord) return 0;   // EXCEPTION_CONTINUE_SEARCH
        DWORD code = info->ExceptionRecord->ExceptionCode;
        switch (code) {
            case 0xC0000005: case 0xC00000FDu: case 0xC000001Du: case 0xC0000096u:
            case 0xC0000374u: case 0xC0000094u: case 0xC0000409u: case 0x80000003u:
                break;
            default: return 0;
        }
        static int seen = 0;
        if (seen++ < 8) {
            auto addr = (uintptr_t) info->ExceptionRecord->ExceptionAddress;
            uintptr_t base = (uintptr_t) GetModuleHandleA(nullptr);
            HMODULE mod = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR) addr, &mod) && mod) {
                GetModuleFileNameA(mod, name, MAX_PATH);
            }
            char const* slash = std::strrchr(name, '\\');
            log::error("[fatal] code {:#x} at {:#x} ({}+{:#x}) exe+{:#x} tick {} params {} {:#x}",
                (uint32_t) code, addr, slash ? slash + 1 : name,
                mod ? addr - (uintptr_t) mod : 0, addr - base, s_state.tick,
                info->ExceptionRecord->NumberParameters,
                info->ExceptionRecord->NumberParameters > 1
                    ? info->ExceptionRecord->ExceptionInformation[1] : 0);
        }
        return 0;
    }

    void installFatalProbe() {
        if (s_veh) return;
        s_veh = AddVectoredExceptionHandler(1, (PVECTORED_EXCEPTION_HANDLER) fatalProbe);
        log::info("[fatal] vectored probe installed ({})", s_veh ? "ok" : "failed");
    }
}

void syncHolding(bool p1Down, bool p2Down) {
    s_state.holding[0] = p1Down;
    s_state.holding[1] = p2Down;
    s_state.holdingLeft[0] = s_state.holdingLeft[1] = false;
    s_state.holdingRight[0] = s_state.holdingRight[1] = false;
}

void releaseAll(PlayLayer* pl) {
    for (int p = 0; p < 2; p++) {
        for (int b = 1; b <= 3; b++) applyButton(pl, false, b, p == 1);
    }
}

float perTickForSpeed(float playerSpeed) {
    static constexpr float kPer[5] = {1.046843f, 1.29825f, 1.614250f, 1.949215f, 2.400f};
    int r = playerSpeed < 0.8f ? 0 : playerSpeed < 1.0f ? 1 : playerSpeed < 1.2f ? 2
          : playerSpeed < 1.45f ? 3 : 4;
    return kPer[r];
}

static float s_traceLo = 0.f;
static float s_traceHi = -1.f;
void setTraceWindow(float lo, float hi) { s_traceLo = lo; s_traceHi = hi; }

void applyDueInputs(PlayLayer* pl, InputList const& inputs, size_t& cursor) {
    float px = playerX(pl);
    bool seen[2][3] = {};
    while (cursor < inputs.size()) {
        auto const& ev = inputs[cursor];
        // atX < 0 means the event is tick-scheduled, which is every path the solver writes.
        bool due = ev.atX >= 0.f ? px >= ev.atX : ev.tick <= s_state.tick;
        if (!due) break;
        int bi = ev.button >= 1 && ev.button <= 3 ? ev.button - 1 : 0;
        int pi = ev.player2 ? 1 : 0;
        // One state change per button per tick. A recorder's release/press pair sits two
        // frames apart; collapsing both into one tick would delete the released frame.
        if (ev.atX >= 0.f && seen[pi][bi]) break;
        seen[pi][bi] = true;
        applyButton(pl, ev.down, ev.button, ev.player2);
        cursor++;
    }
}

static constexpr size_t kPlayerReversedOffset = 2498;

bool playerReversed(cocos2d::CCNode* player) {
    if (!player) return false;
    return *reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(player) + kPlayerReversedOffset) != 0;
}

float playerDirection(cocos2d::CCNode* player) { return playerReversed(player) ? -1.f : 1.f; }

uint64_t* varianceRandom() {
#if defined(GEODE_IS_WINDOWS) && GEODE_COMP_GD_VERSION == 22081
    return reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(geode::base::get()) + 0x6c2ee0);
#else
    return nullptr;
#endif
}

void seedVarianceRandom() {
    if (auto g = varianceRandom()) *g = kRandomSeed;
}

uint32_t* eclipseExpectedTicks() {
#if defined(GEODE_IS_WINDOWS) && GEODE_COMP_GD_VERSION == 22081
    static unsigned char* site = reinterpret_cast<unsigned char*>(geode::base::get()) + 0x237a5b;
    static bool checked = false;
    if (!checked) {
        checked = true;
        static constexpr unsigned char kOriginal[] = {0xF3, 0x0F, 0x10, 0x86, 0x30, 0x03, 0x00, 0x00};
        bool original = std::memcmp(site, kOriginal, sizeof kOriginal) == 0;
        bool patched = site[0] == 0x48 && site[1] == 0xB8;
        if (!original && !patched)
            log::warn("[engine] unknown code at GJBaseGameLayer::update+0x20b");
        else log::info("[engine] substep count: {}", patched ? "eclipse tps bypass" : "vanilla");
    }
    if (site[0] != 0x48 || site[1] != 0xB8) return nullptr;
    uint64_t imm = 0;
    std::memcpy(&imm, site + 2, sizeof imm);
    return reinterpret_cast<uint32_t*>(imm);
#else
    return nullptr;
#endif
}

void setHeapCheckEvery(int ticks) { s_heapCheckEvery = ticks < 0 ? 0 : ticks; }
int heapCheckEvery() { return s_heapCheckEvery; }
void setFastActions(bool on) { s_fastActions = on; }
void setItemBaseReset(bool on) { s_itemBaseReset = on; }
bool checkHeap(char const* where, int tick) {
#ifdef GEODE_IS_WINDOWS
    if (s_heapCorrupt) return false;
    int crt = _heapchk();
    bool ok = crt == _HEAPOK || crt == _HEAPEMPTY;
    int bad = -1, nheaps = 0;
    if (ok) {
        HANDLE heaps[64];
        nheaps = (int) GetProcessHeaps(64, heaps);
        for (int i = 0; i < nheaps && i < 64; i++) {
            if (HeapValidate(heaps[i], 0, nullptr)) continue;
            ok = false;
            bad = i;
            break;
        }
    }
    if (!ok) {
        s_heapCorrupt = true;
        log::error("[engine] heap already corrupt at {} tick {} (crt status {}, heap #{} of {});"
                   " the damage happened before this point", where, tick, crt, bad, nheaps);
        return false;
    }
    log::debug("[engine] heap clean at {} tick {} ({} heaps)", where, tick, nheaps);
#endif
    return true;
}

void step(PlayLayer* pl) {
    s_state.diedThisStep = false;
    s_state.reachedEndThisStep = false;
    s_state.commandsThisStep = 0;
    pl->m_queuedButtons.clear();
    pl->m_resumeTimer = 0;
    auto progressBefore = pl->m_gameState.m_currentProgress;
    double timeBefore = pl->m_gameState.m_levelTime;
    float warp = std::min(1.f, pl->m_gameState.m_timeWarp);
    if (!(warp > 0.f)) warp = 1.f;
    float dt = warp * kStepDt;
    s_state.internalStep = true;
    pl->update(kStepDt);
    if (s_state.takeover && s_fastActions && Bot::get().fastStepping()) {
        if (auto am = cocos2d::CCDirector::sharedDirector()->getActionManager()) {
            s_state.internalAction = true;
            static_cast<ActionManagerAccess*>(am)->update(dt);
            s_state.internalAction = false;
        }
    }
    s_state.internalStep = false;
    if (!s_state.reachedEndThisStep && pl->m_endXPosition > 0.f && playerX(pl) >= pl->m_endXPosition) {
        s_state.reachedEndThisStep = true;
    }
    if (s_traceHi > s_traceLo) {
        float tx = playerX(pl);
        if (tx >= s_traceLo && tx <= s_traceHi) {
            auto* tp = pl->m_player1;
            log::debug("[etrace] t={} x={:.3f} y={:.3f} vy={:.4f} g={} ground={} mode={:x} rings={} tring={} buf={} hold={} srj={} rset={} tset={} tcr={} spd={:.4f} grav={:.4f} size={:.2f} ystart={:.4f} warp={:.4f} slope={} dash={} dead={} cd={} lock={} stop={} follow={} nfol={} force={} aff={} oob={} gmod={:.3f} xvr={:.3f} xvr2={:.3f} nostickx={} boostx={}",
                s_state.tick, tx, playerY(pl), tp ? tp->m_yVelocity : 0.0,
                tp && tp->m_isUpsideDown ? 1 : 0, tp && tp->m_isOnGround ? 1 : 0,
                tp ? (int) ((tp->m_isBall ? 1 : 0) | (tp->m_isShip ? 2 : 0) | (tp->m_isBird ? 4 : 0)
                            | (tp->m_isDart ? 8 : 0) | (tp->m_isRobot ? 16 : 0) | (tp->m_isSpider ? 32 : 0)
                            | (tp->m_isSwing ? 64 : 0)) : 0,
                tp && tp->m_touchingRings ? tp->m_touchingRings->count() : 0,
                tp && tp->m_touchedRing ? 1 : 0, tp && tp->m_jumpBuffered ? 1 : 0,
                s_state.holding[0] ? 1 : 0,
                tp && tp->m_stateRingJump ? 1 : 0,
                tp ? (int) tp->m_ringRelatedSet.size() : -1,
                tp ? (int) tp->m_touchedRings.size() : -1,
                tp && tp->m_touchedCustomRing ? 1 : 0,
                tp ? tp->m_playerSpeed : 0.0, tp ? tp->m_gravityMod : 0.0,
                tp ? tp->m_vehicleSize : 0.0, tp ? tp->m_yStart : 0.0,
                pl->m_gameState.m_timeWarp,
                tp && tp->m_isOnSlope ? 1 : 0, tp && tp->m_isDashing ? 1 : 0,
                tp && tp->m_isDead ? 1 : 0, tp && tp->m_controlsDisabled ? 1 : 0,
                tp && tp->m_isLocked ? 1 : 0, tp && tp->m_maybeHasStopped ? 1 : 0,
                tp ? tp->m_followRelated : -1,
                tp ? (int) tp->m_playerFollowFloats.size() : -1,
                tp ? tp->m_stateForce : -1, tp && tp->m_affectedByForces ? 1 : 0,
                tp && tp->m_isOutOfBounds ? 1 : 0, tp ? tp->m_gravityMod : 0.f,
                tp ? tp->m_xVelocityRelated : 0.f, tp ? tp->m_xVelocityRelated2 : 0.f,
                tp ? (int) tp->m_stateNoStickX : -1, tp ? tp->m_stateBoostX : -1);
        }
    }
    if (s_state.totalSteps < 4) {
        log::debug("[engine] tick {} -> progress {}->{} levelTime {:.5f}->{:.5f} commands={} x={:.3f} y={:.3f} vy={:.4f} ground={}",
            s_state.tick, progressBefore, pl->m_gameState.m_currentProgress, timeBefore, pl->m_gameState.m_levelTime,
            s_state.commandsThisStep, playerX(pl), playerY(pl), pl->m_player1 ? pl->m_player1->m_yVelocity : 0.0,
            pl->m_player1 ? pl->m_player1->m_isOnGround : false);
    }
    s_state.tick++;
    s_state.totalSteps++;
}

void restoreStart(PlayLayer* pl) {
    if (!pl) return;
    if (s_startSnapshot && pl->m_level && s_startSnapshotLevel == pl->m_level->m_levelID) {
        releaseAll(pl);
        if (restoreSnapshot(pl, *s_startSnapshot)) return;
        log::warn("[engine] start snapshot restore failed; falling back to resetToStart");
    }
    resetToStart(pl);
}

void resetToStart(PlayLayer* pl, bool hard) {
    releaseAll(pl);
    clearCollisionLogs(pl);
    pl->stopAllActions();
    if (pl->m_checkpointArray) pl->m_checkpointArray->removeAllObjects();
    pl->m_currentCheckpoint = nullptr;
    pl->m_isPracticeMode = false;
    s_state.internalReset = true;
    seedRandom(kRandomSeed);
    seedVarianceRandom();
    pl->m_replayRandSeed = kRandomSeed;
    if (hard) pl->resetLevelFromStart();
    else pl->resetLevel();
    resetPersistentItems(pl);
    if (pl->m_level && s_startPlayerApply) {
        auto it = s_startPlayers.find(pl->m_level->m_levelID);
        if (it != s_startPlayers.end() && it->second.captured) {
            it->second.p1.apply(pl->m_player1);
            if (it->second.hasP2 && pl->m_player2) it->second.p2.apply(pl->m_player2);
        }
    }
    pl->m_solidCollisionObjectsCount = 0;
    pl->m_solidCollisionObjectsIndex = 0;
    pl->m_hazardCollisionObjectsCount = 0;
    pl->m_hazardCollisionObjectsIndex = 0;
    pl->m_queuedButtons.clear();
    pl->m_queuedRecordedButtons.clear();
    pl->m_activeObjectsCount = 0;
    pl->m_activeObjectsIndex = 0;
    if (pl->m_objects) {
        for (auto o : geode::cocos::CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!o) continue;
            o->m_isObjectRectDirty = true;
            o->m_isOrientedBoxDirty = true;
        }
    } else {
        for (auto o : dynamicObjects()) {
            if (!o) continue;
            o->m_isObjectRectDirty = true;
            o->m_isOrientedBoxDirty = true;
        }
    }
    if (pl->m_level) {
        auto& ah = s_attemptHomes[pl->m_level->m_levelID];
        if (!ah.captured) {
            ah.attempts = pl->m_attempts; ah.saved = pl->m_savedAttempts; ah.captured = true;
            log::info("[engine] attempts pinned for level {}: {} ({} saved)", pl->m_level->m_levelID, ah.attempts, ah.saved);
        } else {
            pl->m_attempts = ah.attempts;
            pl->m_savedAttempts = ah.saved;
        }
    }
    if (s_initialWarpCaptured) {
        pl->m_gameState.m_timeWarp = s_initialTimeWarp;
        pl->m_gameState.m_queuedTimeWarp = s_initialTimeWarp;
    }
    if (pl->m_level && s_startSnapshotLevel != pl->m_level->m_levelID) {
        s_startSnapshot = takeSnapshot(pl);
        if (s_startSnapshot) {
            s_startSnapshotLevel = pl->m_level->m_levelID;
            log::info("[engine] start snapshot captured for level {}", s_startSnapshotLevel);
        }
    }
    pl->updatePlayerCollisionBlocks();
    pl->checkSpawnObjects();
    pl->sortSectionVector();
    pl->updateVisibility(0.f);
    pl->m_extraDelta = 0.0;
    s_state.internalReset = false;
}

void resetPersistentItems(PlayLayer* pl) {
    if (!pl || !pl->m_effectManager || !pl->m_level) return;
    auto it = s_persistentBaseline.find(pl->m_level->m_levelID);
    if (it == s_persistentBaseline.end()) return;
    auto em = pl->m_effectManager;
    auto const& b = it->second;
    em->m_itemCountMap.clear();
    em->m_itemCountMap.insert(b.counts.begin(), b.counts.end());
    em->m_persistentItemCountMap.clear();
    em->m_persistentItemCountMap.insert(b.persistentCounts.begin(), b.persistentCounts.end());
    em->m_timerItemMap.clear();
    em->m_timerItemMap.insert(b.timers.begin(), b.timers.end());
}

void noteActivation(GameObject* object, uint32_t kind) {
    if (!object) return;
    uint64_t v = ((uint64_t) kind << 32) | (uint32_t) object->m_uniqueID;
    s_state.activationHash ^= v + 0x9e3779b97f4a7c15ull + (s_state.activationHash << 6) + (s_state.activationHash >> 2);
    s_state.activationCount++;
    if (s_state.traceActivations && (kind != 2 || object->m_uniqueID != s_state.lastTracedRing)) {
        if (kind == 2) s_state.lastTracedRing = object->m_uniqueID;
        log::info("[trace] tick {} activated {} obj {} (uid {}) at ({:.1f},{:.1f})", s_state.tick,
            kind == 1 ? "trigger" : kind == 2 ? "ring" : "object", object->m_objectID, object->m_uniqueID,
            object->getPositionX(), object->getPositionY());
    }
}
void resetAreaEffectState(PlayLayer* pl) {
    if (!pl || !s_areaReset) return;
    pl->resetActiveEnterEffects();
    pl->m_areaObjects.clear();
    pl->m_processedAreaObjects.clear();
    pl->m_areaObjectsCount = 0;
    pl->m_processedAreaObjectsCount = 0;
    pl->m_areaObjectsIndex = 0;
    pl->m_processedAreaObjectsIndex = 0;
    pl->m_areaObjectsUpdated = true;
    pl->m_areaMovedCount = 0;
    pl->m_areaScaledCount = 0;
    pl->m_areaRotatedCount = 0;
    pl->m_areaColorCount = 0;
    pl->m_areaMovedCountTotal = 0;
    pl->m_areaScaledCountTotal = 0;
    pl->m_areaRotatedCountTotal = 0;
    pl->m_areaColorCountTotal = 0;
    pl->m_areaMovedCountDisplay = 0;
    pl->m_areaScaledCountDisplay = 0;
    pl->m_areaRotatedCountDisplay = 0;
    pl->m_areaColorCountDisplay = 0;
    pl->m_areaMovedCountTotalDisplay = 0;
    pl->m_areaScaledCountTotalDisplay = 0;
    pl->m_areaRotatedCountTotalDisplay = 0;
    pl->m_areaColorCountTotalDisplay = 0;
}

void setAreaReset(bool on) { s_areaReset = on; }
void setStartPlayerApply(bool on) { s_startPlayerApply = on; }

void onAfterReset(PlayLayer* pl) {
    resetAreaEffectState(pl);
    if (pl) {
        pl->m_randomSeed = kRandomSeed;
        pl->m_unk32e0 = kRandomSeed;
        pl->m_replayRandSeed = kRandomSeed;
        if (s_state.takeover) pinResetCounters(pl);
        for (auto p : {pl->m_player1, pl->m_player2}) {
            if (!p) continue;
            p->m_lastJumpTime = 0.0;
            p->m_isOnGround = false;
            p->m_isOnGround2 = false;
            p->m_isOnSlope = false;
            p->m_touchedRing = false;
            p->m_jumpBuffered = false;
        }
        pl->m_endChecked = false;
    }
    s_state.tick = 0;
    s_state.activationHash = 0;
    s_state.activationCount = 0;
    s_state.diedThisStep = false;
    s_state.reachedEndThisStep = false;
    s_state.deathObjectID = 0;
    s_state.deathObject = nullptr;
    std::memset(s_state.deathPlayerRect, 0, sizeof(s_state.deathPlayerRect));
    std::memset(s_state.holding, 0, sizeof(s_state.holding));
    std::memset(s_state.holdingLeft, 0, sizeof(s_state.holdingLeft));
    std::memset(s_state.holdingRight, 0, sizeof(s_state.holdingRight));
    if (pl) pl->m_queuedButtons.clear();
    seedRandom(kRandomSeed);
}

void seedRandom(uint64_t seed) {
    GameToolbox::fast_srand(seed);
    std::srand((unsigned int) seed);
}

uint64_t currentSeed() {
    return GameToolbox::getfast_srand();
}

float playerX(PlayLayer* pl) {
    return pl && pl->m_player1 ? pl->m_player1->getPositionX() : 0.f;
}

float playerY(PlayLayer* pl) {
    return pl && pl->m_player1 ? pl->m_player1->getPositionY() : 0.f;
}

float percent(PlayLayer* pl) {
    return pl ? pl->getCurrentPercent() : 0.f;
}

bool isDead(PlayLayer* pl) {
    return s_state.diedThisStep || (pl && pl->m_player1 && pl->m_player1->m_isDead);
}

static inline void mix(uint64_t& h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}

static inline uint64_t bitsOf(float f) {
    uint32_t u; std::memcpy(&u, &f, sizeof u); return u;
}

static inline uint64_t bitsOf(double d) {
    uint64_t u; std::memcpy(&u, &d, sizeof u); return u;
}

static void hashPlayer(PlayerObject* p, uint64_t& h) {
    if (!p) return;
    mix(h, bitsOf(p->getPositionX()));
    mix(h, bitsOf(p->getPositionY()));
    mix(h, bitsOf(p->m_yVelocity));
    uint64_t flags = 0;
    flags |= p->m_isOnGround ? 1 : 0;
    flags |= p->m_isUpsideDown ? 2 : 0;
    flags |= p->m_isShip ? 4 : 0;
    flags |= p->m_isBall ? 8 : 0;
    flags |= p->m_isBird ? 16 : 0;
    flags |= p->m_isDart ? 32 : 0;
    flags |= p->m_isRobot ? 64 : 0;
    flags |= p->m_isSpider ? 128 : 0;
    flags |= p->m_isSwing ? 256 : 0;
    flags |= p->m_isDashing ? 512 : 0;
    flags |= p->m_isDead ? 1024 : 0;
    mix(h, flags);
}

void hashState(PlayLayer* pl, uint64_t& hash) {
    if (!pl) return;
    mix(hash, (uint64_t) s_state.tick);
    hashPlayer(pl->m_player1, hash);
    if (pl->m_gameState.m_isDualMode) hashPlayer(pl->m_player2, hash);
}

}
