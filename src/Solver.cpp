#include "Solver.hpp"
#include "BallPlan.hpp"
#include "BallExact.hpp"
#include "ShipPlan.hpp"

#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Corridor.hpp"
#include "Engine.hpp"
#include "Snapshot.hpp"

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <malloc.h>
#include <psapi.h>
#endif

using namespace geode::prelude;

namespace gdpf {

namespace {
    std::string s_dumpPrefix;
    int s_dumpCount = 0;
    float kRoomWeight = 0.5f;
    int kFunnelDepth = 240;
    int kDoomPenalty = 600;
    int kLaneCap = 200;
    float kBucketFloor = 1.f;
    int kKeyBonusArg = 400;
    int kItemPull = 10;
    float kItemReach = 1200.f;
    int kItemPullCap = 4000;
    int kItemId = 0;
    int kRingPull = 0;
    float kRingReach = 400.f;
    int kRingPullCap = 4000;
    int kRingLate = 0;
    double kLookAheadSeconds = -1.0;
    int kRefuseFatal = -1;
    int kPlanWeight = 0;
    int kPlanPull = 0;
    int kPlanPullCap = 4000;
    bool s_p2Plan = true;
    float kPlanInner = 0.3f;
    double kPlanVTop = 12.0;
    double kPlanVMax = 0.0;
    int kPlanSteer = 0;
    float kSkyMargin = 400.f;
    float kVoidMargin = 300.f;
    int kHeapNodes = 0;
    double kVerifyCap = 240.0;
    double kSpeedBonus = 1200.0;
    int kBallLook = 1;
    double kBallAccel = 0.029115;
    double kBallVy = 0.1294;
    double kBallVyToY = 0.225;
    double kBallGroundRatio = 0.251559;
    double kBallBlueRatio = 0.235530;
    double kBallYStart = 1.3333;
    double kBallGravRef = 1.0;
    float kBallProbeLo = 0.f;
    float kBallProbeHi = 0.f;
    double kBallDyMax = 3.375;
    double kBallInner = 1.0;
    int kBallPlan = 1;
    int kBallSurvive = 400;
    int kBallPull = 30;
    int kBallPullCap = 3000;
    int kBallAhead = 40;
    int kBallSteer = 6;
    int kBallRoomEdge = 1;
    int kBallForce = 0;
    int kBallCalm = 1;
    int kBallClear = 400;
    int kBallOrbEager = 1;
    int kBallOrbSkip = 300;
    int kBallArm = 2;
    int kBallOrbForce = 0;
    int kBallGroundEager = 1;
    int kBallGroundForce = 0;
    int kBallPhaseReach = 260;
    int kBallCrossCap = 48;
    int kBallExact = 1;
    int kBallExactForce = 1;
    int kBallExactHorizon = 1600;
    int kBallExactBeam = 96;
    int kBallExactCap = 400;
    int kBallBuffer = 1;
    int kBallCache = 16;
    int s_ballTrace = 0;
    int kPortalPull = 40;
    float kPortalReach = 900.f;
    int kPortalPullCap = 6000;
    int kSkyDynamic = 0;
    int kDrivenStep = 24;
    float kMovedKey = 320.f;
    float kMovedQuant = 8.f;
    uint64_t g_steerUp = 0, g_steerDown = 0;
    constexpr double kMemoryFloorMB = 512.0;
    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    double processMemoryMB() {
#ifdef GEODE_IS_WINDOWS
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            return (double) pmc.PrivateUsage / (1024.0 * 1024.0);
        }
#endif
        return 0.0;
    }

    double availableMemoryMB() {
#ifdef GEODE_IS_WINDOWS
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) return (double) ms.ullAvailPhys / (1024.0 * 1024.0);
#endif
        return 0.0;
    }

    constexpr uint8_t kActP1 = 1;
    constexpr uint8_t kActP2 = 2;

    enum class Gamemode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

    Gamemode gamemodeOf(PlayerObject* p) {
        if (!p) return Gamemode::Cube;
        if (p->m_isShip) return Gamemode::Ship;
        if (p->m_isBall) return Gamemode::Ball;
        if (p->m_isBird) return Gamemode::Ufo;
        if (p->m_isDart) return Gamemode::Wave;
        if (p->m_isRobot) return Gamemode::Robot;
        if (p->m_isSpider) return Gamemode::Spider;
        if (p->m_isSwing) return Gamemode::Swing;
        return Gamemode::Cube;
    }

    bool isFlyMode(Gamemode m) {
        return m == Gamemode::Ship || m == Gamemode::Ufo || m == Gamemode::Wave || m == Gamemode::Swing;
    }

    // m_playerSpeed -> the 0..4 rank the corridor stores for speed portals.
    inline int speedRank(float sp) {
        if (sp < 0.8f) return 0;
        if (sp < 1.0f) return 1;
        if (sp < 1.2f) return 2;
        if (sp < 1.45f) return 3;
        return 4;
    }
    inline float perTickForSpeed(float sp) {
        static constexpr float kPer[5] = {1.0465f, 1.29825f, 1.614250f, 1.950f, 2.400f};
        return kPer[speedRank(sp)];
    }

    struct FlyModel {
        static constexpr int kModes = 8;
        double up[kModes];
        double down[kModes];
        double scale = 0.2165;

        double flap[kModes];
        double vTop[kModes];

        FlyModel() {
            for (int i = 0; i < kModes; i++) { up[i] = 0.12; down[i] = 0.12; flap[i] = 8.0; vTop[i] = kPlanVTop; }
        }

        void sample(int mode, bool held, double vyBefore, double dvy, double dyWorld, bool flipped) {
            if (mode < 0 || mode >= kModes) return;
            if (mode == (int) Gamemode::Ufo && held) {
                double beforeLocal = flipped ? -vyBefore : vyBefore;
                double afterLocal = flipped ? -(vyBefore + dvy) : (vyBefore + dvy);
                if (afterLocal > 1.0 && afterLocal < 40.0 && afterLocal > beforeLocal + 0.5)
                    flap[mode] += 0.25 * (afterLocal - flap[mode]);
            }
            {
                double after = std::fabs(vyBefore + dvy);
                if (after > vTop[mode] && after < 60.0) vTop[mode] = after;
            }
            double dyLocal = flipped ? -dyWorld : dyWorld;
            if (std::fabs(vyBefore) > 2.0) {
                double s = dyLocal / vyBefore;
                if (s > 0.05 && s < 1.0) scale += 0.05 * (s - scale);
            }
            double mag = held ? dvy : -dvy;
            if (!(mag > 0.0) || mag > 4.0) return;
            double& slot = held ? up[mode] : down[mode];
            slot += 0.15 * (mag - slot);
        }

        double stoppingDistance(int mode, double vy) const {
            if (mode < 0 || mode >= kModes) return 0.0;
            double a = std::max(0.02, vy >= 0 ? down[mode] : up[mode]);
            double d = scale * vy * vy / (2.0 * a);
            return vy >= 0 ? d : -d;
        }
    };

    inline void mix(uint64_t& h, uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    }

    struct ArcModel {
        struct Arc { double vy0 = 0.0, s = 0.0, g = 0.0; int samples = 0; };
        std::map<uint64_t, Arc> arcs;

        static uint64_t keyOf(PlayerObject* p) {
            uint64_t k = 0x9e37ull;
            mix(k, p->m_vehicleSize < 0.9f ? 1 : 0);
            mix(k, (uint64_t) std::llround(p->m_playerSpeed * 100.f));
            mix(k, p->m_isUpsideDown ? 1 : 0);
            mix(k, (uint64_t) std::llround(p->m_gravityMod * 1000.f));
            mix(k, (uint64_t) std::llround(p->m_gravity * 1000.0));
            return k;
        }
        void sampleImpulse(PlayerObject* p, double vy0) {
            if (std::fabs(vy0) < 2.0) return;
            auto& a = arcs[keyOf(p)];
            if (a.samples > 0 && std::fabs(a.vy0 - vy0) > 0.02 * std::fabs(vy0)) { a = Arc{}; }
            a.vy0 = vy0;
            if (a.samples < 1000) a.samples++;
        }
        void sampleFlight(PlayerObject* p, double vyBefore, double vyAfter, double dyWorld) {
            if (std::fabs(vyBefore) < 1.0 || std::fabs(vyAfter) > 14.9) return;
            auto it = arcs.find(keyOf(p));
            if (it == arcs.end()) return;
            auto& a = it->second;
            double s = dyWorld / vyBefore;
            double g = vyBefore - vyAfter;
            if (!(s > 0.05 && s < 1.0)) return;
            if (a.s == 0.0) { a.s = s; a.g = g; }
            else { a.s += 0.2 * (s - a.s); a.g += 0.2 * (g - a.g); }
        }
        Arc const* arcFor(PlayerObject* p) const {
            auto it = arcs.find(keyOf(p));
            if (it == arcs.end() || it->second.samples < 2 || it->second.s == 0.0) return nullptr;
            return &it->second;
        }
    };

    struct PlayerKeyInfo {
        uint8_t heldRun = 0;
        Gamemode mode;
        bool upsideDown;
        bool onGround;
        bool mini;
        bool dashing;
        bool jumpBuffered;
        bool reversed;
        float y;
        double vy;
        float x;
        uint64_t rings;
    };

    PlayerKeyInfo keyInfo(PlayerObject* p) {
        PlayerKeyInfo k{};
        if (!p) return k;
        k.mode = gamemodeOf(p);
        k.upsideDown = p->m_isUpsideDown;
        k.onGround = p->m_isOnGround;
        k.mini = p->m_vehicleSize < 0.9f;
        k.dashing = p->m_isDashing;
        k.jumpBuffered = p->m_jumpBuffered;
        k.reversed = engine::playerReversed(p);
        k.y = p->getPositionY();
        k.vy = p->m_yVelocity;
        k.x = p->getPositionX();
        k.rings = 0;
        return k;
    }

    void mixPlayer(uint64_t& h, PlayerKeyInfo const& k, int K, float scale) {
        float yBucket = 0.5f * K;
        double vyBucket = 0.1 * K;
        float xBucket = 2.f * K;
        if (k.mode == Gamemode::Ship || k.mode == Gamemode::Swing || k.mode == Gamemode::Ufo) {
            yBucket = 1.f * K;
            vyBucket = 0.25 * K;
        }
        if (isFlyMode(k.mode)) {
            yBucket *= scale;
            vyBucket *= scale;
        }
        mix(h, (uint64_t) (int64_t) std::llround(k.y / yBucket));
        mix(h, (uint64_t) (int64_t) std::llround(k.vy / vyBucket));
        mix(h, (uint64_t) (int64_t) std::llround(k.x / xBucket));
        if (k.mode == Gamemode::Robot) mix(h, (uint64_t) (k.heldRun / 4));
        uint64_t flags = (uint64_t) k.mode;
        flags |= k.upsideDown ? 0x10 : 0;
        flags |= k.onGround ? 0x20 : 0;
        flags |= k.mini ? 0x40 : 0;
        flags |= k.dashing ? 0x80 : 0;
        flags |= k.jumpBuffered ? 0x100 : 0;
        flags |= k.reversed ? 0x200 : 0;
        mix(h, flags);
        mix(h, k.rings);
    }
}

struct Solver::Impl {
    struct Node {
        int parent = -1;
        int tick = 0;
        uint8_t action = 0;
        uint8_t nextAction = 0;
        uint8_t actionCount = 0;
        uint8_t order[4] = {0, 0, 0, 0};
        bool closed = false;
        bool inOpen = false;
        uint32_t seq = 0;
        float x = 0.f;
        float y = 0.f;
        float speed = 0.f;
        float x2 = 0.f;
        float y2 = 0.f;
        int guideIdx = 0;
        int progress = 0;
        float room = 0.f;
        uint16_t funnel = 0;
        bool doomed = false;
        uint8_t mode = 0;
        uint8_t heldRun = 0;
        uint16_t items = 0;
        uint8_t planDepth = 0;
        bool orbSkipped = false;
        uint16_t planPull = 0;
        uint8_t boostStage = 0;
        engine::SnapshotPtr snap;
    };

    struct OpenEntry {
        int prio;
        uint32_t seq;
        int node;
        bool operator<(OpenEntry const& o) const {
            if (prio != o.prio) return prio < o.prio;
            return seq < o.seq;
        }
    };

    PlayLayer* pl = nullptr;
    Corridor corridor;
    FlyModel fly;
    ShipPlan shipPlan;
    ShipPlan::GapQuery gapQuery;
    std::vector<Corridor::Item> itemsAhead;
    std::vector<Corridor::Item> ringsAhead;
    std::vector<Corridor::Item> speedPortalsAhead;
    uint64_t planPrefs = 0, planDoomed = 0, planSilent = 0;
    int maxPlanDepth = 0;
    uint64_t adviseCalls = 0, advisePulls = 0;
    int maxBestDepth = 0, maxPull = 0, maxPull2 = 0;
    uint64_t p2Doomed = 0;
    mutable uint64_t ringSeen = 0, ringAir = 0, ringNull = 0, ringEmpty = 0;
    float maxPullY = 0.f, maxPullBestY = 0.f;
    int pullUp = 0, pullDown = 0;
    float maxPlanDepthX = 0.f;
    int shipPlanPrefer(PlayerObject* p, int mode, int ticks, bool& doomed, int* depthOut = nullptr, int slot = 0,
                       int* holdOut = nullptr, int* releaseOut = nullptr);
    ShipPlan::Advice shipPlanAdvise(PlayerObject* p, int mode, int slot = 0);
    void ensureGapQuery();
    int ballOrbSoon(PlayerObject* p, int ticks) const;
    int ballPressNow(PlayerObject* p, BallPlan::Params const& prm) const;
    bool ballWaitSurvives(PlayerObject* p, BallPlan::Params const& prm,
                          double floorY, double ceilY, int wait, int cap) const;
    bool ballLaneAt(PlayerObject* p, double* floorY, double* ceilY) const;
    int ballExactAdvise(PlayerObject* p, unsigned action);
    bool ballExactLive() const;
    ballexact::Params ballExactParams(PlayerObject* p) const;
    ballexact::Plan ballEx;
    int ballExBuilds = 0;
    int ballExHit = 0;
    int ballExMiss = 0;
    int ballExUsed = 0;
    int ballExDepth = 0;
    ShipPlan::Params planParamsFor(PlayerObject* p, int mode, int slot = 0);
    BallPlan::Params ballParamsFor(PlayerObject* p);
    int ballPlanDepth(PlayerObject* p, bool& doomed, int* pressOut = nullptr,
                      int* idleOut = nullptr, int* pressRoomOut = nullptr,
                      int* idleRoomOut = nullptr);
    PlayerObject* secondPlayer() const {
        if (!twoPlayer || !pl->m_gameState.m_isDualMode) return nullptr;
        return pl->m_player2;
    }
    double replaySecondsPerTick = -1.0;
    Solver::Segment segment;
    double firstCandidateTime = 0.0;
    std::unordered_set<uint64_t> candidateKeys;
    int K = 4;
    int pass = 0;
    bool twoPlayer = false;
    bool platformer = false;
    uint64_t maxNodes = 400000;
    size_t maxOpen = 60000;
    double memoryLimitMB = 2500.0;
    double lastMemoryTrim = 0.0;
    static constexpr double kMemoryTrimGap = 5.0;
    double baseMemoryMB = 0.0;
    uint64_t memoryChecks = 0;

    std::vector<Node> nodes;
    std::priority_queue<OpenEntry> open;
    std::unordered_set<uint64_t> visited;
    int current = -1;
    int liveNode = -1;
    uint32_t seqCounter = 0;
    double startTime = 0.0;
    double sliceTime = 0.0;
    uint64_t deaths = 0;
    int deepestTick = 0;
    float furthestX = 0.f;
    float furthestPercent = 0.f;
    float levelLength = 1.f;
    bool logDetails = true;
    double lastProgressTime = 0.0;
    int stallRefinements = 0;
    int deepestNodeId = 0;
    struct Death { float x, y; int obj; uint32_t cause; };
    std::vector<Death> recentDeaths;
    bool dumpedStall = false;
    static constexpr int kBackjumpStages[5] = {90, 180, 360, 720, 1440};
    int backjumpStage = 0;
    int variant = 0;
    int boostBelow = -1;
    double lastBackjumpTime = 0.0;
    bool progressSinceBackjump = false;
    bool guided = false;
    int deepestProgress = 0;
    uint64_t guidePruned = 0;
    float guideRadius0 = 0.f;
    int guideWidenings = 0;
    double lastGuideWiden = 0.0;
    static constexpr int kGuideWidenSteps = 4;
    static constexpr int kGuideWindow = 48;
    static constexpr float kSkyWindow = 300.f;

    uint64_t skyPruned = 0;
    int progressOf(Node const& n) const { return guided ? n.guideIdx : n.progress; }
    int matchGuide(int from, float x, float y, float& dist) const;
    static constexpr float kRoomCap = 40.f;
    static constexpr int kFunnelUnit = 2;
    static constexpr int kFunnelCap = 40;
    static constexpr float kWallBand = 60.f;

