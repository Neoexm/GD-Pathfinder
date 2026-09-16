#pragma once

#include <cmath>

#include <Geode/Geode.hpp>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Engine.hpp"
#include "Session.hpp"
#include "Solver.hpp"

namespace gdpf {

class Experiment;

struct SafeModeState {
    bool captured = false;
    int statJumps = 0;
    int statAttempts = 0;
    int levelAttempts = 0;
    int levelJumps = 0;
    int levelClicks = 0;
    int normalPercent = 0;
    int practicePercent = 0;
    int orbCompletion = 0;
    int newNormalPercent2 = 0;
    int bestTime = 0;

    void capture(PlayLayer* pl);
    void restore(PlayLayer* pl);
};

class Bot {
public:
    static Bot& get();

    void requestSolve(GJGameLevel* level, bool playAfter = true, bool allowCache = true);
    void setSolveTimeLimit(double seconds) { m_solveTimeLimit = seconds; }
    void setResumeFile(std::string path) { m_resumePath = std::move(path); }
    void setSegmentCap(double s) { m_segmentCap = s > 0.0 ? s : 0.0; }
    void setSegmentTicks(int t) { m_segmentTicksArg = t > 0 ? t : 0; }
    static void setSegmentScaleMax(int v);
    static void setShowSearch(int v);
    void setPlanOrder(bool on) { m_planOrder = on; }
    void setSegCandidates(int n) { m_segCandidates = n; }
    void setSegExtra(double v) { m_segExtra = v; }
    void setSegMargin(int v) { m_segMargin = v; }
    void setCandFirst(bool on) { m_candFirst = on; }
    void setCandY(int v) { m_candY = v; }
    void setCandX(bool v) { m_candX = v; }
    void setLeanOverride(int v) { m_leanOverride = v; }
    bool setGuideFile(std::string const& path, float radius);
    void requestPlay(GJGameLevel* level, InputList inputs);
    void requestVerify(GJGameLevel* level, InputList inputs, int runs);
    void requestExperiment(GJGameLevel* level, std::unique_ptr<Experiment> experiment);
    void clearPending();
    bool hasPendingFor(GJGameLevel* level) const;
    Request pendingRequest() const { return m_pending; }

    void onPlayLayerInit(PlayLayer* pl);
    void onLevelStarted(PlayLayer* pl);
    void onAfterReset(PlayLayer* pl);
    void onQuit(PlayLayer* pl);
    bool onDeath(PlayLayer* pl, PlayerObject* player, GameObject* object);
    bool onReachedEnd(PlayLayer* pl);
    void onLevelComplete(PlayLayer* pl);
    void onLayerUpdate(PlayLayer* pl, float dt);
    void onFrame(float dt);

    bool active() const { return m_mode != Mode::Idle; }
    bool activeOn(PlayLayer* pl) const { return active() && m_layer == pl; }
    Mode mode() const { return m_mode; }
    bool solving() const { return m_mode == Mode::Solving; }
    bool fastStepping() const { return m_mode == Mode::Solving || m_mode == Mode::Verifying || m_mode == Mode::Experiment; }
    bool inputBlocked() const;

    void cancel();

    std::vector<RunResult> const& runResults() const { return m_runResults; }
    InputList const& inputs() const { return m_inputs; }
    void setInputs(InputList inputs) { m_inputs = std::move(inputs); }
    std::string const& levelKey() const { return m_levelKey; }

    std::function<void(Bot&)> onFinished;

    void startPlayback(PlayLayer* pl);
    void startVerification(PlayLayer* pl, int runs);

    Solver* solver() { return m_solver.get(); }
    float progressPercent() const;
    uint64_t progressRoutes() const;
    int progressAttempts() const { return m_segmentAttempts; }
    double solveSeconds() const;
    PlayLayer* layer() const { return m_layer; }
    std::string const& experimentSummary() const { return m_experimentSummary; }
    bool experimentPassed() const { return m_experimentPassed; }
    std::string const& lastFailure() const { return m_lastFailure; }

    bool showTrajectory() const { return m_showTrajectory; }

private:
    Bot() = default;
    void beginSession(PlayLayer* pl, Mode mode);
    void endSession() { endSession(true); }
    void finishRun(PlayLayer* pl, RunResult result);
    void startSolve(PlayLayer* pl);
    void onSolveFinished(PlayLayer* pl, bool found);
    void savePath(PlayLayer* pl, int ticks, bool verified = true);
    void launchSolver(PlayLayer* pl);
    void launchSegment(PlayLayer* pl);
    void segmentFailed(PlayLayer* pl);
    void savePartial(PlayLayer* pl);
    void finishSearchFailure(PlayLayer* pl, bool cancelled);
    bool startLeanSearch(PlayLayer* pl, Request next);
    void setForeignGameplayHooks(bool enabled);
    std::set<geode::Hook*> m_disabledForeignHooks;
    void switchToLevel(GJGameLevel* level, Request next);
    void endSession(bool requestDone);
    void reportFailure(PlayLayer* pl, std::string const& text);

