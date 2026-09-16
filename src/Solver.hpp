#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "Session.hpp"
#include "Snapshot.hpp"

namespace gdpf {
double releaseFreeMemory();
double processMemoryUsedMB();
double memoryFreeMB();

class Solver {
public:
    struct Stats {
        uint64_t nodesExpanded = 0;
        uint64_t stepsSimulated = 0;
        uint64_t snapshots = 0;
        uint64_t restores = 0;
        uint64_t merged = 0;
        uint64_t routesEnded = 0;
        double seconds = 0.0;
    };

    struct GuidePoint { float x, y; };
    using Guide = std::shared_ptr<const std::vector<GuidePoint>>;

    struct Segment {
        engine::SnapshotPtr root;
        InputList prefix;
        int goalTick = -1;
        int margin = 60;
        int candidates = 24;
        double extraTime = 3.0;
        Guide guide;
        float guideRadius = 120.f;
        int rootGuideIdx = 0;
    };

    struct Candidate {
        InputList path;
        int boundaryTick = 0;
        engine::SnapshotPtr state;
        bool reachedEnd = false;
        float x = 0.f, y = 0.f;
        int guideIdx = 0;
        int planDepth = -1;
        int planPull = 0;
    };

    explicit Solver(PlayLayer* pl);
    Solver(PlayLayer* pl, Segment segment);
    ~Solver();

    void runSlice(double budgetSeconds);
    void cancel();
    void setTimeLimit(double seconds) { m_timeLimit = seconds; }
    void setVariant(int variant);
    static void setDumpPrefix(std::string prefix);
    static void setDoomPenalty(int v);
    static void setLaneCap(int v);
    static void setFunnelDepth(int v);
    static void setRoomWeight(float v);
    static void setBucketFloor(float v);
    static void setKeyBonus(int v);
    static void setItemPull(int v);
    static void setItemReach(float v);
    static void setItemPullCap(int v);
    static void setItemId(int v);
    static void setRingPull(int v);
    static void setRingReach(float v);
    static void setRingPullCap(int v);
    static void setRingLate(int v);
    static void setLookAheadSeconds(double v);
    static void setSpeedBonus(double v);
    static void setBallLook(int v);
    static void setBallAccel(double v);
    static void setBallPlan(int v);
    static void setBallSurvive(int v);
    static void setBallVy(double v);
    static void setBallYStart(double v);
    static void setBallVyToY(double v);
    static void setBallGroundRatio(double v);
    static void setBallBlueRatio(double v);
    static void setBallProbe(float lo, float hi);
    static void setBallGravRef(double v);
    static void setBallTrace(int v);
    static void setBallPull(int v);
    static void setBallPullCap(int v);
    static void setBallAhead(int v);
    static void setBallSteer(int v);
    static void setBallRoomEdge(int v);
    static void setBallForce(int v);
    static void setBallCalm(int v);
    static void setBallClear(int v);
    static void setBallOrbEager(int v);
    static void setBallOrbSkip(int v);
    static void setBallArm(int v);
    static void setBallGroundEager(int v);
    static void setBallGroundForce(int v);
    static void setBallPhaseReach(int v);
    static void setBallCrossCap(int v);
    static void setBallExact(int v);
    static void setBallExactForce(int v);
    static void setBallExactHorizon(int v);
    static void setBallExactBeam(int v);
    static void setBallExactCap(int v);
    static void setBallExactTrace(int v);
    static void setBallBuffer(int v);
    static void setBallCache(int v);
    static void setBallOrbForce(int v);
    static void setBallDyMax(double v);
    static void setBallInner(double v);
    static void setPortalPull(int v);
    static void setPortalReach(double v);
    static void setPortalPullCap(int v);
    static void setRefuseFatal(int v);
    static void setPlanWindows(int v);
    static void setPlanYCells(int v);
    static void setPlanAccTol(float v);
    static void setPlanWeight(int v);
    static void setPlanBodyX(float v);
    static void setPlanSky(float v);
    static void setPlanPull(int v);
    static void setPlanPullCap(int v);
    static void setP2Plan(bool v);
    static void setPlanInner(float v);
    static void setPlanVTop(double v);
    static void setPlanVMax(double v);
    static void setPlanSteer(int v);
    static void setSkyMargin(float v);
    static void setVoidMargin(float v);
    static void setHeapNodes(int v);
    static void setSkyDynamic(int v);
    static void setDrivenStep(int v);
    static void setMovedKey(float v);
    static void setMovedQuant(float v);
    static void setVerifyCap(double v);
    static void setPoseOverride(int v);
    static void setMoverSweep(float v);

    bool finished() const { return m_finished; }
    bool found() const { return m_found; }
    bool cancelled() const { return m_cancelled; }
    InputList const& result() const { return m_result; }
    std::vector<Candidate> const& candidates() const { return m_candidates; }
    float furthestPercent() const { return m_furthestPercent; }
    float furthestX() const { return m_furthestX; }
    InputList deepestPath() const;
    int deepestTick() const;
    Stats const& stats() const { return m_stats; }
    std::string const& failureReason() const { return m_failure; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    PlayLayer* m_layer = nullptr;
    bool m_finished = false;
    bool m_found = false;
    bool m_cancelled = false;
    InputList m_result;
    std::vector<Candidate> m_candidates;
    float m_furthestPercent = 0.f;
    float m_furthestX = 0.f;
    Stats m_stats;
    std::string m_failure;
    double m_timeLimit = 600.0;
};

}