    bool rankActive = false;
    static constexpr double kRankDelay = 3.0;
    static constexpr int kLaneBand = 120;
    static constexpr float kLaneHeight = 30.f;
    static constexpr int kLaneUnit = 1;
    std::unordered_map<uint32_t, uint16_t> laneUse;
    uint32_t laneCellOf(Node const& n) const {
        uint32_t band = (uint32_t) (std::max(0, progressOf(n)) / kLaneBand);
        int lane = (int) std::floor(n.y / kLaneHeight);
        return (band << 12) ^ (uint32_t) (lane & 0xfff);
    }
    void noteLane(Node const& n) {
        auto& u = laneUse[laneCellOf(n)];
        if (u < 0xffff) u++;
    }
    int lanePenalty(Node const& n) const {
        auto it = laneUse.find(laneCellOf(n));
        if (it == laneUse.end()) return 0;
        return std::min<int>((int) it->second * kLaneUnit, kLaneCap);
    }
    int itemPull(Node const& n) const {
        if (kItemPull <= 0 || itemsAhead.empty()) return 0;
        auto it = std::upper_bound(itemsAhead.begin(), itemsAhead.end(), n.x,
            [](float x, Corridor::Item const& p) { return x < p.x; });
        while (it != itemsAhead.end() && kItemId > 0 && it->id != kItemId) ++it;
        if (it == itemsAhead.end()) return 0;
        if (it->x - n.x > kItemReach) return 0;
        int d = (int) std::lround(std::fabs(n.y - it->y)) * kItemPull;
        return std::min(d, kItemPullCap);
    }
    static int speedRankOf(float sp) { return speedRank(sp); }
    int portalPull(Node const& n) const {
        if (kPortalPull <= 0 || speedPortalsAhead.empty()) return 0;
        int cur = speedRankOf(n.speed);
        auto it = std::upper_bound(speedPortalsAhead.begin(), speedPortalsAhead.end(), n.x,
            [](float x, Corridor::Item const& p) { return x < p.x; });
        for (; it != speedPortalsAhead.end(); ++it) {
            if (it->x - n.x > kPortalReach) return 0;
            if (it->id <= cur) continue;
            int d = (int) std::lround(std::fabs(n.y - it->y)) * kPortalPull;
            return std::min(d, kPortalPullCap);
        }
        return 0;
    }
    int ringPull(Node const& n) const {
        if (kRingPull <= 0 || ringsAhead.empty()) return 0;
        auto it = std::upper_bound(ringsAhead.begin(), ringsAhead.end(), n.x,
            [](float x, Corridor::Item const& p) { return x < p.x; });
        if (it == ringsAhead.end()) return 0;
        if (it->x - n.x > kRingReach) return 0;
        int d = (int) std::lround(std::fabs(n.y - it->y)) * kRingPull;
        return std::min(d, kRingPullCap);
    }
    int rankOf(Node const& n) const {
        int r = progressOf(n);
        r += (int) n.items * kKeyBonusArg;
        r += (int) n.planDepth * kPlanWeight;
        if (kBallSurvive > 0 && n.mode == (uint8_t) Gamemode::Ball) {
            int d = std::min<int>(n.planDepth, BallPlan::kHorizonTicks);
            r -= kBallSurvive * (BallPlan::kHorizonTicks - d) / BallPlan::kHorizonTicks;
        }
        if (kBallOrbSkip > 0 && n.orbSkipped) r -= kBallOrbSkip;
        if (kBallClear > 0 && n.mode == (uint8_t) Gamemode::Ball) {
            int c = std::clamp((int) n.room, 0, BallPlan::kRoomCap);
            r -= kBallClear * (BallPlan::kRoomCap - c) / BallPlan::kRoomCap;
        }
        r -= itemPull(n);
        r -= ringPull(n);
        r -= portalPull(n);
        if (kSpeedBonus > 0.0) r += (int) std::lround(kSpeedBonus * (double) n.speed);
        if (n.mode == (uint8_t) Gamemode::Ball) {
            if (kBallPull > 0) r -= std::min((int) n.planPull * kBallPull, kBallPullCap);
        } else if (kPlanPull > 0) r -= std::min((int) n.planPull * kPlanPull, kPlanPullCap);
        if (!rankActive) return r;
        r += (int) std::lround(std::min(n.room, kRoomCap) / kRoomCap * (kRoomWeight * kRoomCap));
        r -= std::min<int>(n.funnel, kFunnelCap) * kFunnelUnit;
        if (n.doomed) r -= kDoomPenalty;
        r -= lanePenalty(n);
        return r;
    }
    bool flyAtFrontier() const {
        if (nodes.empty()) return false;
        int id = std::clamp(deepestNodeId, 0, (int) nodes.size() - 1);
        return isFlyMode(static_cast<Gamemode>(nodes[id].mode));
    }
    int backjumpCycles = 0;
    bool loggedFlyHold = false;
    float roomOf(PlayerObject* p) const;
    float bandOf(PlayerObject* p) const;
    bool shipDoomed(PlayerObject* p, int mode) const;
    static uint16_t itemScoreOf(PlayLayer* pl) {
        auto em = pl ? pl->m_effectManager : nullptr;
        if (!em) return 0;
        unsigned total = 0, distinct = 0;
        for (auto const& [id, count] : em->m_itemCountMap) {
            if (count <= 0) continue;
            total += (unsigned) count;
            distinct++;
        }
        return (uint16_t) std::min<unsigned>(total + distinct, 0xffff);
    }
    static std::string describeItems(PlayLayer* pl) {
        auto em = pl ? pl->m_effectManager : nullptr;
        if (!em || em->m_itemCountMap.empty()) return "no items";
        std::string s;
        int shown = 0;
        for (auto const& [id, count] : em->m_itemCountMap) {
            if (count <= 0) continue;
            if (shown++) s += ", ";
            if (shown > 8) { s += "..."; break; }
            s += fmt::format("item {}={}", id, count);
        }
        return s.empty() ? "no items" : s;
    }
    static bool planned(Gamemode m) {
        return m == Gamemode::Ship || m == Gamemode::Swing || m == Gamemode::Ball || m == Gamemode::Ufo;
    }
    int flyDeathTick(PlayerObject* p, int mode, bool press, bool holding, int ticks, float bodyFrac) const;
    int flyLookVerdict(PlayerObject* p, int mode, bool holding, bool& doomed) const;
    static int combineDual(int a, int b, bool& doomed) {
        if (a == 0) return b;
        if (b == 0) return a;
        if (a == b) return a;
        doomed = true;
        return 0;
    }
    PlayerObject* dualPartner() const {
        if (twoPlayer || !pl->m_gameState.m_isDualMode) return nullptr;
        return pl->m_player2;
    }
    static bool flyLookMode(Gamemode m) {
        return m == Gamemode::Ufo || m == Gamemode::Wave || m == Gamemode::Spider
            || (kBallLook && m == Gamemode::Ball);
    }
    int flyLookTicks() const {
        if (lookAheadTicks > 0) return lookAheadTicks;
        return std::max(4, 2 * K);
    }
    int lookAheadTicks = 0;
    BallPlan ballPlan;
    std::function<float(float, float)> ballSpeedAt;
    BallPlan::OrbQuery ballOrbAt;
    int ballRefRank = 3;
    mutable uint64_t ballPlanCalls = 0;
    mutable uint64_t ballPlanDecided = 0;
    mutable uint64_t ballPlanDoomed = 0;
    int ballTrace = 0;
    mutable uint64_t ballAdvised = 0;
    mutable uint64_t ballPulled = 0;
    mutable uint64_t ballSteered = 0;
    mutable uint64_t ballRoomDecided = 0;
    mutable uint64_t ballForced = 0;
    mutable uint64_t ballCalmed = 0;
    mutable uint64_t ballOrbEager = 0;
    mutable uint64_t ballGroundEager = 0;
    mutable int phaseTrace = 0;
    mutable uint64_t flyLookQueries = 0;
    mutable uint64_t flyLookDecided = 0;
    mutable uint64_t flyLookDoomed = 0;
    mutable uint64_t flyLookDropped = 0;
    mutable uint64_t flyLookBlind = 0;
    void noteWallDeath(int parentId, float deathX);
    uint64_t doomedCount = 0;
    uint64_t funnelMarks = 0;
    uint64_t stuckMarks = 0;
    uint16_t mostItems = 0;
    uint64_t frDied = 0, frMerged = 0, frSky = 0, frGuide = 0, frStuck = 0, frKept = 0;
    void resetFrontierReasons() { frDied = frMerged = frSky = frGuide = frStuck = frKept = 0; }
    bool nearFrontier(Node const& n) const { return progressOf(n) >= deepestProgress - kFunnelDepth; }
    float perTickX = 0.f;
    ArcModel arc;
    enum class Verdict { Safe, Dies, Lands };
    Verdict predictJump(PlayerObject* p, ArcModel::Arc const& a) const;
    bool walkDies(PlayerObject* p, int ticks) const;
    mutable uint64_t arcPruned = 0;
    mutable uint64_t arcRefused = 0;
    mutable uint64_t planRefused = 0;
    mutable uint64_t planRefuseSeen = 0;
    bool refuseFatal = false;
    mutable uint64_t arcBlind = 0;

    std::string startPass(Solver& s);
    void warmStart(Solver& s);
    InputList warmPath;
    bool ensureLive(Node& n);
    void applyAction(uint8_t action);
    uint64_t keyOf(uint8_t action, float bucketScale, uint8_t heldRun = 0) const;
    void orderActions(Node& n) const;
    int createChild(int parentId, uint8_t action, int guideIdx, float room, bool doomed,
                    uint8_t heldRun = 0, int planDepth = 0, int planPull = 0,
                    bool orbSkipped = false);
    bool expand(Solver& s, int nodeId, uint8_t action);
    void closeNode(int id);
    bool boosted(Node const& n) const {
        return backjumpStage > 0 && (progressOf(n) <= boostBelow || n.boostStage == backjumpStage);
    }
    int prioOf(Node const& n) const { return boosted(n) ? rankOf(n) + (1 << 28) : rankOf(n); }
    void clearBackjump() { backjumpStage = 0; boostBelow = -1; progressSinceBackjump = false; }
    void reopenFrontier();
    std::deque<int> keptClosed;
    static constexpr size_t kKeptClosed = 3000;
    void advanceBackjump(double stalled);
    void pushOpen(int id);
    int popOpen();
    void trimOpen();
    void rebuildOpen();
    InputList buildPath(int nodeId, uint8_t lastAction, int endTick) const;
    int replayToBoundary(InputList const& own, int tick, float& x, float& y, engine::SnapshotPtr& out, int laneNode = -1);
    int replayDeathTick = 0;
    int replayDeathObj = 0;
    void noteProgress();
    void dumpStall();
};

std::string Solver::Impl::startPass(Solver& s) {
    {
        double secs = kLookAheadSeconds;
        if (secs < 0.0) secs = Mod::get()->getSettingValue<double>("look-ahead-seconds");
        lookAheadTicks = secs > 0.0 ? std::max(1, (int) std::lround(secs * 240.0)) : 0;
        refuseFatal = kRefuseFatal >= 0 ? kRefuseFatal != 0
                                        : Mod::get()->getSettingValue<bool>("refuse-fatal-clicks");
    }
    nodes.clear();
    keptClosed.clear();
    while (!open.empty()) open.pop();
    visited.clear();
    laneUse.clear();
    backjumpCycles = 0;
    loggedFlyHold = false;
    current = -1;
    liveNode = -1;
    seqCounter = 0;
    deepestNodeId = 0;

    auto& st = engine::state();
    engine::releaseAll(pl);
    if (segment.root) {
        if (!engine::restoreSnapshot(pl, *segment.root)) return "could not restore the segment's start state";
    } else {
        engine::restoreStart(pl);
        engine::setDynamicObjects(engine::collectSnapshotObjects(pl));
    }
    corridor.resetMotion();
    Node root;
    root.tick = st.tick;
    root.action = (st.holding[0] ? kActP1 : 0) | (st.holding[1] ? kActP2 : 0);
    root.seq = seqCounter++;
    root.x = engine::playerX(pl);
    root.y = engine::playerY(pl);
    root.speed = pl->m_player1 ? pl->m_player1->m_playerSpeed : 0.f;
    root.snap = engine::takeSnapshot(pl);
    if (!root.snap) return "could not snapshot the level start";
    s.m_stats.snapshots++;
    guided = segment.guide && !segment.guide->empty();
    guideWidenings = 0;
    lastGuideWiden = now();
    if (guideRadius0 <= 0.f) guideRadius0 = segment.guideRadius;
    else segment.guideRadius = guideRadius0;
    if (guided) {
        float dist = 0.f;
        root.guideIdx = matchGuide(std::max(0, segment.rootGuideIdx - kGuideWindow), root.x, root.y, dist);
        log::info("[solver] guided: root at ({:.1f},{:.1f}) matches guide sample {} of {} ({:.1f} units off, radius {:.0f})",
            root.x, root.y, root.guideIdx, segment.guide->size(), dist, segment.guideRadius);
    }
    deepestProgress = progressOf(root);
    orderActions(root);
    nodes.push_back(std::move(root));
    visited.insert(keyOf(0, 1.f));
    current = 0;
    liveNode = 0;
    lastProgressTime = now();
    lastBackjumpTime = now();
    clearBackjump();
    dumpedStall = false;
    recentDeaths.clear();
    itemsAhead = corridor.items();
    ringsAhead = corridor.rings();
    speedPortalsAhead = corridor.speedPortals();
    log::info("[solver] pass {} K={} twoPlayer={} platformer={} length={:.1f} corridor: {} blocking objects, {} movable",
        pass, K, twoPlayer, platformer, levelLength, corridor.objectCount(), corridor.dynamicCount());
    if (!warmPath.empty()) warmStart(s);
    return "";
}

void Solver::Impl::warmStart(Solver& s) {
    auto& st = engine::state();
    size_t cursor = 0;
    uint8_t state = 0;
    int lastEventTick = warmPath.back().tick;
    int nodeId = 0;
    int created = 0;
    while (!s.m_finished) {
        while (cursor < warmPath.size() && warmPath[cursor].tick <= st.tick) {
            auto const& ev = warmPath[cursor++];
            if (ev.button == 1) {
                uint8_t bit = ev.player2 ? kActP2 : kActP1;
                state = ev.down ? (state | bit) : (state & ~bit);
            }
        }
        if (st.tick > lastEventTick + K) break;
        auto& n = nodes[nodeId];
        int slot = -1;
        for (int i = n.nextAction; i < n.actionCount; i++) if (n.order[i] == state) { slot = i; break; }
        if (slot < 0) break;
        std::swap(n.order[slot], n.order[n.nextAction]);
        uint8_t action = n.order[n.nextAction++];
        if (!expand(s, nodeId, action)) break;
        auto& parent = nodes[nodeId];
        if (parent.nextAction < parent.actionCount) pushOpen(nodeId);
        else closeNode(nodeId);
        nodeId = (int) nodes.size() - 1;
        created++;
    }
    current = s.m_finished ? -1 : nodeId;
    if (current >= 0 && nodes[current].closed) current = -1;
    log::info("[solver] warm start replayed {} events into {} nodes (deepest tick {})", warmPath.size(), created, deepestTick);
    warmPath.clear();
}

bool Solver::Impl::ensureLive(Node& n) {
    int id = (int) (&n - nodes.data());
    if (liveNode == id) return true;
    if (id == 0 && !segment.root) {
        engine::releaseAll(pl);
        engine::restoreStart(pl);
        corridor.resetMotion();
        liveNode = 0;
        return true;
    }
    if (!n.snap) return false;
    bool ok = engine::restoreSnapshot(pl, *n.snap);
    if (ok) engine::syncHolding((n.action & kActP1) != 0, (n.action & kActP2) != 0);
    corridor.resetMotion();
    liveNode = ok ? id : -1;
    return ok;
}

void Solver::Impl::applyAction(uint8_t action) {
    engine::applyButton(pl, (action & kActP1) != 0, 1, false);
    if (twoPlayer) engine::applyButton(pl, (action & kActP2) != 0, 1, true);
}

static uint64_t levelStateSignature(PlayLayer* pl) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    auto& gs = pl->m_gameState;
    mix(h, engine::state().activationHash);
    mix(h, (uint64_t) gs.m_activatedObjectIDs.size());
    mix(h, (uint64_t) (int64_t) std::llround(gs.m_timeWarp * 100.0f));
    mix(h, (uint64_t) gs.m_stateObjects.size());
    if (auto em = pl->m_effectManager) {
        uint64_t items = 0;
        for (auto const& [id, count] : em->m_itemCountMap) items += (uint64_t) id * 2654435761ull + (uint64_t) (int64_t) count * 40503ull;
        mix(h, items);
        if (em->m_unkMap288.size() <= 512) {
            uint64_t g = 0;
            for (auto const& [id, on] : em->m_unkMap288) if (on) g += (uint64_t) id * 2654435761ull;
            mix(h, g);
        } else {
            mix(h, (uint64_t) em->m_unkMap288.size());
        }
    }
    if (pl->m_collectedItems) mix(h, (uint64_t) pl->m_collectedItems->count());
    return h;
}