    Mode m_mode = Mode::Idle;
    Request m_pending = Request::None;
    geode::Ref<GJGameLevel> m_pendingLevel;
    double m_pendingSince = 0.0;
    PlayLayer* m_layer = nullptr;
    std::string m_levelKey;
    bool m_allowCache = true;

    InputList m_inputs;
    size_t m_cursor = 0;
    double m_timeAccumulator = 0.0;
    int m_runsRemaining = 0;
    RunResult m_current;
    std::vector<RunResult> m_runResults;
    struct TrajPoint { float x, y; double vy; uint32_t flags; double lastJump; };
    std::vector<TrajPoint> m_refTrajectory;
    int m_trajMismatches = 0;
    bool m_reportedJumpClock = false;
    int m_waitTicks = 0;
    bool m_playAfterVerify = false;
    int m_solveAttempt = 0;
    double m_solveDeadline = 0.0;
    double m_solveStarted = 0.0;
    float m_bestPercent = 0.f;
    uint64_t m_nodesTotal = 0;
    uint64_t m_routesTotal = 0;
    int m_segmentAttempts = 0;

    struct SolvedSegment {
        std::vector<Solver::Candidate> candidates;
        size_t next = 0;
        InputList prefix;
        int rootTick = 0;
    };
    std::vector<SolvedSegment> m_segments;
    int m_segmentTicks = 0;
    int m_segmentScale = 1;
    static constexpr int kSteeringVariants = 3;
    int m_segmentVariant = 0;
    float m_variantBest[kSteeringVariants] = {-1.f, -1.f, -1.f};
    int m_variantOrder[kSteeringVariants] = {0, 1, 2};
    int m_variantCount = kSteeringVariants;
    int m_variantIdx = 0;
    static constexpr float kVariantSlack = 200.f;
    void resetVariants(int first);
    void orderVariants();
    InputList m_segmentPrefix;
    InputList m_deepestPath;
    int m_deepestTick = 0;
    Solver::Guide m_guide;
    float m_guideRadius = 120.f;

    geode::Ref<GJGameLevel> m_realLevel;
    geode::Ref<GJGameLevel> m_leanLevel;
    bool m_leanPhase = false;
    bool m_continueRequest = false;
    std::string m_realKey;
    std::string m_resumeMessage;
    bool m_playWhenSolved = true;
    double m_solveTimeLimit = 600.0;
    struct StartState {
        bool  ok = false;
        float y = 0.f;
        float speed = 0.f;
        float size = 0.f;
        bool  flipped = false;
        int   mode = 0;
        bool sameAs(StartState const& o) const {
            if (!ok || !o.ok) return true;          // nothing to compare: do not block
            return mode == o.mode && flipped == o.flipped
                && std::abs(y - o.y) <= 1.f
                && std::abs(speed - o.speed) <= 0.01f
                && std::abs(size - o.size) <= 0.01f;
        }
    };
    static StartState startStateOf(PlayLayer* pl);
    StartState m_realStart;
    std::string m_resumePath;
    double m_segmentCap = 120.0;
    int m_segmentTicksArg = 0;
    bool m_planOrder = true;
    int m_segCandidates = 0;
    double m_segExtra = -1.0;
    int m_segMargin = -1;
    bool m_candFirst = true;
    int m_candY = 0;
    bool m_candX = true;
    int m_leanOverride = -1;
    InputList m_resumeInputs;
    int m_resumeTicks = 0;
    bool m_resumeStale = false;
    bool m_resumePending = false;
    bool m_resumeNext = false;
    InputList m_bestPrefix;
    int m_bestPrefixTick = 0;
    void resumeFromPartial(PlayLayer* pl);
    bool rootFromPartial(PlayLayer* pl, InputList const& inputs, int ticks, char const* why, int* reachedOut = nullptr);

    SafeModeState m_safe;
    std::unique_ptr<Solver> m_solver;
    std::unique_ptr<Experiment> m_experiment;
    std::string m_experimentSummary;
    bool m_experimentPassed = false;
    std::string m_lastFailure;
    bool m_showTrajectory = false;
};

std::string makeLevelKey(GJGameLevel* level);
std::string levelDisplayName(GJGameLevel* level);

}