uint64_t Solver::Impl::keyOf(uint8_t action, float bucketScale, uint8_t heldRun) const {
    auto& st = engine::state();
    uint64_t h = 0x243f6a8885a308d3ull;
    mix(h, (uint64_t) st.tick);
    mix(h, action);
    bool dual = pl->m_gameState.m_isDualMode;
    mix(h, dual ? 1 : 0);
    mix(h, levelStateSignature(pl));
    if (kMovedKey > 0.f && !corridor.empty()) {
        // Snap the window to whole buckets so two nodes the player-x bucket already calls equal
        // cannot pick different windows and hash differently for the same world state.
        float cx = std::floor(engine::playerX(pl) / Corridor::kDynamicBucket) * Corridor::kDynamicBucket;
        mix(h, corridor.movedSignature(cx - kMovedKey, cx + kMovedKey, kMovedQuant));
    }
    auto k1 = keyInfo(pl->m_player1);
    k1.heldRun = heldRun;
    int kk = ballExactLive() ? 1 : K;
    mixPlayer(h, k1, kk, bucketScale);
    if (dual && pl->m_player2) {
        auto k2 = keyInfo(pl->m_player2);
        k2.heldRun = heldRun;
        mixPlayer(h, k2, kk, bucketScale);
    }
    return h;
}

float Solver::Impl::roomOf(PlayerObject* p) const {
    if (!p || corridor.empty()) return 0.f;
    auto mode = gamemodeOf(p);
    if (!isFlyMode(mode)) return 0.f;
    float x = p->getPositionX(), y = p->getPositionY();
    float dir = engine::playerDirection(p);
    float perTick = perTickForSpeed(p->m_playerSpeed);
    float a = x + dir * 6.f, b = x + dir * std::max(30.f, perTick * 24.f);
    float below = 0.f, above = 0.f;
    if (!corridor.roomAt(std::min(a, b), std::max(a, b), y, kRoomCap, below, above)) return kRoomCap;
    float band = below + above;
    if (band <= 0.f) return 0.f;
    return std::clamp(std::min(below, above) / (band * 0.5f), 0.f, 1.f) * kRoomCap;
}

float Solver::Impl::bandOf(PlayerObject* p) const {
    if (!p || corridor.empty()) return 2.f * kRoomCap;
    float x = p->getPositionX(), y = p->getPositionY();
    float below = 0.f, above = 0.f;
    if (!corridor.roomAt(x - 6.f, x + 6.f, y, kRoomCap, below, above)) return 2.f * kRoomCap;
    return below + above;
}

bool Solver::Impl::shipDoomed(PlayerObject* p, int mode) const {
    if (!p || corridor.empty()) return false;
    if (mode != (int) Gamemode::Ship && mode != (int) Gamemode::Swing) return false;
    double vy = p->m_yVelocity;
    double up = std::max(0.02, fly.up[mode]), down = std::max(0.02, fly.down[mode]);
    double s = fly.scale;
    bool flipped = p->m_isUpsideDown;
    double accHold = flipped ? -up : up;
    double accRelease = flipped ? down : -down;
    float perTick = perTickX > 0.f ? perTickX : perTickForSpeed(p->m_playerSpeed);
    float dir = engine::playerDirection(p);
    auto const& r = engine::peekRect(p);
    float hw = std::max(4.f, r.size.width * 0.5f), hh = std::max(4.f, r.size.height * 0.5f);
    float x0 = p->getPositionX(), y0 = p->getPositionY();
    for (int t : {6, 12, 18, 24, 36, 48}) {
        double tt = (double) t;
        double dA = s * (vy * tt + accHold * tt * tt * 0.5);
        double dB = s * (vy * tt + accRelease * tt * tt * 0.5);
        float lo = y0 + (float) std::min(dA, dB) - hh;
        float hi = y0 + (float) std::max(dA, dB) + hh;
        float x = x0 + dir * perTick * (float) t;
        float below = 0.f, above = 0.f;
        float mid = (lo + hi) * 0.5f;
        if (!corridor.roomAt(x - hw, x + hw, mid, 1e9f, below, above)) continue;
        float gapLo = mid - below, gapHi = mid + above;
        if (gapHi - gapLo >= 2.f * hh && std::min(gapHi, hi) - std::max(gapLo, lo) >= 2.f * hh) continue;
        bool any = false;
        for (float y = lo + hh; y <= hi - hh + 0.01f && !any; y += hh) {
            float b2 = 0.f, a2 = 0.f;
            if (!corridor.roomAt(x - hw, x + hw, y, 1e9f, b2, a2)) { any = true; break; }
            if (b2 >= hh && a2 >= hh) any = true;
        }
        if (!any) return true;
    }
    return false;
}

Solver::Impl::Verdict Solver::Impl::predictJump(PlayerObject* p, ArcModel::Arc const& a) const {
    float perTick = perTickX > 0.f ? perTickX : perTickForSpeed(p->m_playerSpeed);
    float dir = engine::playerDirection(p);
    auto const& r = engine::peekRect(p);
    float hw = std::max(3.f, r.size.width * 0.5f), hh = std::max(3.f, r.size.height * 0.5f);
    float x = p->getPositionX(), y = p->getPositionY();
    double vy = a.vy0;
    bool flipped = p->m_isUpsideDown;
    for (int k = 1; k <= 160; k++) {
        x += dir * perTick;
        y += (float) (a.s * vy);
        vy -= a.g;
        if (corridor.boxHitsHazard(x - hw, x + hw, y - hh, y + hh)) return Verdict::Dies;
        float feetLo = flipped ? y + hh - 5.f : y - hh, feetHi = flipped ? y + hh : y - hh + 5.f;
        float bodyLo = flipped ? y - hh : y - hh + 5.f, bodyHi = flipped ? y + hh - 5.f : y + hh;
        if (corridor.boxHitsSolid(x - hw, x + hw, bodyLo, bodyHi)) return Verdict::Dies;
        bool descending = flipped ? vy > 0.0 : vy < 0.0;
        if (descending && corridor.boxHitsSolid(x - hw, x + hw, feetLo, feetHi)) return Verdict::Lands;
        if (std::fabs(y - p->getPositionY()) > 400.f) break;
    }
    return Verdict::Safe;
}

bool Solver::Impl::walkDies(PlayerObject* p, int ticks) const {
    float perTick = perTickX > 0.f ? perTickX : perTickForSpeed(p->m_playerSpeed);
    float dir = engine::playerDirection(p);
    auto const& r = engine::peekRect(p);
    float hw = std::max(3.f, r.size.width * 0.5f), hh = std::max(3.f, r.size.height * 0.5f);
    float x = p->getPositionX(), y = p->getPositionY();
    float x1 = x + dir * perTick * (float) ticks;
    if (corridor.boxHitsHazard(std::min(x, x1) - hw, std::max(x, x1) + hw, y - hh, y + hh)) return true;
    bool flipped = p->m_isUpsideDown;
    float bodyLo = flipped ? y - hh : y - hh + 5.f, bodyHi = flipped ? y + hh - 5.f : y + hh;
    if (corridor.boxHitsSolid(std::min(x, x1) - hw, std::max(x, x1) + hw, bodyLo, bodyHi)) return true;
    return false;
}

int Solver::Impl::flyDeathTick(PlayerObject* p, int mode, bool press, bool holding, int ticks, float bodyFrac) const {
    if (!p || corridor.empty() || ticks <= 0) return -1;
    bool bird = mode == (int) Gamemode::Ufo;
    bool dart = mode == (int) Gamemode::Wave;
    bool spider = mode == (int) Gamemode::Spider;
    bool roll = kBallLook && mode == (int) Gamemode::Ball;
    if (!bird && !dart && !spider && !roll) return -1;
    float perTick = perTickX > 0.f ? perTickX : perTickForSpeed(p->m_playerSpeed);
    float dir = engine::playerDirection(p);
    bool flipped = p->m_isUpsideDown;
    auto const& r = engine::peekRect(p);
    float hw = std::max(3.f, r.size.width * 0.5f), hh = std::max(3.f, r.size.height * 0.5f);
    float hzw = hw * bodyFrac, hzh = hh * bodyFrac;
    float sw = hw * bodyFrac, sh = std::max(2.f, hh * bodyFrac - 5.f);
    float x = p->getPositionX(), y = p->getPositionY();

    float lead = dir >= 0.f ? 1.f : -1.f;
    auto fatalAt = [&](float nx, float ny) {
        if (corridor.boxHitsHazard(nx - hzw, nx + hzw, ny - hzh, ny + hzh)) return true;
        float fx = nx + lead * sw;
        return corridor.boxHitsSolid(std::min(fx, fx - lead * 4.f), std::max(fx, fx - lead * 4.f),
                                     ny - sh, ny + sh);
    };

    if (spider) {
        if (!press) {
            for (int t = 1; t <= ticks; t++) {
                float nx = x + dir * perTick * (float) t;
                if (fatalAt(nx, y)) return t;
            }
            return -1;
        }
        float below = 0.f, above = 0.f;
        if (!corridor.roomAt(x - 6.f, x + 6.f, y, 600.f, below, above)) return -1;
        float dest = (p->m_isUpsideDown ? y - below : y + above);
        if (std::fabs(dest - y) < 1.f) return -1;
        dest += p->m_isUpsideDown ? hh : -hh;
        if (fatalAt(x, dest)) return 1;
        for (int t = 1; t <= ticks; t++) {
            float nx = x + dir * perTick * (float) t;
            if (fatalAt(nx, dest)) return t;
        }
        return -1;
    }

    if (roll) {
        static constexpr int kSecond[] = {9999, 30, 24, 34, 20, 44, 16, 60};
        double accel = std::max(0.004, kBallAccel);
        int best = 0;
        for (int k : kSecond) {
            if (k != 9999 && k > ticks) continue;
            double dy = p->m_yVelocity * kBallVyToY;
            int g = flipped ? 1 : -1;
            if (press) g = -g;
            double ny = y;
            int died = -1;
            for (int t = 1; t <= ticks; t++) {
                if (k != 9999 && t % k == 0) g = -g;
                dy += g * accel;
                ny += dy;
                float nx = x + dir * perTick * (float) t;
                float below = 0.f, above = 0.f;
                if (corridor.roomAt(nx - hw, nx + hw, (float) ny, 600.f, below, above)) {
                    if (g < 0 && below < hh) { ny += (double) (hh - below); dy = 0.0; }
                    else if (g > 0 && above < hh) { ny -= (double) (hh - above); dy = 0.0; }
                }
                if (fatalAt(nx, (float) ny)) { died = t; break; }
            }
            if (died < 0) return -1;       // this schedule clears the horizon
            if (died > best) best = died;
        }
        return best;
    }

    if (dart) {
        float slope = (p->m_vehicleSize < 0.9f ? 2.1f : 1.1f) * (press ? 1.f : -1.f);
        if (flipped) slope = -slope;
        for (int t = 1; t <= ticks; t++) {
            float nx = x + dir * perTick * (float) t;
            float ny = y + slope * perTick * (float) t;
            if (fatalAt(nx, ny)) return t;
        }
        return -1;
    }

    double vy = p->m_yVelocity;
    if (press && !holding) {
        double impulse = std::max(1.0, fly.flap[mode]);
        vy = flipped ? -impulse : impulse;
    }
    double grav = std::max(0.02, fly.down[mode]);
    double accel = flipped ? grav : -grav;
    double s = fly.scale;
    for (int t = 1; t <= ticks; t++) {
        double tt = (double) t;
        float nx = x + dir * perTick * (float) t;
        float ny = y + (float) (s * (vy * tt + accel * tt * tt * 0.5));
        if (fatalAt(nx, ny)) return t;
    }
    return -1;
}

int Solver::Impl::flyLookVerdict(PlayerObject* p, int mode, bool holding, bool& doomed) const {
    doomed = false;
    if (!p) return 0;
    int ticks = mode == (int) Gamemode::Spider ? std::max(1, K) : flyLookTicks();
    if (mode != (int) Gamemode::Ball) {
        float px = p->getPositionX(), dir = engine::playerDirection(p);
        float reach = (float) ticks * (perTickX > 0.f ? perTickX : 1.3f) + 20.f;
        if (corridor.hasPortal(px, px + dir * reach)) { flyLookBlind++; return 0; }
    }
    flyLookQueries++;
    int dPress = flyDeathTick(p, mode, true, holding, ticks, 1.f);
    int dIdle = flyDeathTick(p, mode, false, holding, ticks, 1.f);
    if (dPress < 0 && dIdle < 0) return 0;
    if (dPress < 0) { flyLookDecided++; return +1; }
    if (dIdle < 0) { flyLookDecided++; return -1; }
    doomed = true;
    flyLookDoomed++;
    if (dPress == dIdle) return 0;
    return dPress > dIdle ? +1 : -1;
}

void Solver::Impl::noteWallDeath(int parentId, float deathX) {
    if (deathX < furthestX - kWallBand) return;
    int limit = nodes[parentId].progress - kFunnelDepth;
    for (int id = parentId; id > 0 && nodes[id].progress >= limit; id = nodes[id].parent) {
        if (nodes[id].funnel < 0xFFFF) nodes[id].funnel++;
        funnelMarks++;
    }
}

static bool preferJumpGround(PlayerObject* p, Corridor const& corridor) {
    if (!p) return false;
    if (!p->m_isOnGround && !p->m_isOnSlope) return false;
    float x = p->getPositionX(), y = p->getPositionY();
    float dir = engine::playerDirection(p);
    float lo = 0.f, hi = 0.f;
    float a = x + dir * 25.f, b = x + dir * 85.f;
    if (!corridor.freeBand(std::min(a, b), std::max(a, b), y, lo, hi)) return false;
    if (p->m_isUpsideDown) return hi < y - 10.f;
    return lo > y + 10.f;
}

static bool preferHoldFly(PlayerObject* p, Corridor const& corridor, bool currentlyHolding,
                          Corridor::Steer steer, FlyModel const& fly, int mode, int planVerdict = 0) {
    bool flipped = p->m_isUpsideDown;
    if (planVerdict != 0 && !p->m_isBird && !p->m_isDart) return planVerdict > 0;
    float x = p->getPositionX();
    float y = p->getPositionY();
    float speed = perTickForSpeed(p->m_playerSpeed) * 240.f;
    float slope = p->m_isDart ? (p->m_vehicleSize < 0.9f ? 2.1f : 1.1f) : 1.2f;
    float half = (p->m_isDart ? 5.f : p->m_isBird ? 8.f : 10.f) * (p->m_vehicleSize < 0.9f ? 0.6f : 1.f);
    float target = corridor.targetY(x, y, speed, steer, engine::playerDirection(p), slope, half);
    double vy = p->m_yVelocity;
    double err = flipped ? (double) (y - target) : (double) (target - y);

    if (p->m_isBird) {
        if (planVerdict != 0) return planVerdict > 0 && !currentlyHolding;
        double vmax = 3.0;
        bool wantLocalUp;
        if (std::fabs(err) <= 10.0) wantLocalUp = vy < -vmax;
        else wantLocalUp = err > 0;
        if (wantLocalUp && vy > vmax) wantLocalUp = false;
        return wantLocalUp && !currentlyHolding;
    }

    if (p->m_isDart) {
        if (planVerdict != 0) return planVerdict > 0;
        if (err > 2.0) return true;
        if (err < -2.0) return false;
        return currentlyHolding;
    }

    double stop = fly.stoppingDistance(mode, vy);
    double margin = 2.0;
    if (err > stop + margin) return true;
    if (err < stop - margin) return false;
    return currentlyHolding;
}

static int ballReachTicks(double y, int g, double jumpDy, double accel, double dyLim,
                          double bandLo, double bandHi, double floorY, double ceilY, int cap) {
    int ng = g ^ 1;
    double dy = ng ? jumpDy : -jumpDy;
    for (int t = 1; t <= cap; t++) {
        dy += ng ? accel : -accel;
        dy = std::clamp(dy, -dyLim, dyLim);
        y += dy;
        if (y <= floorY) { y = floorY; dy = 0.0; }
        if (y >= ceilY) { y = ceilY; dy = 0.0; }
        if (y >= bandLo && y <= bandHi) return t;
    }
    return -1;
}

// 1 = touching an orb right now, 2 = airborne and reaching one within `ticks`, 0 = neither.
// The caller still gates on the plan saying a press is no worse than idling.
int Solver::Impl::ballOrbSoon(PlayerObject* p, int ticks) const {
    if (!p) return 0;
    if (p->m_touchingRings && p->m_touchingRings->count() > 0) return 1;
    if (ticks <= 0 || corridor.orbs().empty()) return 0;   // ticks == 0: touching only
    if (p->m_isOnGround) return 0;                  // a press here would flip off the surface
    float perTick = perTickX > 0.f ? perTickX : perTickForSpeed(p->m_playerSpeed);
    float dir = engine::playerDirection(p);
    float x = p->getPositionX();
    float y = p->getPositionY();
    auto const& r = engine::peekRect(p);
    float hw = std::max(4.f, r.size.width * 0.5f);
    float hh = std::max(4.f, r.size.height * 0.5f);
    float span = perTick * (float) ticks;
    float a = x - dir * hw, b = x + dir * (span + hw);
    double dy = (double) p->m_yVelocity * kBallVyToY;
    double acc = (p->m_isUpsideDown ? 1.0 : -1.0) * std::max(0.004, kBallVy * kBallVyToY);
    double tt = (double) ticks;
    float yEnd = (float) ((double) y + dy * tt + acc * tt * tt * 0.5);
    float lo = 0.f, hi = 0.f;
    if (!corridor.orbSpans(std::min(a, b), std::max(a, b), &lo, &hi)) return 0;
    float ylo = std::min(y, yEnd) - hh, yhi = std::max(y, yEnd) + hh;
    return (yhi >= lo && ylo <= hi) ? 2 : 0;
}

bool Solver::Impl::ballWaitSurvives(PlayerObject* p, BallPlan::Params const& prm,
                                    double floorY, double ceilY, int wait, int cap) const {
    if (!p) return true;
    float x0 = p->getPositionX();
    float ys = p->getPositionY();
    auto const& r = engine::peekRect(p);
    float hw = std::max(4.f, r.size.width * 0.5f);
    float hh = std::max(4.f, r.size.height * 0.5f);
    float dir = prm.dir != 0.f ? prm.dir : 1.f;
    float perTick = prm.perTick > 0.f ? prm.perTick : 1.9f;
    float cx = x0;
    if (wait > 0) {
        float xe = x0 + dir * (perTick * (float) wait);
        float a = std::min(x0, xe) - hw, b = std::max(x0, xe) + hw;
        if (corridor.boxHitsHazard(a, b, ys - hh, ys + hh)) return false;
        cx = xe;
    }
    int g = (p->m_isUpsideDown ? 1 : 0) ^ 1;
    double dy = g ? prm.jumpDy : -prm.jumpDy;
    double y = (double) ys;
    for (int t = 1; t <= cap; t++) {
        dy += g ? prm.accel : -prm.accel;
        dy = std::clamp(dy, -prm.dyMax, prm.dyMax);
        double ny = y + dy;
        bool landed = false;
        if (ny <= floorY) { ny = floorY; landed = true; }
        if (ny >= ceilY) { ny = ceilY; landed = true; }
        float nx = cx + dir * perTick;
        float a = std::min(cx, nx) - hw, b = std::max(cx, nx) + hw;
        float ylo = (float) std::min(y, ny) - hh, yhi = (float) std::max(y, ny) + hh;
        if (corridor.boxHitsHazard(a, b, ylo, yhi)) return false;
        if (landed) return true;
        y = ny;
        cx = nx;
    }
    return true;
}

bool Solver::Impl::ballLaneAt(PlayerObject* p, double* floorY, double* ceilY) const {
    if (!p) return false;
    float x = p->getPositionX();
    float y = p->getPositionY();
    auto const& r = engine::peekRect(p);
    float hh = std::max(4.f, r.size.height * 0.5f);
    std::vector<std::pair<float, float>> gaps;
    bool content = false; float top = 0.f;
    const_cast<Impl*>(this)->ensureGapQuery();
    gapQuery(x - 1.f, x + 1.f, 0.f, false, gaps, content, top);
    double lo = (double) y - 60.0, hi = (double) y + 60.0;
    for (auto const& gp : gaps) {
        if ((double) gp.second <= (double) y && (double) gp.second > lo) lo = gp.second;
        if ((double) gp.first >= (double) y && (double) gp.first < hi) hi = gp.first;
    }
    *floorY = lo + (double) hh;
    *ceilY = hi - (double) hh;
    return *ceilY > *floorY;
}

bool Solver::Impl::ballExactLive() const {
    if (kBallExact <= 0 || !pl) return false;
    auto p1 = pl->m_player1;
    if (!p1 || p1->m_controlsDisabled) return false;
    if (pl->m_gameState.m_isDualMode) return false;
    return gamemodeOf(p1) == Gamemode::Ball;
}

ballexact::Params Solver::Impl::ballExactParams(PlayerObject* p) const {
    ballexact::Params prm;
    if (!p) return prm;
    auto const& r = engine::peekRect(p);
    prm.hw = std::max(4.f, r.size.width * 0.5f);
    prm.hh = std::max(4.f, r.size.height * 0.5f);
    prm.yStart = p->m_yStart > 0.1f ? (double) p->m_yStart : 11.23;
    prm.perTick = (double) perTickForSpeed(p->m_playerSpeed);
    prm.dir = (double) engine::playerDirection(p);
    prm.grav = kBallVy;
    prm.vyToY = kBallVyToY;
    prm.vyMax = 15.0;
    prm.surfBase = 0.240;
    for (int i = 0; i < 8; i++) prm.orbBase[i] = 0.0;
    prm.orbBase[BallPlan::OrbBlue] = 0.224;
    return prm;
}

int Solver::Impl::ballExactAdvise(PlayerObject* p, unsigned action) {
    if (!p) return -1;
    int tick = engine::state().tick;
    double y = (double) p->getPositionY();
    double vy = (double) p->m_yVelocity;
    int g = p->m_isUpsideDown ? 1 : 0;
    int held = (action & kActP1) != 0 ? 1 : 0;
    int buf = p->m_jumpBuffered ? 1 : 0;
    if (ballEx.matches(tick, y, vy, g, held, buf, 0.02)) {
        ballExHit++;
        ballExUsed++;
        return ballEx.at(tick) ? 1 : 0;
    }
    if (ballEx.covers(tick)) {
        ballExMiss++;
        size_t i = (size_t) (tick - ballEx.tick0);
        if (ballExMiss <= 8)
            geode::log::info("[bexact] off plan t={} want y={:.3f} vy={:.4f} g={} held={} buf={}"
                             " | got y={:.3f} vy={:.4f} g={} held={} buf={}",
                tick, ballEx.y[i], ballEx.vy[i], ballEx.g[i], ballEx.held[i], ballEx.buf[i],
                y, vy, g, held, buf);
    }
    if (ballExBuilds >= kBallExactCap) return -1;
    ballExBuilds++;
    ballexact::Start st;
    st.x = (double) p->getPositionX();
    st.y = y;
    st.vy = vy;
    st.g = g;
    st.held = held;
    st.buf = buf;
    st.tick = tick;
    auto plan = ballexact::build(corridor, st, ballExactParams(p),
                                 kBallExactHorizon, kBallExactBeam, 0.0);
    if (plan.act.empty()) return -1;
    ballEx = std::move(plan);
    ballExDepth = ballEx.depth;
    if (ballExBuilds <= 6 || (ballExBuilds % 50) == 0)
        geode::log::info("[bexact] build {} at t={} x={:.1f} y={:.2f} vy={:.3f} g={} -> depth {} ({}) endX {:.1f}",
            ballExBuilds, tick, st.x, st.y, st.vy, st.g, ballEx.depth,
            ballEx.ok ? "full" : "partial", ballEx.endX);
    ballExUsed++;
    return ballEx.act[0] ? 1 : 0;
}

int Solver::Impl::ballPressNow(PlayerObject* p, BallPlan::Params const& prm) const {
    if (!p) return -1;
    double floorY = 0.0, ceilY = 0.0;
    if (!ballLaneAt(p, &floorY, &ceilY)) return -1;
    int cap = kBallCrossCap > 0 ? kBallCrossCap : 48;
    bool now = ballWaitSurvives(p, prm, floorY, ceilY, 0, cap);
    if (!now) return -1;                        // leaving now dies too: no opinion
    bool later = ballWaitSurvives(p, prm, floorY, ceilY, 1, cap);
    float x = p->getPositionX();
    float y = p->getPositionY();
    auto const& r = engine::peekRect(p);
    float hw = std::max(4.f, r.size.width * 0.5f);
    float hh = std::max(4.f, r.size.height * 0.5f);
    float dir = prm.dir;
    float perTick = prm.perTick > 0.f ? prm.perTick : 1.9f;
    if (!later) {
        if (kBallProbeHi > kBallProbeLo && x >= kBallProbeLo && x <= kBallProbeHi
            && phaseTrace < 200) {
            phaseTrace++;
            geode::log::debug("[phase] x={:.2f} y={:.2f} up={} lane={:.1f}..{:.1f}"
                              " LAST-SAFE-TICK -> PRESS", x, y,
                              p->m_isUpsideDown ? 1 : 0, floorY, ceilY);
        }
        return 1;
    }
    auto const& orbs = corridor.orbs();
    bool onCeiling = p->m_isUpsideDown;
    Corridor::Orb const* best = nullptr;
    float bestReach = 1e30f;
    bool sameSide = false;
    for (auto const& o : orbs) {
        float enter = dir >= 0.f ? o.x0 - hw : -(o.x1 + hw);
        float u = dir >= 0.f ? x : -x;
        if (enter <= u) continue;
        if (enter - u > (float) kBallPhaseReach) continue;
        if (enter - u >= bestReach) continue;
        float bandLo = o.y0 - hh, bandHi = o.y1 + hh;
        bool opposite = onCeiling ? bandHi < y - 4.f : bandLo > y + 4.f;
        bool same = onCeiling ? bandLo > y - 4.f : bandHi < y + 4.f;
        if (!opposite && !same) continue;
        bestReach = enter - u;
        best = &o;
        sameSide = !opposite;
    }
    if (!best) return -1;
    int g0 = p->m_isUpsideDown ? 1 : 0;
    double bandLo = (double) (best->y0 - hh), bandHi = (double) (best->y1 + hh);
    int t;
    if (!sameSide) {
        t = ballReachTicks((double) y, g0, prm.jumpDy, prm.accel, prm.dyMax,
                           bandLo, bandHi, floorY, ceilY, 240);
    } else {
        double farLo = g0 ? floorY : ceilY - 0.01;
        double farHi = g0 ? floorY + 0.01 : ceilY;
        int tCross = ballReachTicks((double) y, g0, prm.jumpDy, prm.accel, prm.dyMax,
                                    farLo, farHi, floorY, ceilY, 240);
        int tBack = tCross > 0
            ? ballReachTicks(g0 ? floorY : ceilY, g0 ^ 1, prm.jumpDy, prm.accel, prm.dyMax,
                             bandLo, bandHi, floorY, ceilY, 240)
            : -1;
        t = (tCross > 0 && tBack > 0) ? tCross + 1 + tBack : -1;
    }
    if (t <= 0) return -1;
    float need = bestReach - (float) t * perTick;
    bool go = need <= 0.5f * perTick;
    if (!go) {
        int d = (int) std::floor(need / perTick);
        if (d < 1) d = 1;
        if (d > 90) d = 90;
        if (!ballWaitSurvives(p, prm, floorY, ceilY, d, cap)) go = true;
    }
    if (kBallProbeHi > kBallProbeLo && x >= kBallProbeLo && x <= kBallProbeHi
        && phaseTrace < 200) {
        phaseTrace++;
        geode::log::debug("[phase] x={:.2f} y={:.2f} up={} orbx={:.1f}..{:.1f} band={:.1f}..{:.1f}"
                          " lane={:.1f}..{:.1f} reach={:.1f} t={} per={:.3f} need={:.2f} -> {}",
            x, y, p->m_isUpsideDown ? 1 : 0, best->x0, best->x1, best->y0 - hh, best->y1 + hh,
            floorY, ceilY, bestReach, t, perTick, need,
            go ? (sameSide ? "PRESS-rt" : "PRESS") : (sameSide ? "wait-rt" : "wait"));
        (void) 0;
    }
    return go ? 1 : 0;
}

void Solver::Impl::ensureGapQuery() {
    if (gapQuery) return;
    gapQuery = [this](float x0, float x1, float predictTicks, bool hazard, std::vector<std::pair<float, float>>& out, bool& content, float& top) {
        corridor.blockedAt(x0, x1, predictTicks, hazard, out);
        content = corridor.hasContent(x0, x1);
        top = -1e30f;
        if (content) corridor.contentTop(x0, x1, top);
    };
}

BallPlan::Params Solver::Impl::ballParamsFor(PlayerObject* p) {
    BallPlan::Params prm;
    prm.tick = engine::state().tick;
    prm.perTick = perTickX > 0.f ? perTickX : perTickForSpeed(p->m_playerSpeed);
    prm.dir = engine::playerDirection(p);
    prm.flipped = p->m_isUpsideDown;
    prm.accel = kBallVy > 0.0 ? std::max(0.004, kBallVy * kBallVyToY)
                              : std::max(0.004, kBallAccel);
    prm.dyMax = kBallDyMax;
    auto const& r = engine::peekRect(p);
    prm.halfHeight = std::max(4.f, r.size.height * 0.5f);
    prm.innerHalf = std::max(2.f, prm.halfHeight * (float) kBallInner);
    prm.halfWidth = std::max(4.f, r.size.width * 0.5f);
    prm.innerHalfW = std::max(2.f, prm.halfWidth * (float) kBallInner);
    ballRefRank = speedRankOf(p->m_playerSpeed);
    if (!ballSpeedAt && !corridor.speedPortals().empty()) {
        auto* self = this;
        ballSpeedAt = [self](float x, float fallback) -> float {
            static constexpr float kPer[5] = {1.0465f, 1.29825f, 1.614250f, 1.950f, 2.400f};
            auto const& ports = self->corridor.speedPortals();
            auto it = std::upper_bound(ports.begin(), ports.end(), x,
                [](float xv, Corridor::Item const& q) { return xv < q.x; });
            if (it == ports.begin()) return fallback;
            --it;
            int r = std::clamp(it->id, 0, 4);
            int base = std::clamp(self->ballRefRank, 0, 4);
            return fallback * kPer[r] / kPer[base];
        };
    }
    prm.perTickAt = ballSpeedAt;
    if (!ballOrbAt && !corridor.orbs().empty()) {
        auto* self = this;
        ballOrbAt = [self](float x0, float x1, float* lo, float* hi) {
            return self->corridor.orbSpans(x0, x1, lo, hi);
        };
    }
    prm.orbAt = ballOrbAt;
    double ys = (double) p->m_yStart;
    if (!(ys > 0.0)) ys = 11.23;
    double unit = ys * kBallVyToY;
    double anchor = kBallBlueRatio / 0.80;
    prm.jumpDy = kBallGroundRatio * unit;
    prm.orbDy[BallPlan::OrbBlue]   = kBallBlueRatio * unit;
    prm.orbDy[BallPlan::OrbYellow] = 1.00 * anchor * unit;
    prm.orbDy[BallPlan::OrbPink]   = 0.77 * anchor * unit;
    prm.orbDy[BallPlan::OrbRed]    = 1.34 * anchor * unit;
    prm.orbDy[BallPlan::OrbGreen]  = 1.00 * anchor * unit;
    prm.orbDy[BallPlan::OrbDrop]   = 15.0 * kBallVyToY;
    if (kBallGravRef > 0.0) {
        double mod = (double) p->m_gravityMod;
        if (!(mod > 0.0)) mod = 1.0;
        prm.accel *= mod / kBallGravRef;
    }
    return prm;
}

int Solver::Impl::ballPlanDepth(PlayerObject* p, bool& doomed, int* pressOut, int* idleOut,
                                int* pressRoomOut, int* idleRoomOut) {
    doomed = false;
    if (!p || corridor.empty()) return -1;
    ensureGapQuery();
    BallPlan::Params prm = ballParamsFor(p);
    ballPlan.trace = s_ballTrace;
    ballPlan.cacheCap = (size_t) std::max(1, kBallCache);
    auto v = ballPlan.query(gapQuery, prm, p->getPositionX(), p->getPositionY(),
                            (double) p->m_yVelocity * kBallVyToY);
    ballPlanCalls++;
    bool inWindow = kBallProbeHi > kBallProbeLo
                  && p->getPositionX() >= kBallProbeLo && p->getPositionX() <= kBallProbeHi;
    if (inWindow ? ballTrace < 400 : (kBallProbeHi <= kBallProbeLo && ballTrace < 24)) {
        ballTrace++;
        double lo = 0.0, hi = 0.0;
        bool onSurf = false;
        {
            auto adv = ballPlan.advise(gapQuery, prm, p->getPositionX(), p->getPositionY(), 0);
            lo = adv.lo; hi = adv.hi;
        }
        log::info("[ball] x {:.1f} y {:.2f} dy {:+.3f} grav{} orb{} | d {} press {} idle {} "
                  "room {}/{} | band {:.0f}-{:.0f} | jump {:.3f} blue {:.3f} acc {:.4f}",
                  p->getPositionX(), p->getPositionY(),
                  (double) p->m_yVelocity * kBallVyToY, prm.flipped ? "UP" : "DN",
                  (p->m_touchingRings && p->m_touchingRings->count() > 0) ? "Y" : "n",
                  v.depth, v.pressDepth, v.idleDepth, v.pressRoom, v.idleRoom, lo, hi,
                  prm.jumpDy, prm.orbDy[BallPlan::OrbBlue], prm.accel);
        (void) onSurf;
    }
    if (!v.ok) return -1;
    if (pressOut) *pressOut = v.pressDepth;
    if (idleOut) *idleOut = v.idleDepth;
    if (pressRoomOut) *pressRoomOut = v.pressRoom;
    if (idleRoomOut) *idleRoomOut = v.idleRoom;
    if (v.depth <= 0) { doomed = true; ballPlanDoomed++; }
    return v.depth;
}

ShipPlan::Params Solver::Impl::planParamsFor(PlayerObject* p, int mode, int slot) {
    ShipPlan::Params prm;
    prm.slot = slot;
    prm.tick = engine::state().tick;
    bool flipped = p->m_isUpsideDown;
    double up = std::max(0.02, fly.up[mode]), down = std::max(0.02, fly.down[mode]);
    prm.accHold = flipped ? -up : up;
    prm.accRelease = flipped ? down : -down;
    if (mode == (int) Gamemode::Ufo) {
        prm.mode = ShipPlan::Mode::Ufo;
        double g = std::max(0.02, fly.down[mode]);
        prm.accRelease = flipped ? g : -g;
        prm.accHold = prm.accRelease;
        double imp = std::max(1.0, fly.flap[mode]);
        prm.flap = flipped ? -imp : imp;
    }
    if (mode == (int) Gamemode::Ball) {
        prm.mode = ShipPlan::Mode::Ball;
        double g = std::max(0.02, fly.down[mode]);
        prm.accRelease = flipped ? g : -g;
        prm.accHold = flipped ? -g : g;
    }
    prm.scale = fly.scale;
    prm.vMax = kPlanVMax > 0.0 ? kPlanVMax
                               : std::max(4.0, fly.vTop[std::clamp(mode, 0, FlyModel::kModes - 1)] + 0.5);
    prm.perTick = perTickX > 0.f ? perTickX : perTickForSpeed(p->m_playerSpeed);
    prm.dir = engine::playerDirection(p);
    {
        auto const& r = engine::peekRect(p);
        prm.halfHeight = std::max(4.f, r.size.height * 0.5f);
        prm.innerHalf = std::max(2.f, prm.halfHeight * kPlanInner);
        prm.groundY = 90.f;
    }
    {
        float x = p->getPositionX(), reach = (float) ShipPlan::s_windows * std::max(20.f, prm.perTick * 15.f);
        prm.dynamic = corridor.hasDynamic(std::min(x, x + prm.dir * reach), std::max(x, x + prm.dir * reach));
    }
    return prm;
}

int Solver::Impl::shipPlanPrefer(PlayerObject* p, int mode, int ticks, bool& doomed, int* depthOut, int slot,
                                 int* holdOut, int* releaseOut) {
    doomed = false;
    if (!p || corridor.empty()) return 0;
    ensureGapQuery();
    ShipPlan::Params prm = planParamsFor(p, mode, slot);
    int holdDepth = -1, releaseDepth = -1;
    int v = shipPlan.prefer(gapQuery, prm, p->getPositionX(), p->getPositionY(), p->m_yVelocity, std::max(1, ticks), doomed, holdDepth, releaseDepth);
    if (v != 0) planPrefs++; else planSilent++;
    if (doomed) planDoomed++;
    if (depthOut) *depthOut = std::max(0, std::max(holdDepth, releaseDepth));
    if (holdOut) *holdOut = holdDepth;
    if (releaseOut) *releaseOut = releaseDepth;
    return v;
}

ShipPlan::Advice Solver::Impl::shipPlanAdvise(PlayerObject* p, int mode, int slot) {
    ShipPlan::Advice out;
    if (!p || corridor.empty()) return out;
    ensureGapQuery();
    ShipPlan::Params prm = planParamsFor(p, mode, slot);
    return shipPlan.advise(gapQuery, prm, p->getPositionX(), p->getPositionY(), p->m_yVelocity);
}

void Solver::Impl::orderActions(Node& n) const {
    auto p1 = pl->m_player1;
    bool dual = pl->m_gameState.m_isDualMode;
    auto mode = gamemodeOf(p1);
    uint8_t preferP1 = 0;
    int flyVerdict = 0;
    bool flyBothDie = false;
    auto steer = static_cast<Corridor::Steer>((backjumpStage + variant) % 3);
    if (isFlyMode(mode) && p1) {
        int verdict = 0;
        if (planned(mode)) {
            bool doomed = false;
            verdict = const_cast<Impl*>(this)->shipPlanPrefer(p1, (int) mode, K, doomed);
            flyBothDie = doomed;
        } else if (flyLookMode(mode)) {
            verdict = flyLookVerdict(p1, (int) mode, (n.action & kActP1) != 0, flyBothDie);
        }
        if (auto p2 = dualPartner()) {
            auto m2 = gamemodeOf(p2);
            int v2 = 0; bool d2 = false;
            if (planned(m2)) v2 = const_cast<Impl*>(this)->shipPlanPrefer(p2, (int) m2, K, d2);
            else if (flyLookMode(m2)) v2 = flyLookVerdict(p2, (int) m2, (n.action & kActP1) != 0, d2);
            if (d2) flyBothDie = true;
            verdict = combineDual(verdict, v2, flyBothDie);
        }
        flyVerdict = verdict;
        preferP1 = preferHoldFly(p1, corridor, (n.action & kActP1) != 0, steer, fly, (int) mode, verdict) ? kActP1 : 0;
    } else if (n.action & kActP1) {
        preferP1 = kActP1;
    } else if (preferJumpGround(p1, corridor)) {
        preferP1 = kActP1;
    }
    if (p1 && mode == Gamemode::Spider && !p1->m_controlsDisabled && (!dual || dualPartner())) {
        flyVerdict = flyLookVerdict(p1, (int) mode, (n.action & kActP1) != 0, flyBothDie);
        if (auto p2 = dualPartner()) {
            auto m2 = gamemodeOf(p2);
            if (flyLookMode(m2)) {
                bool d2 = false;
                int v2 = flyLookVerdict(p2, (int) m2, (n.action & kActP1) != 0, d2);
                if (d2) flyBothDie = true;
                flyVerdict = combineDual(flyVerdict, v2, flyBothDie);
            }
        }
        if (flyVerdict != 0) preferP1 = flyVerdict > 0 ? kActP1 : 0;
    }
    bool ballFirm = false;
    bool ballExactSaid = false;
    if (p1 && mode == Gamemode::Ball && !p1->m_controlsDisabled && !dual) {
        int pressD = -1, idleD = -1, pressR = 0, idleR = 0;
        bool got = false, doom = false;
        int exact = kBallExact > 0
            ? const_cast<Impl*>(this)->ballExactAdvise(p1, n.action) : -1;
        if (kBallPlan && exact < 0) {
            auto* self = const_cast<Impl*>(this);
            got = self->ballPlanDepth(p1, doom, &pressD, &idleD, &pressR, &idleR) >= 0;
        }
        if (exact >= 0) {
            preferP1 = exact > 0 ? kActP1 : 0;
            ballExactSaid = true;
            ballPlanDecided++;
            flyVerdict = exact > 0 ? +1 : -1;
            ballFirm = kBallExactForce > 0;
        } else if (got) {
            if (pressD != idleD) {
                preferP1 = pressD > idleD ? kActP1 : 0;
                ballPlanDecided++;
                flyVerdict = pressD > idleD ? +1 : -1;
                ballFirm = kBallForce >= 1;
            } else if (kBallOrbEager > 0 && ballOrbSoon(p1, kBallArm) > 0) {
                bool holding = (n.action & kActP1) != 0;
                bool wantPress = !holding;
                preferP1 = wantPress ? kActP1 : 0;
                ballPlanDecided++;
                ballOrbEager++;
                flyVerdict = wantPress ? +1 : -1;
                ballFirm = kBallOrbForce > 0;
            } else if (kBallGroundEager > 0 && p1->m_isOnGround && (n.action & kActP1) == 0) {
                auto* self = const_cast<Impl*>(this);
                int go = self->ballPressNow(p1, self->ballParamsFor(p1));
                if (go >= 0) {
                    preferP1 = go > 0 ? kActP1 : 0;
                    ballPlanDecided++;
                    ballGroundEager++;
                    flyVerdict = go > 0 ? +1 : -1;
                    ballFirm = kBallGroundForce > 0;
                }
            } else if (std::abs(pressR - idleR) >= kBallRoomEdge) {
                preferP1 = pressR > idleR ? kActP1 : 0;
                ballPlanDecided++;
                ballRoomDecided++;
                flyVerdict = pressR > idleR ? +1 : -1;
                ballFirm = kBallForce >= 2;
            } else if (kBallSteer > 0) {
                auto* self = const_cast<Impl*>(this);
                self->ensureGapQuery();
                auto prm = self->ballParamsFor(p1);
                auto adv = self->ballPlan.advise(gapQuery, prm, p1->getPositionX(),
                                                 p1->getPositionY(), kBallAhead);
                float dyTo = adv.ok ? adv.bestY - p1->getPositionY() : 0.f;
                if (adv.ok && std::fabs(dyTo) > (float) kBallSteer) {
                    bool wantUp = dyTo > 0.f;
                    bool gravUp = p1->m_isUpsideDown;
                    preferP1 = wantUp != gravUp ? kActP1 : 0;
                    flyVerdict = wantUp != gravUp ? +1 : -1;
                    ballSteered++;
                    ballFirm = kBallForce >= 4;
                }
            }
            if (doom) flyBothDie = true;
        } else if (kBallLook) {
            flyVerdict = flyLookVerdict(p1, (int) mode, (n.action & kActP1) != 0, flyBothDie);
            if (flyVerdict != 0) preferP1 = flyVerdict > 0 ? kActP1 : 0;
        }
    }
    if (kPlanSteer > 0 && p1 && !p1->m_controlsDisabled && planned(mode)
        && (mode == Gamemode::Ship || mode == Gamemode::Swing || mode == Gamemode::Ufo)) {
        auto adv = const_cast<Impl*>(this)->shipPlanAdvise(p1, (int) mode);
        float dy = adv.bestY - p1->getPositionY();
        if (adv.ok && adv.bestDepth > adv.curDepth && std::fabs(dy) > (float) kPlanSteer) {
            bool wantUp = dy > 0.f;
            bool holdGoesUp = !p1->m_isUpsideDown;
            preferP1 = (wantUp == holdGoesUp) ? kActP1 : 0;
            if (wantUp) g_steerUp++; else g_steerDown++;
        }
    }
    if (kRingLate != 0 && p1 && !p1->m_controlsDisabled && !isFlyMode(mode)
        && p1->m_touchingRings && p1->m_touchingRings->count() > 0) {
        preferP1 = kRingLate > 0 ? 0 : kActP1;
    }
    auto insensitive = [&](PlayerObject* p, bool holding) {
        if (!p) return false;
        if (!p->m_touchingRings) ringNull++;
        else if (p->m_touchingRings->count() == 0) ringEmpty++;
        else {
            ringSeen++;
            if (!p->m_isOnGround && !p->m_isOnSlope) ringAir++;
        }
        if (p->m_controlsDisabled) return true;
        auto m = gamemodeOf(p);
        if (isFlyMode(m)) return false;
        if (holding) return false;
        if (p->m_isOnGround || p->m_isOnSlope) return false;
        if (p->m_touchingRings && p->m_touchingRings->count() > 0) return false;
        if (p->m_isDashing) return false;
        if (kBallBuffer > 0 && m == Gamemode::Ball && !p->m_jumpBuffered) return false;
        return true;
    };
    bool p1Insensitive = insensitive(p1, (n.action & kActP1) != 0)
        && (!dual || !pl->m_player2 || insensitive(pl->m_player2, (n.action & kActP1) != 0));
    if (twoPlayer && dual) {
        auto p2 = pl->m_player2;
        auto mode2 = gamemodeOf(p2);
        uint8_t preferP2 = 0;
        if (isFlyMode(mode2) && p2) {
            int v2 = 0;
            if (s_p2Plan && planned(mode2)) {
                bool d2 = false;
                v2 = const_cast<Impl*>(this)->shipPlanPrefer(p2, (int) mode2, K, d2, nullptr, 1);
            } else if (flyLookMode(mode2)) {
                bool d2 = false;
                v2 = flyLookVerdict(p2, (int) mode2, (n.action & kActP2) != 0, d2);
            }
            preferP2 = preferHoldFly(p2, corridor, (n.action & kActP2) != 0, steer, fly, (int) mode2, v2) ? kActP2 : 0;
        } else if (n.action & kActP2) {
            preferP2 = kActP2;
        }
        uint8_t pref = preferP1 | preferP2;
        n.actionCount = 4;
        n.order[0] = pref;
        n.order[1] = pref ^ kActP1;
        n.order[2] = pref ^ kActP2;
        n.order[3] = pref ^ (kActP1 | kActP2);
    } else if (p1Insensitive && !ballExactSaid) {
        n.actionCount = 1;
        n.order[0] = 0;
    } else {
        n.actionCount = 2;
        n.order[0] = preferP1;
        n.order[1] = preferP1 ^ kActP1;
        if (p1 && !dual && mode == Gamemode::Cube && p1->m_isOnGround && !p1->m_isOnSlope
            && !p1->m_controlsDisabled) {
            if (auto a = arc.arcFor(p1)) {
                auto jump = predictJump(p1, *a);
                bool walkBad = walkDies(p1, K);
                uint8_t first = n.order[0];
                if (jump == Verdict::Dies && !walkBad) first = 0;
                else if (walkBad && jump != Verdict::Dies) first = kActP1;
                if (n.order[0] != first) { std::swap(n.order[0], n.order[1]); arcPruned++; }
                if (refuseFatal && jump == Verdict::Dies && !walkBad) {
                    n.actionCount = 1;
                    n.order[0] = 0;
                    arcRefused++;
                } else if (refuseFatal && walkBad && jump != Verdict::Dies) {
                    n.actionCount = 1;
                    n.order[0] = kActP1;
                    arcRefused++;
                }
            } else {
                arcBlind++;
            }
        }
        if (ballFirm && p1 && !dual && mode == Gamemode::Ball && !p1->m_controlsDisabled
            && (ballExactSaid
                || !(p1->m_touchingRings && p1->m_touchingRings->count() > 0))) {
            n.actionCount = 1;
            n.order[0] = preferP1;
            ballForced++;
        }
        if (flyVerdict != 0 && !flyBothDie && p1 && !dual && !p1->m_controlsDisabled
            && flyLookMode(mode) && mode != Gamemode::Ball) {
            uint8_t live = flyVerdict > 0 ? kActP1 : 0;
            bool fatalPress = flyVerdict < 0;
            int ticks = mode == Gamemode::Spider ? std::max(1, K) : flyLookTicks();
            if (flyDeathTick(p1, (int) mode, fatalPress, (n.action & kActP1) != 0, ticks, 0.8f) >= 0) {
                float x = p1->getPositionX(), dir = engine::playerDirection(p1);
                float reach = (float) ticks * (perTickX > 0.f ? perTickX : 1.3f);
                float ax0 = std::min(x, x + dir * reach), ax1 = std::max(x, x + dir * reach);
                if (refuseFatal || !corridor.hasDynamic(ax0, ax1)) {
                    n.actionCount = 1;
                    n.order[0] = live;
                    flyLookDropped++;
                }
            }
        }
        if (refuseFatal && p1 && !dual && !p1->m_controlsDisabled && planned(mode) && isFlyMode(mode)
            && n.actionCount == 2) {
            bool bothDead = false;
            int hold = -1, release = -1;
            const_cast<Impl*>(this)->shipPlanPrefer(p1, (int) mode, K, bothDead, nullptr, 0, &hold, &release);
            planRefuseSeen++;
            if (!bothDead && hold >= 0 && release >= 0 && (hold == 0) != (release == 0)) {
                n.actionCount = 1;
                n.order[0] = hold > 0 ? kActP1 : 0;
                planRefused++;
            }
        }
    }
    n.nextAction = 0;
}

int Solver::Impl::createChild(int parentId, uint8_t action, int guideIdx, float room, bool doomed,
                              uint8_t heldRun, int planDepth, int planPull, bool orbSkipped) {
    Node child;
    child.heldRun = heldRun;
    child.parent = parentId;
    child.tick = engine::state().tick;
    child.action = action;
    child.seq = seqCounter++;
    child.x = engine::playerX(pl);
    child.y = engine::playerY(pl);
    child.speed = pl->m_player1 ? pl->m_player1->m_playerSpeed : 0.f;
    if (pl->m_gameState.m_isDualMode && pl->m_player2) { child.x2 = pl->m_player2->getPositionX(); child.y2 = pl->m_player2->getPositionY(); }
    child.snap = engine::takeSnapshot(pl);
    child.guideIdx = guideIdx;
    child.progress = nodes[parentId].progress + (int) std::lround(std::fabs(child.x - nodes[parentId].x));
    child.room = room;
    child.doomed = doomed;
    child.planDepth = (uint8_t) std::clamp(planDepth, 0, 255);
    child.orbSkipped = orbSkipped;
    child.planPull = (uint16_t) std::clamp(planPull, 0, 65535);
    if (planDepth > maxPlanDepth) { maxPlanDepth = planDepth; maxPlanDepthX = child.x; }
    child.mode = static_cast<uint8_t>(gamemodeOf(pl->m_player1));
    child.items = itemScoreOf(pl);
    if (child.items > mostItems) {
        mostItems = child.items;
        log::info("[solver] item score {} at x {:.0f} ({})", child.items, engine::playerX(pl), describeItems(pl));
    }
    if (doomed) doomedCount++;
    if (backjumpStage > 0 && boosted(nodes[parentId])) child.boostStage = (uint8_t) backjumpStage;
    orderActions(child);
    nodes.push_back(std::move(child));
    int id = (int) nodes.size() - 1;
    noteLane(nodes[id]);
    if (progressOf(nodes[id]) > progressOf(nodes[deepestNodeId])) deepestNodeId = id;
    return id;
}

int Solver::Impl::matchGuide(int from, float x, float y, float& dist) const {
    auto const& g = *segment.guide;
    int n = (int) g.size();
    if (n == 0) { dist = 0.f; return 0; }
    int lo = std::clamp(from, 0, n - 1);
    int hi = std::min(n - 1, lo + kGuideWindow);
    int best = lo;
    float bestD = 1e30f;
    for (int j = lo; j <= hi; j++) {
        float dx = g[j].x - x, dy = g[j].y - y;
        float d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = j; }
    }
    dist = std::sqrt(bestD);
    return best;
}

void Solver::Impl::closeNode(int id) {
    auto& n = nodes[id];
    n.closed = true;
    if (id != 0) {
        if (n.progress < deepestProgress - kFunnelDepth) n.snap.reset();
        else {
            keptClosed.push_back(id);
            if (keptClosed.size() > kKeptClosed) {
                int old = keptClosed.front();
                keptClosed.pop_front();
                if (nodes[old].closed) nodes[old].snap.reset();
            }
        }
    }
}

void Solver::Impl::reopenFrontier() {
    int reopened = 0, freed = 0;
    int limit = deepestProgress - kFunnelDepth;
    for (int id : keptClosed) {
        auto& n = nodes[id];
        if (!n.closed || !n.snap) continue;
        if (n.progress < limit) { n.snap.reset(); freed++; continue; }
        n.closed = false;
        n.nextAction = 0;
        pushOpen(id);
        reopened++;
    }
    keptClosed.clear();
    log::info("[solver] reopened {} closed nodes, freed {}", reopened, freed);
}

void Solver::Impl::pushOpen(int id) {
    auto& n = nodes[id];
    if (n.inOpen || n.closed) return;
    n.inOpen = true;
    open.push(OpenEntry{prioOf(n), n.seq, id});
}

void Solver::Impl::rebuildOpen() {
    std::vector<OpenEntry> all;
    all.reserve(open.size());
    while (!open.empty()) { all.push_back(open.top()); open.pop(); }
    for (auto& e : all) { e.prio = prioOf(nodes[e.node]); open.push(e); }
}

void Solver::Impl::advanceBackjump(double stalled) {
    lastBackjumpTime = now();
    progressSinceBackjump = false;
    if (backjumpStage >= 5) {
        backjumpCycles++;
        clearBackjump();
        rebuildOpen();
        log::info("[solver] backjump cycle done at {} ({:.0f}s)", deepestProgress, stalled);
        return;
    }
    backjumpStage++;
    boostBelow = std::max(0, deepestProgress - kBackjumpStages[backjumpStage - 1]);
    rebuildOpen();
    log::info("[solver] backjump stage {} at {} ({:.0f}s): boost <= {}, {} open",
        backjumpStage, deepestProgress, stalled, boostBelow, open.size());
}

int Solver::Impl::popOpen() {
    while (!open.empty()) {
        auto e = open.top();
        open.pop();
        auto& n = nodes[e.node];
        if (!n.inOpen || n.closed) continue;
        int now = prioOf(n);
        if (now < e.prio) {
            open.push(OpenEntry{now, e.seq, e.node});
            continue;
        }
        n.inOpen = false;
        return e.node;
    }
    return -1;
}

void Solver::Impl::trimOpen() {
    bool byMemory = false;
    if ((++memoryChecks & 255) == 0 && now() - lastMemoryTrim > kMemoryTrimGap) {
        double mem = processMemoryMB();
        if (mem > memoryLimitMB && open.size() > 500) {
            lastMemoryTrim = now();
            byMemory = true;
            maxOpen = std::max<size_t>(1000, std::min(maxOpen, open.size()) / 2);
            log::warn("[solver] process memory {:.0f} MB (limit {:.0f}): open set limit lowered to {}", mem, memoryLimitMB, maxOpen);
        }
    }
    if (open.size() <= maxOpen && !byMemory) return;
    std::vector<OpenEntry> all;
    all.reserve(open.size());
    while (!open.empty()) { all.push_back(open.top()); open.pop(); }
    std::sort(all.begin(), all.end(), [](OpenEntry const& a, OpenEntry const& b) { return b < a; });
    size_t keep = byMemory ? std::min(all.size() / 2, maxOpen) : all.size() * 3 / 4;
    for (size_t i = 0; i < all.size(); i++) {
        if (i < keep || all[i].node == 0) open.push(all[i]);
        else {
            auto& n = nodes[all[i].node];
            n.inOpen = false;
            closeNode(all[i].node);
            n.snap.reset();
        }
    }
    if (byMemory) {
        size_t freed = 0;
        for (size_t id = 1; id < nodes.size(); id++) if (nodes[id].closed && nodes[id].snap) { nodes[id].snap.reset(); freed++; }
        if (freed) log::info("[solver] freed {} kept snapshots of closed nodes (memory)", freed);
    }
    log::info("[solver] trimmed open set to {}{}", open.size(), byMemory ? " (memory)" : "");
}

void Solver::Impl::noteProgress() {
    float x = engine::playerX(pl);
    if (x > furthestX) {
        furthestX = x;
        furthestPercent = engine::percent(pl);
    }
}

void Solver::Impl::dumpStall() {
    dumpedStall = true;
    log::warn("[solver] stuck at tick {} x {:.1f} ({:.2f}%): died {} merged {} stuck {} sky {} guide {} kept {}",
        deepestTick, furthestX, furthestPercent, frDied, frMerged, frStuck, frSky, frGuide, frKept);
    if (!s_dumpPrefix.empty() && s_dumpCount < 40) {
        auto path = fmt::format("{}.stall{}.txt", s_dumpPrefix, ++s_dumpCount);
        if (auto f = std::fopen(path.c_str(), "w")) {
            std::fprintf(f, "# stall at tick %d x %.1f (%.2f%%) pass %d K %d\n", deepestTick, furthestX, furthestPercent, pass, K);
            std::fprintf(f, "# deaths: x y objectID\n");
            for (auto const& d : recentDeaths) std::fprintf(f, "death %.2f %.2f %d %x\n", d.x, d.y, d.obj, d.cause);
            std::fprintf(f, "# open states: tick x y\n");
            int n = 0;
            for (auto const& nd : nodes) {
                if (!nd.inOpen || nd.closed) continue;
                std::fprintf(f, "open %d %.2f %.2f\n", nd.tick, nd.x, nd.y);
                if (++n >= 20000) break;
            }
            {
                std::vector<Corridor::LiveRect> live;
                corridor.liveRectsNear(furthestX - 400.f, furthestX + 400.f, live);
                std::fprintf(f, "# live movable geometry near the stall: id type x0 x1 y0 y1 hazard moved off\n");
                for (auto const& r : live)
                    std::fprintf(f, "live %d %d %.2f %.2f %.2f %.2f %d %d %d\n", r.id, r.type,
                                 r.x0, r.x1, r.y0, r.y1, r.hazard ? 1 : 0, r.moved ? 1 : 0, r.off ? 1 : 0);
            }
            std::fprintf(f, "# deepest lane: tick x y planDepth doomed act mode speed\n");
            for (int id = deepestNodeId; id >= 0; id = nodes[id].parent)
                std::fprintf(f, "lane %d %.2f %.2f %d %d %d %d %.3f\n", nodes[id].tick, nodes[id].x, nodes[id].y,
                             (int) nodes[id].planDepth, nodes[id].doomed ? 1 : 0, (int) nodes[id].action,
                             (int) nodes[id].mode, nodes[id].speed);
            std::fclose(f);
            log::info("[solver] stall data written to {}", path);
        }
    }
}

InputList Solver::Impl::buildPath(int nodeId, uint8_t lastAction, int endTick) const {
    std::vector<std::pair<int, uint8_t>> segments;
    segments.push_back({nodes[nodeId].tick, lastAction});
    for (int id = nodeId; id > 0; id = nodes[id].parent) {
        segments.push_back({nodes[nodes[id].parent].tick, nodes[id].action});
    }
    std::reverse(segments.begin(), segments.end());
    InputList list;
    uint8_t state = nodes[0].action;
    for (auto const& [tick, action] : segments) {
        if ((action & kActP1) != (state & kActP1)) list.push_back({tick, 1, (action & kActP1) != 0, false});
        if (twoPlayer && (action & kActP2) != (state & kActP2)) list.push_back({tick, 1, (action & kActP2) != 0, true});
        state = action;
    }
    (void) endTick;
    return list;
}

int Solver::Impl::replayToBoundary(InputList const& own, int tick, float& x, float& y, engine::SnapshotPtr& out, int laneNode) {
    std::unordered_map<int, std::pair<float, float>> lane;
    if (laneNode >= 0)
        for (int id = laneNode; id > 0; id = nodes[id].parent) lane[nodes[id].tick] = {nodes[id].x, nodes[id].y};
    int divergeTick = -1;
    float divX = 0.f, divY = 0.f, divWantX = 0.f, divWantY = 0.f;
    InputList full = segment.prefix;
    full.insert(full.end(), own.begin(), own.end());
    auto& st = engine::state();
    engine::releaseAll(pl);
    engine::restoreStart(pl);
    corridor.resetMotion();
    size_t cursor = 0;
    while (st.tick < tick) {
        engine::applyDueInputs(pl, full, cursor);
        engine::step(pl);
        corridor.sampleMotion();
        if (divergeTick < 0 && !lane.empty()) {
            auto it = lane.find(st.tick);
            if (it != lane.end()) {
                float px = engine::playerX(pl), py = engine::playerY(pl);
                if (std::fabs(px - it->second.first) > 0.05f || std::fabs(py - it->second.second) > 0.05f) {
                    divergeTick = st.tick; divX = px; divY = py;
                    divWantX = it->second.first; divWantY = it->second.second;
                    log::warn("[solver] replay diverges at tick {}: replay ({:.3f},{:.3f}) vs search ({:.3f},{:.3f})",
                        divergeTick, divX, divY, divWantX, divWantY);
                }
            }
        }
        if (st.diedThisStep) {
            x = engine::playerX(pl);
            y = engine::playerY(pl);
            replayDeathTick = st.tick;
            replayDeathObj = st.deathObjectID;
            return 0;
        }
        if (st.reachedEndThisStep) {
            x = engine::playerX(pl);
            y = engine::playerY(pl);
            return 2;
        }
    }
    engine::applyDueInputs(pl, full, cursor);
    x = engine::playerX(pl);
    y = engine::playerY(pl);
    out = engine::takeSnapshot(pl);
    return out ? 1 : 0;
}

bool Solver::Impl::expand(Solver& s, int nodeId, uint8_t action) {
    auto& node = nodes[nodeId];
    if (!ensureLive(node)) {
        s.m_stats.restores++;
        closeNode(nodeId);
        return false;
    }
    if (liveNode != nodeId) return false;

    auto& st = engine::state();
    {
        auto p1 = pl->m_player1;
        bool freshPress = (action & kActP1) && !(node.action & kActP1);
        bool grounded = p1 && p1->m_isOnGround && !p1->m_isOnSlope && gamemodeOf(p1) == Gamemode::Cube;
        double vyBefore = p1 ? p1->m_yVelocity : 0.0;
        applyAction(action);
        if (freshPress && grounded && p1->m_yVelocity != vyBefore) arc.sampleImpulse(p1, p1->m_yVelocity);
    }
    if (kHeapNodes > 0 && (s.m_stats.nodesExpanded % (uint64_t) kHeapNodes) == 0)
        engine::checkHeap("expand", engine::state().tick);
    bool died = false, ended = false;
    int steps = ballExactLive() ? 1 : K;
    if (kDrivenStep > steps && pl->m_player1 && pl->m_player1->m_controlsDisabled
        && (!pl->m_gameState.m_isDualMode || !pl->m_player2 || pl->m_player2->m_controlsDisabled))
        steps = kDrivenStep;
    for (int i = 0; i < steps; i++) {
        auto p1 = pl->m_player1;
        int flyMode = p1 ? (int) gamemodeOf(p1) : -1;
        bool flyFlipped = p1 && p1->m_isUpsideDown;
        double flyVy = p1 ? p1->m_yVelocity : 0.0;
        float flyY = p1 ? p1->getPositionY() : 0.f;
        float flyX = p1 ? p1->getPositionX() : 0.f;
        bool airborne = p1 && !p1->m_isOnGround && !p1->m_isOnSlope && !p1->m_isDashing;
        uint32_t activationsBefore = st.activationCount;
        engine::step(pl);
        if (p1 && flyMode >= 0 && !st.diedThisStep && (int) gamemodeOf(p1) == flyMode
            && p1->m_isUpsideDown == flyFlipped) {
            fly.sample(flyMode, (action & kActP1) != 0, flyVy, p1->m_yVelocity - flyVy,
                (double) (p1->getPositionY() - flyY), flyFlipped);
            perTickX = std::fabs(p1->getPositionX() - flyX);
            if (flyMode == (int) Gamemode::Cube && airborne && !p1->m_isOnGround && !p1->m_isOnSlope
                && st.activationCount == activationsBefore)
                arc.sampleFlight(p1, flyVy, p1->m_yVelocity, (double) (p1->getPositionY() - flyY));
        }
        corridor.sampleMotion();
        s.m_stats.stepsSimulated++;
        noteProgress();
        if (st.diedThisStep) { died = true; break; }
        if (st.reachedEndThisStep) { ended = true; break; }
        if (st.tick > deepestTick) deepestTick = st.tick;
        if (i + 1 >= K && steps > K && pl->m_player1 && !pl->m_player1->m_controlsDisabled) break;
    }
    float room = 0.f;
    int planDepth = 0;
    int planPull = 0;
    bool doomed = false;
    float bucketScale = 1.f;
    bool orbSkipped = false;
    if (!died && !ended) {
        float px = engine::playerX(pl), py = engine::playerY(pl), top = 0.f;
        bool driven = (pl->m_player1 && pl->m_player1->m_controlsDisabled) || node.actionCount <= 1;
        bool skyBlind = kSkyDynamic > 0 && corridor.hasDynamic(px - kSkyWindow, px + kSkyWindow);
        if (!driven && !skyBlind && corridor.contentTop(px - kSkyWindow, px + kSkyWindow, top) && py > top + kSkyMargin) {
            skyPruned++;
            if (nearFrontier(node)) frSky++;
            liveNode = -1;
            return false;
        }
        bool forced = pl->m_player1 && pl->m_player1->m_controlsDisabled;
        if (kVoidMargin > 0.f && !forced) {
            float bot = 0.f;
            if (corridor.contentBottom(px - kSkyWindow, px + kSkyWindow, bot) && py < bot - kVoidMargin) {
                skyPruned++;
                if (nearFrontier(node)) frSky++;
                liveNode = -1;
                return false;
            }
        }
        float dyMoved = std::fabs(py - node.y);
        bool pinned = std::fabs(px - node.x) < 0.01f && dyMoved > 0.5f;
        int moved = (int) std::lround(std::fabs(px - node.x));
        if (pinned && moved == 0) moved = std::max(1, K);
        int prog = node.progress + moved;
        if (!guided && prog > deepestProgress) { deepestProgress = prog; lastProgressTime = now(); dumpedStall = false; recentDeaths.clear(); progressSinceBackjump = true; resetFrontierReasons(); }
        if (!platformer) {
            auto pp = pl->m_player1;
            bool driven2 = pp && (pp->m_controlsDisabled || pp->m_isDashing);
            float expect = (perTickX > 0.f ? perTickX : 1.3f) * (float) std::max(1, K);
            if (!driven2 && !pinned && expect > 0.5f && std::fabs(px - node.x) < expect * 0.25f) {
                doomed = true;
                stuckMarks++;
                if (nearFrontier(node)) frStuck++;
            }
        }
        if (auto p1 = pl->m_player1) {
            auto mode = gamemodeOf(p1);
            if (isFlyMode(mode)) {
                room = roomOf(p1);
                doomed = shipDoomed(p1, (int) mode);
                if (auto p2 = dualPartner()) {
                    auto m2 = gamemodeOf(p2);
                    if (isFlyMode(m2)) {
                        room = std::min(room, roomOf(p2));
                        if (shipDoomed(p2, (int) m2)) doomed = true;
                    }
                    if (flyLookMode(m2)) {
                        bool d2 = false;
                        flyLookVerdict(p2, (int) m2, (action & kActP1) != 0, d2);
                        if (d2) doomed = true;
                    }
                }
                if (planned(mode)) {
                    bool planSaysDoomed = false;
                    shipPlanPrefer(p1, (int) mode, K, planSaysDoomed, &planDepth);
                    if (kPlanPull > 0) {
                        auto adv = shipPlanAdvise(p1, (int) mode);
                        adviseCalls++;
                        if (adv.bestDepth > maxBestDepth) maxBestDepth = adv.bestDepth;
                        if (adv.ok && adv.curDepth > planDepth) planDepth = adv.curDepth;
                        if (adv.ok && adv.bestDepth > adv.curDepth) {
                            planPull = (int) std::lround(std::fabs(p1->getPositionY() - adv.bestY));
                            if (planPull > 0) {
                                advisePulls++;
                                if (adv.bestY > p1->getPositionY()) pullUp++; else pullDown++;
                                if (planPull > maxPull) {
                                    maxPull = planPull;
                                    maxPullY = p1->getPositionY();
                                    maxPullBestY = adv.bestY;
                                }
                            }
                        }
                    }
                    if (planSaysDoomed) doomed = true;
                } else if (flyLookMode(mode)) {
                    bool bothDie = false;
                    flyLookVerdict(p1, (int) mode, (action & kActP1) != 0, bothDie);
                    if (bothDie) doomed = true;
                }
                if (rankActive) bucketScale = std::clamp(bandOf(p1) / 40.f, kBucketFloor, 4.f);
            } else if (mode == Gamemode::Ball && kBallPlan) {
                bool ballDoom = false;
                int pr = 0, ir = 0;
                int bd = ballPlanDepth(p1, ballDoom, nullptr, nullptr, &pr, &ir);
                if (bd >= 0) {
                    room = (float) std::max(pr, ir);
                    planDepth = bd;
                    if (ballDoom) doomed = true;
                    if (rankActive) bucketScale = std::clamp(bandOf(p1) / 40.f, kBucketFloor, 4.f);
                    if ((action & kActP1) == 0 && (node.action & kActP1) == 0
                        && ballOrbSoon(p1, kBallArm) > 0) {
                        orbSkipped = true;
                    }
                    if (kBallPull > 0) {
                        ensureGapQuery();
                        auto prm = ballParamsFor(p1);
                        auto adv = ballPlan.advise(gapQuery, prm, p1->getPositionX(),
                                                   p1->getPositionY(), kBallAhead);
                        if (adv.ok) {
                            ballAdvised++;
                            planPull = (int) std::lround(std::fabs(p1->getPositionY() - adv.bestY));
                            if (planPull > 0) ballPulled++;
                        }
                    }
                }
            }
            if (s_p2Plan) {
                if (auto p2 = secondPlayer()) {
                    auto m2 = gamemodeOf(p2);
                    if (isFlyMode(m2)) {
                        if (shipDoomed(p2, (int) m2)) { doomed = true; p2Doomed++; }
                        if (planned(m2)) {
                            bool d2 = false;
                            int d2depth = 0;
                            shipPlanPrefer(p2, (int) m2, K, d2, &d2depth, 1);
                            if (d2) { doomed = true; p2Doomed++; }
                            if (kPlanPull > 0) {
                                auto a2 = shipPlanAdvise(p2, (int) m2, 1);
                                if (a2.ok && a2.bestDepth > a2.curDepth) {
                                    int pull2 = (int) std::lround(std::fabs(p2->getPositionY() - a2.bestY));
                                    planPull += pull2;
                                    if (pull2 > maxPull2) maxPull2 = pull2;
                                }
                            }
                        } else if (flyLookMode(m2)) {
                            bool bd2 = false;
                            flyLookVerdict(p2, (int) m2, (action & kActP2) != 0, bd2);
                            if (bd2) { doomed = true; p2Doomed++; }
                        }
                    }
                }
            }
        }
    }
    int guideIdx = 0;
    if (guided && !died) {
        float dist = 0.f;
        guideIdx = matchGuide(node.guideIdx, engine::playerX(pl), engine::playerY(pl), dist);
        if (dist > segment.guideRadius) {
            guidePruned++;
            if (nearFrontier(node)) frGuide++;
            liveNode = -1;
            return false;
        }
        if (guideIdx > deepestProgress) { deepestProgress = guideIdx; lastProgressTime = now(); dumpedStall = false; recentDeaths.clear(); progressSinceBackjump = true; resetFrontierReasons(); }
    }
    if (ended) {
        Solver::Candidate c;
        c.path = buildPath(nodeId, action, st.tick);
        c.boundaryTick = st.tick;
        c.reachedEnd = true;
        c.x = engine::playerX(pl);
        c.y = engine::playerY(pl);
        s.m_candidates.insert(s.m_candidates.begin(), c);
        s.m_result = c.path;
        s.m_found = true;
        s.m_finished = true;
        liveNode = -1;
        log::info("[solver] end reached at tick {} ({} events, {} nodes)", st.tick, c.path.size(), nodes.size());
        return false;
    }
    if (!died && segment.goalTick >= 0 && st.tick >= segment.goalTick + segment.margin) {
        int boundary = nodeId;
        while (boundary > 0 && nodes[boundary].tick > segment.goalTick) boundary = nodes[boundary].parent;
        auto const& b = nodes[boundary];
        engine::SnapshotPtr state = b.snap;
        InputList path = boundary > 0 ? buildPath(b.parent, b.action, b.tick) : InputList{};
        if (state) {
            engine::restoreSnapshot(pl, *state);
        } else {
            bool rooted = false;
            if (nodes[0].snap) {
                rooted = engine::restoreSnapshot(pl, *nodes[0].snap);
            } else if (!segment.root) {
                engine::releaseAll(pl);
                engine::restoreStart(pl);
                corridor.resetMotion();
                rooted = true;
            } else {
                log::warn("[solver] no root state left to rebuild the boundary at tick {}", b.tick);
            }
            if (rooted) {
                size_t cursor = 0;
                while (st.tick < b.tick) {
                    engine::applyDueInputs(pl, path, cursor);
                    engine::step(pl);
                    if (st.diedThisStep) break;
                }
                if (!st.diedThisStep) state = engine::takeSnapshot(pl);
            }
        }
        liveNode = -1;
        if (!state) return false;
        uint64_t key = 0x51ed270693c4ull;
        {
            auto k1 = keyInfo(pl->m_player1);
            mix(key, (uint64_t) (int64_t) std::llround(k1.y / 15.f));
            mix(key, (uint64_t) (int64_t) std::llround(k1.vy / 2.0));
            mix(key, ((uint64_t) k1.mode << 8) | (k1.upsideDown ? 1 : 0) | (k1.onGround ? 2 : 0) | (k1.mini ? 4 : 0) | (k1.reversed ? 8 : 0));
            if (pl->m_gameState.m_isDualMode && pl->m_player2) {
                auto k2 = keyInfo(pl->m_player2);
                mix(key, (uint64_t) (int64_t) std::llround(k2.y / 15.f));
                mix(key, (uint64_t) (int64_t) std::llround(k2.vy / 2.0));
            }
        }
        if (candidateKeys.insert(key).second) {
            float bx = engine::playerX(pl), by = engine::playerY(pl);
            if (replaySecondsPerTick < 0.0 || replaySecondsPerTick * (double) b.tick <= kVerifyCap) {
                float rx = 0.f, ry = 0.f;
                engine::SnapshotPtr real;
                double replayStart = now();
                int outcome = replayToBoundary(path, b.tick, rx, ry, real, boundary);
                if (b.tick > 0) replaySecondsPerTick = (now() - replayStart) / (double) b.tick;
                if (outcome == 2) {
                    Solver::Candidate c;
                    c.path = std::move(path);
                    c.boundaryTick = st.tick;
                    c.reachedEnd = true;
                    c.x = rx;
                    c.y = ry;
                    s.m_candidates.insert(s.m_candidates.begin(), c);
                    s.m_result = c.path;
                    s.m_found = true;
                    s.m_finished = true;
                    log::info("[solver] end reached at tick {} while replaying to the boundary ({} events)",
                        st.tick, c.path.size());
                    return false;
                }
                if (outcome == 0) {
                    int laneTick = 0; float laneX = 0.f, laneY = 0.f;
                    for (int id = boundary; id >= 0; id = nodes[id].parent) {
                        if (nodes[id].tick <= replayDeathTick) { laneTick = nodes[id].tick; laneX = nodes[id].x; laneY = nodes[id].y; break; }
                    }
                    log::warn("[solver] boundary at tick {} dies on replay at tick {} ({:.1f},{:.1f}) obj {}; search had ({:.1f},{:.1f}) at tick {}",
                        b.tick, replayDeathTick, rx, ry, replayDeathObj, laneX, laneY, laneTick);
                    return false;
                }
                if (std::fabs(rx - bx) > 0.01f || std::fabs(ry - by) > 0.01f) {
                    log::info("[solver] boundary at tick {} replays to ({:.2f},{:.2f}), expected ({:.2f},{:.2f})", b.tick, rx, ry, bx, by);
                }
                state = real;
                bx = rx;
                by = ry;
            }
            Solver::Candidate c;
            if (auto bp = pl->m_player1) {
                auto bmode = gamemodeOf(bp);
                if (isFlyMode(bmode) && planned(bmode)) {
                    auto adv = shipPlanAdvise(bp, (int) bmode);
                    if (adv.ok) {
                        c.planDepth = adv.curDepth;
                        c.planPull = (int) std::lround(std::fabs(bp->getPositionY() - adv.bestY));
                    }
                }
            }
            c.path = std::move(path);
            c.boundaryTick = b.tick;
            c.state = state;
            c.x = bx;
            c.y = by;
            c.guideIdx = b.guideIdx;
            log::info("[solver] candidate #{} tick {} at ({:.2f},{:.2f}) {:.2f}% planDepth {} pull {}", s.m_candidates.size() + 1, b.tick, c.x, c.y, engine::percent(pl), c.planDepth, c.planPull);
            s.m_candidates.push_back(std::move(c));
            s.m_found = true;
            if (firstCandidateTime == 0.0) {
                firstCandidateTime = now();
                s.m_result = s.m_candidates.front().path;
                log::info("[solver] segment goal {} reached at tick {}, boundary {} ({} nodes)", segment.goalTick, st.tick, b.tick, nodes.size());
            }
        }
        return false;
    }
    if (died) {
        deaths++;
        s.m_stats.routesEnded++;
        if (recentDeaths.size() < 4000) {
            uint32_t cause = 0;
            if (st.deathObjectID == 0) {
                auto const& c = st.deathCallers;
                auto pos = c.find("+0x");
                if (pos != std::string::npos) cause = (uint32_t) std::strtoul(c.c_str() + pos + 3, nullptr, 16);
            }
            recentDeaths.push_back({engine::playerX(pl), engine::playerY(pl), st.deathObjectID, cause});
        }
        noteWallDeath(nodeId, engine::playerX(pl));
        if (nearFrontier(node)) frDied++;
        liveNode = -1;
        return false;
    }
    uint8_t keyRun = 0;
    if (action & kActP1) keyRun = (uint8_t) std::min((int) node.heldRun + std::max(1, K), 255);
    uint64_t key = keyOf(action, bucketScale, keyRun);
    if (!visited.insert(key).second) {
        s.m_stats.merged++;
        s.m_stats.routesEnded++;
        if (nearFrontier(node)) frMerged++;
        liveNode = -1;
        return false;
    }
    if (nearFrontier(node)) frKept++;
    int childId = createChild(nodeId, action, guideIdx, room, doomed, keyRun, planDepth, planPull,
                              orbSkipped);
    s.m_stats.snapshots++;
    s.m_stats.nodesExpanded++;
    liveNode = childId;
    return true;
}

void Solver::setDumpPrefix(std::string prefix) { s_dumpPrefix = std::move(prefix); s_dumpCount = 0; }

double releaseFreeMemory() {
#ifdef GEODE_IS_WINDOWS
    double before = processMemoryMB();
    _heapmin();
    double after = processMemoryMB();
    return before > after ? before - after : 0.0;
#else
    return 0.0;
#endif
}

double processMemoryUsedMB() { return processMemoryMB(); }
double memoryFreeMB() { return availableMemoryMB(); }

Solver::Solver(PlayLayer* pl) : Solver(pl, Segment{}) {}

Solver::Solver(PlayLayer* pl, Segment segment) : m_impl(std::make_unique<Impl>()), m_layer(pl) {
    auto& I = *m_impl;
    I.pl = pl;
    I.segment = std::move(segment);
    I.twoPlayer = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
    I.platformer = pl->m_levelSettings && pl->m_levelSettings->m_platformerMode;
    I.levelLength = std::max(1.f, pl->m_levelLength);
    I.startTime = now();
    I.K = 4;
    I.baseMemoryMB = processMemoryMB();
    double budgetMB = std::max(200.0, (double) Mod::get()->getSettingValue<int64_t>("memory-limit"));
    if (double avail = availableMemoryMB(); avail > 0.0) {
        double headroom = std::max(kMemoryFloorMB, std::min(avail * 0.75, avail - kMemoryFloorMB));
        if (budgetMB > headroom) {
            log::info("[solver] memory budget {:.0f} MB does not fit: {:.0f} MB free, process at {:.0f} MB -> using {:.0f} MB",
                budgetMB, avail, I.baseMemoryMB, headroom);
            budgetMB = headroom;
        } else {
            log::info("[solver] memory budget {:.0f} MB ({:.0f} MB free, process at {:.0f} MB)",
                budgetMB, avail, I.baseMemoryMB);
        }
    }
    I.memoryLimitMB = I.baseMemoryMB + budgetMB;
    I.maxNodes = (uint64_t) std::clamp(budgetMB * 1024.0 / 4.5, 400000.0, 1500000.0);
    log::info("[solver] node cap {} for a {:.0f} MB budget", I.maxNodes, budgetMB);
    I.corridor.build(pl);
    if (I.platformer) {
        m_failure = "platformer levels are not supported yet";
        m_finished = true;
        return;
    }
    auto err = I.startPass(*this);
    if (!err.empty()) {
        m_failure = err;
        m_finished = true;
    }
}

Solver::~Solver() = default;

void Solver::setVariant(int variant) { m_impl->variant = ((variant % 3) + 3) % 3; }

InputList Solver::deepestPath() const {
    auto& I = *m_impl;
    if (I.nodes.empty() || I.deepestNodeId < 0 || I.deepestNodeId >= (int) I.nodes.size()) return {};
    auto const& deep = I.nodes[I.deepestNodeId];
    if (deep.parent < 0) return {};
    InputList path = I.segment.prefix;
    auto own = I.buildPath(deep.parent, deep.action, deep.tick);
    path.insert(path.end(), own.begin(), own.end());
    return path;
}

int Solver::deepestTick() const { return m_impl->deepestTick; }

void Solver::cancel() {
    m_cancelled = true;
    m_finished = true;
    m_failure = "cancelled";
}

void Solver::runSlice(double budgetSeconds) {
    if (m_finished) return;
    auto& I = *m_impl;
    double t0 = now();
    auto& snapStats = engine::snapshotStats();
    uint64_t restoresBefore = snapStats.restored;

    while (now() - t0 < budgetSeconds) {
        if (I.firstCandidateTime > 0.0) {
            double took = I.firstCandidateTime - I.startTime;
            bool enough = (int) m_candidates.size() >= std::max(1, I.segment.candidates)
                || now() - I.firstCandidateTime > std::max(2.0, I.segment.extraTime * took);
            if (enough) { m_finished = true; break; }
        } else if (now() - I.lastProgressTime > std::clamp(0.2 * m_timeLimit, 90.0, 180.0)) {
            m_failure = fmt::format("no deeper state for {:.0f}s (furthest {:.2f}%)",
                now() - I.lastProgressTime, I.furthestPercent);
            log::info("[solver] giving up on this variant: {}", m_failure);
            m_finished = true;
            break;
        }
        if (I.current < 0) {
            I.current = I.popOpen();
            if (I.current >= 0 && I.backjumpStage > 0 && !I.boosted(I.nodes[I.current])) {
                I.pushOpen(I.current);
                I.current = -1;
                I.advanceBackjump(now() - I.lastProgressTime);
                continue;
            }
            if (I.current < 0) {
                if (I.pass < 2) {
                    I.pass++;
                    I.K = I.pass == 1 ? 2 : 1;
                    log::info("[solver] pass exhausted, refining to K={} (furthest {:.2f}%)", I.K, I.furthestPercent);
                    auto const& deep = I.nodes[I.deepestNodeId];
                    if (deep.parent >= 0) I.warmPath = I.buildPath(deep.parent, deep.action, deep.tick);
                    if (I.firstCandidateTime > 0.0) { m_finished = true; break; }
                    engine::releaseAll(I.pl);
                    if (!I.segment.root) engine::resetToStart(I.pl);
                    auto err = I.startPass(*this);
                    if (!err.empty()) { m_failure = err; m_finished = true; break; }
                    continue;
                }
                if (I.firstCandidateTime > 0.0) { m_finished = true; break; }
                m_failure = fmt::format("search space exhausted (furthest {:.2f}%)", I.furthestPercent);
                m_finished = true;
                break;
            }
        }
        auto& node = I.nodes[I.current];
        if (node.closed || node.nextAction >= node.actionCount) {
            I.closeNode(I.current);
            I.current = -1;
            continue;
        }
        uint8_t action = node.order[node.nextAction++];
        int parentId = I.current;
        bool created = I.expand(*this, parentId, action);
        if (m_finished) break;
        if (I.progressSinceBackjump && I.backjumpStage > 0) {
            I.clearBackjump();
            I.rebuildOpen();
        }
        I.progressSinceBackjump = false;
        if (created) {
            auto& parent = I.nodes[parentId];
            if (parent.nextAction < parent.actionCount) I.pushOpen(parentId);
            else I.closeNode(parentId);
            I.current = (int) I.nodes.size() - 1;
            I.trimOpen();
        }
        if (I.nodes.size() >= I.maxNodes) {
            m_failure = fmt::format("node limit reached (furthest {:.2f}%)", I.furthestPercent);
            m_finished = true;
            break;
        }
        if (now() - I.startTime > m_timeLimit) {
            if (I.firstCandidateTime == 0.0) m_failure = fmt::format("time limit reached (furthest {:.2f}%)", I.furthestPercent);
            m_finished = true;
            break;
        }
    }

    m_stats.restores += snapStats.restored - restoresBefore;
    m_stats.seconds = now() - I.startTime;
    m_furthestPercent = I.furthestPercent;
    m_furthestX = I.furthestX;
    I.sliceTime += now() - t0;

    double stalled = now() - I.lastProgressTime;
    bool wantRank = stalled > I.kRankDelay;
    if (!m_finished && wantRank != I.rankActive) {
        I.rankActive = wantRank;
        I.rebuildOpen();
    }
    double sinceBackjump = now() - std::max(I.lastProgressTime, I.lastBackjumpTime);
    if (!m_finished && sinceBackjump > 6.0 && I.deepestTick > 0) I.advanceBackjump(stalled);
    if (!m_finished && I.guided && stalled > 12.0 && I.guideWidenings < Impl::kGuideWidenSteps
        && now() - I.lastGuideWiden > 12.0) {
        I.guideWidenings++;
        I.lastGuideWiden = now();
        float was = I.segment.guideRadius;
        I.segment.guideRadius = std::min(was * 1.8f, I.guideRadius0 * 8.f);
        log::info("[solver] stuck at guide index {} ({:.0f}s), band {:.0f} to {:.0f}", I.deepestProgress, stalled, was, I.segment.guideRadius);
        I.reopenFrontier();
    }
    if (!m_finished && stalled > 15.0 && !I.dumpedStall) I.dumpStall();
    double refineAfter = std::clamp(0.15 * m_timeLimit, 10.0, 30.0);
    bool flyWall = I.flyAtFrontier();
    bool mayRefine = !flyWall || I.backjumpCycles > 0;
    if (!m_finished && stalled > refineAfter && I.K > 1 && I.stallRefinements < 2 && mayRefine) {
        I.K /= 2;
        I.stallRefinements++;
        I.lastProgressTime = now();
        I.lastBackjumpTime = now();
        if (!flyWall) I.clearBackjump();
        I.rebuildOpen();
        I.dumpedStall = false;
        log::info("[solver] stalled at {:.2f}% for {:.0f}s, K={}{}", I.furthestPercent, stalled, I.K, flyWall ? " (fly)" : "");
        I.reopenFrontier();
    } else if (!m_finished && flyWall && stalled > refineAfter && I.K > 1 && I.stallRefinements < 2
               && !I.loggedFlyHold) {
        I.loggedFlyHold = true;
        log::info("[solver] fly stall at {:.2f}% ({:.0f}s), staying at K={} for backjump stage {}", I.furthestPercent, stalled, I.K, I.backjumpStage);
    }

    int pdDeep = I.nodes.empty() ? 0 : (int) I.nodes[std::clamp(I.deepestNodeId, 0, (int) I.nodes.size() - 1)].planDepth;
    static double lastLog = 0.0;
    if (now() - lastLog > 2.0) {
        lastLog = now();
        log::info("[solver] t={:.1f}s pass={} K={} nodes={} open={} merged={} deaths={} steps={} restores={} badRestores={} deepest={} progress={}{} furthest={:.2f}%{}{}{}{}{}{}",
            m_stats.seconds, I.pass, I.K, I.nodes.size(), I.open.size(), m_stats.merged, I.deaths, m_stats.stepsSimulated,
            m_stats.restores, engine::snapshotStats().restoreMismatches, I.deepestTick, I.deepestProgress,
            I.guided ? fmt::format(" guide={}/{} pruned={} band={:.0f}{}", I.deepestProgress, I.segment.guide->size(),
                I.guidePruned, I.segment.guideRadius,
                I.guideWidenings > 0 ? fmt::format("(+{})", I.guideWidenings) : "") : "",
            I.furthestPercent, I.backjumpStage > 0 ? fmt::format(" backjump<={}", I.boostBelow) : "",
            I.skyPruned > 0 ? fmt::format(" sky={}", I.skyPruned) : "",
            I.doomedCount > 0 || I.funnelMarks > 0 ? fmt::format(" doomed={} funnel={}{}", I.doomedCount, I.funnelMarks,
                I.stuckMarks > 0 ? fmt::format(" stuck={}", I.stuckMarks) : "") : "",
            I.arcPruned > 0 || I.arcBlind > 0 || I.ringSeen > 0 || I.ringNull > 0 || I.ringEmpty > 0 || I.planRefuseSeen > 0 || I.flyLookQueries > 0 || I.flyLookBlind > 0 || I.ballPlanCalls > 0 ? fmt::format(" arc={}/{}/{} ring={}/{} rnull={} rempty={} prefuse={}/{} look={}q/{}dec/{}doom/{}blind/{}drop bplan={}/{}/{} ({} built, {:.1f}s, offgrid {}) pull={}/{} steer={} clr={} calm={} orb={} gnd={} firm={}", I.arcPruned, I.arcBlind, I.arcRefused, I.ringSeen, I.ringAir, I.ringNull, I.ringEmpty, I.planRefused, I.planRefuseSeen, I.flyLookQueries, I.flyLookDecided, I.flyLookDoomed, I.flyLookBlind, I.flyLookDropped, I.ballPlanDecided, I.ballPlanDoomed, I.ballPlanCalls, I.ballPlan.plansBuilt, I.ballPlan.planSeconds, I.ballPlan.offGrid, I.ballPulled, I.ballAdvised, I.ballSteered, I.ballRoomDecided, I.ballCalmed, I.ballOrbEager, I.ballGroundEager, I.ballForced) : "",
            I.flyLookQueries > 0 ? fmt::format(" look={}/{}/{}@{}{}", I.flyLookDecided, I.flyLookDoomed,
                I.flyLookQueries, I.flyLookTicks(),
                fmt::format("{}{}", I.flyLookDropped > 0 ? fmt::format(" dropped={}", I.flyLookDropped) : "",
                    I.flyLookBlind > 0 ? fmt::format(" blind={}", I.flyLookBlind) : "")) : "",
            I.shipPlan.plansBuilt > 0 ? fmt::format(" plan={}/{}/{} ({} built, {:.1f}s, offaxis {}, offgrid {}, vmax {:.1f}, rebuild m{} s{} st{} y{} a{})", I.planPrefs, I.planSilent, I.planDoomed, I.shipPlan.plansBuilt, I.shipPlan.planSeconds, I.shipPlan.offAxis, I.shipPlan.offGrid, I.fly.vTop[1], I.shipPlan.reMode, I.shipPlan.reStale, I.shipPlan.reStep, I.shipPlan.reY, I.shipPlan.reAcc) + fmt::format(" pd={}/{}@x{:.0f} adv={}/{} bd={} mp={}@y{:.0f}->y{:.0f} up/dn={}/{} steer={}/{} p2={}/{}", pdDeep, I.maxPlanDepth, I.maxPlanDepthX, I.advisePulls, I.adviseCalls, I.maxBestDepth, I.maxPull, I.maxPullY, I.maxPullBestY, I.pullUp, I.pullDown, g_steerUp, g_steerDown, I.p2Doomed, I.maxPull2) : "");
        {
            auto const& ss = engine::snapshotStats();
            if (ss.taken > 0) log::debug("[solver] snapshots: {} taken, {:.1f} pose + {:.1f} flag states each, restore avg {:.2f} ms, stale collision logs {}",
                ss.taken, (double) ss.poseStates / (double) ss.taken, (double) ss.flagStates / (double) ss.taken,
                ss.restored ? ss.restoreSeconds / (double) ss.restored * 1000.0 : 0.0, engine::staleCollisionLogs());
        }
        double mem = processMemoryMB();
        if (mem > 0.0) log::debug("[solver] process memory {:.0f} MB (+{:.0f} since start)", mem, mem - I.baseMemoryMB);
    }
}


void Solver::setDoomPenalty(int v) { kDoomPenalty = std::max(0, v); }
void Solver::setLaneCap(int v) { kLaneCap = std::max(0, v); }
void Solver::setFunnelDepth(int v) { kFunnelDepth = std::max(30, v); }
void Solver::setRoomWeight(float v) { kRoomWeight = std::max(0.f, v); }
void Solver::setBucketFloor(float v) { kBucketFloor = std::clamp(v, 0.05f, 4.f); }
void Solver::setPlanWindows(int v) { ShipPlan::s_windows = std::clamp(v, 8, ShipPlan::kWindowsMax); }
void Solver::setPlanYCells(int v) { ShipPlan::s_yCells = std::clamp(v, 40, ShipPlan::kYCellsMax); }
void Solver::setPlanAccTol(float v) { ShipPlan::s_accTol = std::clamp(v, 0.f, 2.f); }
void Solver::setPlanWeight(int v) { kPlanWeight = std::max(0, v); }
void Solver::setPlanBodyX(float v) { ShipPlan::s_bodyX = std::clamp(v, 0.f, 2.f); }
void Solver::setPlanSky(float v) { ShipPlan::s_skyMargin = v; }
void Solver::setPlanPull(int v) { kPlanPull = std::max(0, v); }
void Solver::setPlanPullCap(int v) { kPlanPullCap = std::max(0, v); }
void Solver::setP2Plan(bool v) { s_p2Plan = v; }
void Solver::setPlanInner(float v) { kPlanInner = std::clamp(v, 0.05f, 1.5f); }
void Solver::setPlanVTop(double v) { kPlanVTop = std::clamp(v, 0.0, 60.0); }
void Solver::setPlanVMax(double v) { kPlanVMax = std::clamp(v, 0.0, 60.0); }
void Solver::setPlanSteer(int v) { kPlanSteer = std::max(0, v); }
void Solver::setVoidMargin(float v) { kVoidMargin = std::max(0.f, v); }
void Solver::setHeapNodes(int v) { kHeapNodes = std::max(0, v); }
void Solver::setSkyMargin(float v) { kSkyMargin = std::max(20.f, v); }
void Solver::setSkyDynamic(int v) { kSkyDynamic = v; }
void Solver::setDrivenStep(int v) { kDrivenStep = std::max(1, v); }
void Solver::setMovedKey(float v) { kMovedKey = std::max(0.f, v); }
void Solver::setMovedQuant(float v) { kMovedQuant = std::max(1.f, v); }
void Solver::setVerifyCap(double v) { kVerifyCap = std::max(1.0, v); }
void Solver::setPoseOverride(int v) { engine::setPoseOverride(v != 0); }
void Solver::setMoverSweep(float v) { Corridor::s_moverSweep = std::max(0.f, v); }
void Solver::setKeyBonus(int v) { kKeyBonusArg = std::max(0, v); }
void Solver::setItemPull(int v) { kItemPull = std::max(0, v); }
void Solver::setItemReach(float v) { kItemReach = std::max(0.f, v); }
void Solver::setItemPullCap(int v) { kItemPullCap = std::max(0, v); }
void Solver::setItemId(int v) { kItemId = std::max(0, v); }
void Solver::setRingPull(int v) { kRingPull = std::max(0, v); }
void Solver::setRingReach(float v) { kRingReach = std::max(0.f, v); }
void Solver::setRingPullCap(int v) { kRingPullCap = std::max(0, v); }
void Solver::setRingLate(int v) { kRingLate = v; }
void Solver::setLookAheadSeconds(double v) { kLookAheadSeconds = v; }
void Solver::setSpeedBonus(double v) { kSpeedBonus = v; }
void Solver::setBallLook(int v) { kBallLook = v; }
void Solver::setBallAccel(double v) { kBallAccel = v; kBallVy = 0.0; }
void Solver::setBallVy(double v) { kBallVy = std::max(0.0, v); }
void Solver::setBallYStart(double v) { kBallYStart = std::max(0.01, v); }
void Solver::setBallVyToY(double v) { kBallVyToY = std::max(0.01, v); }
void Solver::setBallGroundRatio(double v) { kBallGroundRatio = std::max(0.0, v); }
void Solver::setBallBlueRatio(double v) { kBallBlueRatio = std::max(0.0, v); }
void Solver::setBallProbe(float lo, float hi) { kBallProbeLo = lo; kBallProbeHi = hi; }
void Solver::setBallGravRef(double v) { kBallGravRef = std::max(0.0, v); }
void Solver::setBallPlan(int v) { kBallPlan = v; }
void Solver::setBallTrace(int v) { s_ballTrace = v; }
void Solver::setBallPull(int v) { kBallPull = std::max(0, v); }
void Solver::setBallPullCap(int v) { kBallPullCap = std::max(0, v); }
void Solver::setBallAhead(int v) { kBallAhead = std::max(0, v); }
void Solver::setBallSteer(int v) { kBallSteer = std::max(0, v); }
void Solver::setBallRoomEdge(int v) { kBallRoomEdge = std::max(1, v); }
void Solver::setBallForce(int v) { kBallForce = std::max(0, v); }
void Solver::setBallCalm(int v) { kBallCalm = std::max(0, v); }
void Solver::setBallClear(int v) { kBallClear = std::max(0, v); }
void Solver::setBallOrbEager(int v) { kBallOrbEager = std::max(0, v); }
void Solver::setBallOrbSkip(int v) { kBallOrbSkip = std::max(0, v); }
void Solver::setBallArm(int v) { kBallArm = std::max(0, v); }
void Solver::setBallGroundEager(int v) { kBallGroundEager = std::max(0, v); }
void Solver::setBallGroundForce(int v) { kBallGroundForce = std::max(0, v); }
void Solver::setBallPhaseReach(int v) { kBallPhaseReach = std::max(0, v); }
void Solver::setBallCrossCap(int v) { kBallCrossCap = std::max(1, v); }
void Solver::setBallExact(int v) { kBallExact = std::max(0, v); }
void Solver::setBallExactForce(int v) { kBallExactForce = std::max(0, v); }
void Solver::setBallExactHorizon(int v) { kBallExactHorizon = std::max(1, v); }
void Solver::setBallExactBeam(int v) { kBallExactBeam = std::max(1, v); }
void Solver::setBallExactCap(int v) { kBallExactCap = std::max(0, v); }
void Solver::setBallExactTrace(int v) { ballexact::setTrace(v); }
void Solver::setBallBuffer(int v) { kBallBuffer = std::max(0, v); }
void Solver::setBallCache(int v) { kBallCache = std::max(1, v); }
void Solver::setBallOrbForce(int v) { kBallOrbForce = std::max(0, v); }
void Solver::setBallSurvive(int v) { kBallSurvive = std::max(0, v); }
void Solver::setBallDyMax(double v) { kBallDyMax = v; }
void Solver::setBallInner(double v) { kBallInner = v; }
void Solver::setPortalPull(int v) { kPortalPull = v; }
void Solver::setPortalReach(double v) { kPortalReach = (float) v; }
void Solver::setPortalPullCap(int v) { kPortalPullCap = v; }
void Solver::setRefuseFatal(int v) { kRefuseFatal = v; }

}
