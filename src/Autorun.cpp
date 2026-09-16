#include "Autorun.hpp"

#include <filesystem>

#include <Geode/Geode.hpp>
#include <fstream>
#include <iterator>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/utils/general.hpp>
#include <algorithm>
#include <vector>

#include "Bot.hpp"
#include "Corridor.hpp"
#include "Engine.hpp"
#include "Experiments.hpp"
#include "InputFile.hpp"
#include "Solver.hpp"

using namespace geode::prelude;

namespace gdpf::autorun {

static void startWithLevel(GJGameLevel* level);

namespace {
    enum class Phase { Disabled, WaitingForMenu, Delay, Starting, Running, Done };

    struct Config {
        std::string level;
        std::string mode = "solve-play";
        int runs = 3;
        int interval = 100;
        std::string inputsPath;
        bool exitWhenDone = true;
        float delay = 2.f;
        double solveTimeout = 0.0;
        bool useCache = false;
        double seconds = 45.0;
        int snapTick = 960;
        float diffX = -1.f;
        int runTicks = 2400;
        int diffTick = 0;
        int churn = 200;
        int traceTicks = 0;
    };

    Config s_cfg;
    Phase s_phase = Phase::Disabled;
    float s_timer = 0.f;
    double s_startedAt = 0.0;

    bool s_noclip = false;
    std::vector<float> s_frames;
    double s_fpsElapsed = 0.0;
    float s_fpsWarmup = 3.f;

    int countNodes(cocos2d::CCNode* node) {
        if (!node) return 0;
        int n = 1;
        auto children = node->getChildren();
        if (children) {
            for (unsigned int i = 0; i < children->count(); i++) {
                n += countNodes(static_cast<cocos2d::CCNode*>(children->objectAtIndex(i)));
            }
        }
        return n;
    }

    InputList hardcodedInputs() {
        InputList list;
        for (int t : {330, 640, 960, 1260, 1500, 1800, 2100, 2400, 2700, 3000}) {
            list.push_back({t, 1, true, false});
            list.push_back({t + 4, 1, false, false});
        }
        return list;
    }

    // Build a level straight out of a .gmd export. GD's level servers refuse downloads often
    // enough that depending on them for an unattended run is a liability, and the export
    // carries the same level string, so the level key comes out identical.
    GJGameLevel* levelFromGmd(std::string const& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { log::error("[autorun] cannot open {}", path); return nullptr; }
        std::string raw{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        auto grab = [&](char const* key, char tag) -> std::string {
            std::string open = fmt::format("<k>{}</k><{}>", key, tag);
            auto a = raw.find(open);
            if (a == std::string::npos) return "";
            a += open.size();
            auto b = raw.find(fmt::format("</{}>", tag), a);
            return b == std::string::npos ? std::string() : raw.substr(a, b - a);
        };
        std::string ls = grab("k4", 's');
        if (ls.empty()) { log::error("[autorun] no level string in {}", path); return nullptr; }
        std::string name = grab("k2", 's');
        std::string ids = grab("k1", 'i');
        auto lvl = GJGameLevel::create(CCDictionary::create(), false);
        lvl->m_levelString = ls;
        lvl->m_levelName = name.empty() ? std::string("gmd level") : name;
        lvl->m_levelID = ids.empty() ? 0 : std::atoi(ids.c_str());
        log::info("[autorun] loaded '{}' (id {}, {} bytes of level string) from {}",
            lvl->m_levelName, lvl->m_levelID.value(), ls.size(), path);
        return lvl;
    }

    GJGameLevel* resolveLevel(std::string const& spec) {
        auto glm = GameLevelManager::sharedState();
        // A Windows path has its own colon, so this one cannot go through the generic split.
        if (spec.rfind("gmd:", 0) == 0) return levelFromGmd(spec.substr(4));
        auto colon = spec.find(':');
        std::string kind = colon == std::string::npos ? "main" : spec.substr(0, colon);
        std::string arg = colon == std::string::npos ? spec : spec.substr(colon + 1);
        if (kind == "main") {
            int id = std::atoi(arg.c_str());
            return glm->getMainLevel(id, false);
        }
        if (kind == "saved" || kind == "online") {
            int id = std::atoi(arg.c_str());
            if (auto lvl = glm->getSavedLevel(id)) return lvl;
            if (kind == "online" && glm->hasDownloadedLevel(id)) {
                if (auto dict = glm->m_downloadedLevels) {
                    if (auto lvl = static_cast<GJGameLevel*>(dict->objectForKey(std::to_string(id)))) return lvl;
                }
            }
            return nullptr;
        }
        if (kind == "local") {
            return glm->getLocalLevelByName(arg);
        }
        return nullptr;
    }

    bool isOnlineSpec(std::string const& spec) { return spec.rfind("online:", 0) == 0; }
    int specId(std::string const& spec) { auto c = spec.find(':'); return c == std::string::npos ? 0 : std::atoi(spec.c_str() + c + 1); }

    struct Downloader : public LevelDownloadDelegate {
        void levelDownloadFinished(GJGameLevel* level) override {
            log::info("[autorun] downloaded {}", levelDisplayName(level));
            GameLevelManager::sharedState()->m_levelDownloadDelegate = nullptr;
            startWithLevel(level);
        }
        void levelDownloadFailed(int response) override {
            GameLevelManager::sharedState()->m_levelDownloadDelegate = nullptr;
            log::error("[autorun] download failed ({})", response);
        }
    };
    Downloader s_downloader;

    void finish(std::string const& summary) {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - s_startedAt;
        log::info("[autorun] result level={} mode={} elapsed={:.1f}s {}", s_cfg.level, s_cfg.mode, elapsed, summary);
        s_phase = Phase::Done;
        if (s_cfg.exitWhenDone) {
            log::info("[autorun] exiting");
            Loader::get()->queueInMainThread([] { geode::utils::game::exit(false); });
        }
    }

    std::string summarizeRuns(std::vector<RunResult> const& runs) {
        std::string s = fmt::format("runs={}", runs.size());
        bool deterministic = true;
        for (size_t i = 0; i < runs.size(); i++) {
            auto const& r = runs[i];
            s += fmt::format(" | run{}: {} tick={} x={:.1f} pct={:.2f} hash={:016x}", i + 1,
                r.completed ? "complete" : r.died ? fmt::format("died(obj {})", r.deathObjectID) : "ended",
                r.endTick, r.furthestX, r.furthestPercent, r.trajectoryHash);
            if (i > 0 && (r.endTick != runs[0].endTick || r.trajectoryHash != runs[0].trajectoryHash
                          || r.completed != runs[0].completed || r.died != runs[0].died)) deterministic = false;
        }
        if (runs.size() > 1) s += deterministic ? " | deterministic" : " | nondeterministic";
        if (!runs.empty()) s += runs.back().completed ? " | complete" : " | incomplete";
        return s;
    }
}

bool enabled() { return s_phase != Phase::Disabled; }

void init() {
    auto mod = Mod::get();
    auto level = mod->getLaunchArgument("autorun");
    if (!level) return;
    s_cfg.level = *level;
    if (auto v = mod->getLaunchArgument("mode")) s_cfg.mode = *v;
    if (auto v = mod->getLaunchArgument("runs")) s_cfg.runs = std::max(1, std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("interval")) s_cfg.interval = std::max(1, std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("snap-tick")) s_cfg.snapTick = std::max(1, std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("diff-x")) s_cfg.diffX = (float) std::atof(v->c_str());
    if (auto v = mod->getLaunchArgument("run-ticks")) s_cfg.runTicks = std::max(1, std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("diff-tick")) s_cfg.diffTick = std::atoi(v->c_str());
    if (auto v = mod->getLaunchArgument("churn")) s_cfg.churn = std::max(0, std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("trace-ticks")) s_cfg.traceTicks = std::max(0, std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("inputs")) s_cfg.inputsPath = *v;
    {
        float tlo = 0.f, thi = -1.f;
        if (auto v = mod->getLaunchArgument("trace-x-lo")) tlo = (float) std::atof(v->c_str());
        if (auto v = mod->getLaunchArgument("trace-x-hi")) thi = (float) std::atof(v->c_str());
        if (thi > tlo) engine::setTraceWindow(tlo, thi);
        float dlo = 0.f, dhi = -1.f;
        if (auto v = mod->getLaunchArgument("dump-x-lo")) dlo = (float) std::atof(v->c_str());
        if (auto v = mod->getLaunchArgument("dump-x-hi")) dhi = (float) std::atof(v->c_str());
        if (dhi > dlo) Corridor::setDumpWindow(dlo, dhi);
    }
    {
        int glo = -1, ghi = -1;
        float gx0 = 0.f, gx1 = 0.f;
        if (auto v = mod->getLaunchArgument("geo-tick-lo")) glo = std::atoi(v->c_str());
        if (auto v = mod->getLaunchArgument("geo-tick-hi")) ghi = std::atoi(v->c_str());
        if (auto v = mod->getLaunchArgument("geo-x-lo")) gx0 = (float) std::atof(v->c_str());
        if (auto v = mod->getLaunchArgument("geo-x-hi")) gx1 = (float) std::atof(v->c_str());
        if (glo >= 0 && ghi >= glo && gx1 > gx0) {
            auto path = (Mod::get()->getSaveDir() / "geo.txt").string();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            Corridor::setGeoDump(glo, ghi, gx0, gx1, path);
            log::info("[autorun] live geometry dump: ticks {}..{} x {:.0f}..{:.0f} -> {}", glo, ghi, gx0, gx1, path);
        }
    }
    if (auto v = mod->getLaunchArgument("exit")) s_cfg.exitWhenDone = *v != "0";
    if (auto v = mod->getLaunchArgument("delay")) s_cfg.delay = (float) std::atof(v->c_str());
    if (auto v = mod->getLaunchArgument("solve-timeout")) s_cfg.solveTimeout = std::atof(v->c_str());
    if (auto v = mod->getLaunchArgument("cache")) s_cfg.useCache = *v != "0";
    if (auto v = mod->getLaunchArgument("seconds")) s_cfg.seconds = std::atof(v->c_str());
    if (auto v = mod->getLaunchArgument("doom-penalty")) Solver::setDoomPenalty(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("lane-cap")) Solver::setLaneCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("funnel-depth")) Solver::setFunnelDepth(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("room-weight")) Solver::setRoomWeight((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("bucket-floor")) Solver::setBucketFloor((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("key-bonus")) Solver::setKeyBonus(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("item-pull")) Solver::setItemPull(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("item-reach")) Solver::setItemReach((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("item-pull-cap")) Solver::setItemPullCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("item-id")) Solver::setItemId(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ring-pull")) Solver::setRingPull(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ring-reach")) Solver::setRingReach((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ring-pull-cap")) Solver::setRingPullCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ring-late")) Solver::setRingLate(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("look-ahead")) Solver::setLookAheadSeconds(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-plan")) Solver::setBallPlan(*v != "0" ? 1 : 0);
    if (auto v = mod->getLaunchArgument("ball-survive")) Solver::setBallSurvive(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-vy")) Solver::setBallVy(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-ystart")) Solver::setBallYStart(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-vy-to-y")) Solver::setBallVyToY(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-ground-ratio")) Solver::setBallGroundRatio(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-blue-ratio")) Solver::setBallBlueRatio(std::atof(v->c_str()));
    {
        float plo = 0.f, phi = 0.f;
        if (auto v = mod->getLaunchArgument("ball-probe-lo")) plo = (float) std::atof(v->c_str());
        if (auto v = mod->getLaunchArgument("ball-probe-hi")) phi = (float) std::atof(v->c_str());
        if (phi > plo) Solver::setBallProbe(plo, phi);
    }
    if (auto v = mod->getLaunchArgument("ball-grav-ref")) Solver::setBallGravRef(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-trace")) Solver::setBallTrace(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-pull")) Solver::setBallPull(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-pull-cap")) Solver::setBallPullCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-ahead")) Solver::setBallAhead(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-steer")) Solver::setBallSteer(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-room-edge")) Solver::setBallRoomEdge(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-force")) Solver::setBallForce(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-calm")) Solver::setBallCalm(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-clear")) Solver::setBallClear(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-orb-eager")) Solver::setBallOrbEager(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-orb-skip")) Solver::setBallOrbSkip(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-arm")) Solver::setBallArm(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-ground-eager")) Solver::setBallGroundEager(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-ground-force")) Solver::setBallGroundForce(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-phase-reach")) Solver::setBallPhaseReach(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-cross-cap")) Solver::setBallCrossCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-exact")) Solver::setBallExact(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-exact-force")) Solver::setBallExactForce(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-exact-horizon")) Solver::setBallExactHorizon(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-exact-beam")) Solver::setBallExactBeam(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-exact-cap")) Solver::setBallExactCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-exact-trace")) Solver::setBallExactTrace(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-buffer")) Solver::setBallBuffer(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-cache")) Solver::setBallCache(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-orb-force")) Solver::setBallOrbForce(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("segment-scale-max")) Bot::setSegmentScaleMax(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("show-search")) Bot::setShowSearch(*v != "0" ? 1 : 0);
    if (auto v = mod->getLaunchArgument("ball-dymax")) Solver::setBallDyMax(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-inner")) Solver::setBallInner(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-accel")) Solver::setBallAccel(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("ball-look")) Solver::setBallLook(*v != "0" ? 1 : 0);
    if (auto v = mod->getLaunchArgument("portal-pull")) Solver::setPortalPull(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("portal-reach")) Solver::setPortalReach(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("portal-pull-cap")) Solver::setPortalPullCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("speed-bonus")) Solver::setSpeedBonus(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("refuse-fatal")) Solver::setRefuseFatal(*v != "0" ? 1 : 0);
    if (auto v = mod->getLaunchArgument("plan-windows")) Solver::setPlanWindows(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-ycells")) Solver::setPlanYCells(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-acc-tol")) Solver::setPlanAccTol((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-weight")) Solver::setPlanWeight(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-body-x")) Solver::setPlanBodyX((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-sky")) Solver::setPlanSky((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-pull")) Solver::setPlanPull(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-pull-cap")) Solver::setPlanPullCap(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("p2-plan")) Solver::setP2Plan(*v != "0");
    if (auto v = mod->getLaunchArgument("plan-inner")) Solver::setPlanInner((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-vtop")) Solver::setPlanVTop(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-vmax")) Solver::setPlanVMax(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-steer")) Solver::setPlanSteer(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("sky-margin")) Solver::setSkyMargin((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("void-margin")) Solver::setVoidMargin((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("heap-nodes")) Solver::setHeapNodes(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("sky-dynamic")) Solver::setSkyDynamic(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("item-base-reset")) engine::setItemBaseReset(std::atoi(v->c_str()) != 0);
    if (auto v = mod->getLaunchArgument("area-reset")) engine::setAreaReset(std::atoi(v->c_str()) != 0);
    if (auto v = mod->getLaunchArgument("start-player-apply")) engine::setStartPlayerApply(std::atoi(v->c_str()) != 0);
    if (auto v = mod->getLaunchArgument("driven-step")) Solver::setDrivenStep(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("moved-key")) Solver::setMovedKey((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("moved-quant")) Solver::setMovedQuant((float) std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("verify-cap")) Solver::setVerifyCap(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("pose-override")) Solver::setPoseOverride(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("mover-sweep")) Solver::setMoverSweep((float) std::atof(v->c_str()));
    {
        float radius = 0.f;
        if (auto v = mod->getLaunchArgument("guide-radius")) radius = (float) std::atof(v->c_str());
        if (auto v = mod->getLaunchArgument("guide")) Bot::get().setGuideFile(*v, radius);
    }
    if (auto v = mod->getLaunchArgument("resume")) { if (*v != "0") Bot::get().setResumeFile(*v); }
    if (auto v = mod->getLaunchArgument("segment-cap")) Bot::get().setSegmentCap(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("segment-ticks")) Bot::get().setSegmentTicks(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("plan-order")) Bot::get().setPlanOrder(*v != "0");
    if (auto v = mod->getLaunchArgument("seg-candidates")) Bot::get().setSegCandidates(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("seg-extra")) Bot::get().setSegExtra(std::atof(v->c_str()));
    if (auto v = mod->getLaunchArgument("seg-margin")) Bot::get().setSegMargin(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("cand-first")) Bot::get().setCandFirst(*v != "0");
    if (auto v = mod->getLaunchArgument("cand-y")) Bot::get().setCandY(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("lean")) Bot::get().setLeanOverride(*v != "0" ? 1 : 0);
    if (auto v = mod->getLaunchArgument("cand-x")) Bot::get().setCandX(*v != "0");
    if (auto v = mod->getLaunchArgument("heap-check")) engine::setHeapCheckEvery(std::atoi(v->c_str()));
    if (auto v = mod->getLaunchArgument("fast-actions")) engine::setFastActions(*v != "0");
    s_phase = Phase::WaitingForMenu;
    log::info("[autorun] level={} mode={} runs={} inputs='{}' exit={}", s_cfg.level, s_cfg.mode, s_cfg.runs, s_cfg.inputsPath, s_cfg.exitWhenDone);
}

void onMenuLayer() {
    if (s_phase != Phase::WaitingForMenu) return;
    s_phase = Phase::Delay;
    s_timer = s_cfg.delay;
    log::info("[autorun] starting in {:.1f}s", s_cfg.delay);
}

static void start() {
    s_startedAt = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto level = resolveLevel(s_cfg.level);
    bool hasString = level && !std::string(level->m_levelString).empty();
    if (!hasString && isOnlineSpec(s_cfg.level)) {
        int id = specId(s_cfg.level);
        log::info("[autorun] downloading level {}", id);
        auto glm = GameLevelManager::sharedState();
        glm->m_levelDownloadDelegate = &s_downloader;
        glm->downloadLevel(id, false, 0);
        s_phase = Phase::Running;
        return;
    }
    if (!level) { finish("error: level not found"); return; }
    startWithLevel(level);
}

static void startWithLevel(GJGameLevel* level) {
    if (!level) { finish("error: level not found"); return; }
    std::string lvlStr = level->m_levelString;
    log::info("[autorun] level {} string length {}", levelDisplayName(level), lvlStr.size());
    if (lvlStr.empty()) { finish("error: empty level string"); return; }

    auto& bot = Bot::get();
    auto const& mode = s_cfg.mode;
    InputList inputs;
    if (mode == "play-file" || mode == "verify-file") {
        if (s_cfg.inputsPath.empty()) { finish("error: no inputs path"); return; }
        auto pf = PathFile::load(std::filesystem::path(s_cfg.inputsPath));
        if (pf.isErr()) { finish(fmt::format("error loading inputs: {}", pf.unwrapErr())); return; }
        inputs = pf.unwrap().inputs;
    } else if (mode == "play-hardcoded" || mode == "verify-hardcoded" || mode == "snapshot-test" || mode == "speed-test" || mode == "restore-diff" || mode == "reset-drift") {
        inputs = hardcodedInputs();
        if (!s_cfg.inputsPath.empty()) {
            auto pf = PathFile::load(std::filesystem::path(s_cfg.inputsPath));
            if (pf.isOk()) inputs = pf.unwrap().inputs;
        }
    }

    bot.onFinished = [](Bot& b) {
        if (s_cfg.mode == "snapshot-test" || s_cfg.mode == "speed-test" || s_cfg.mode == "dump-objects" || s_cfg.mode == "restore-diff" || s_cfg.mode == "reset-drift") {
            finish(fmt::format("{} | {}", b.experimentPassed() ? "pass" : "fail", b.experimentSummary()));
        } else if (s_cfg.mode == "solve" || s_cfg.mode == "solve-play") {
            std::string s = summarizeRuns(b.runResults());
            finish(fmt::format("solve inputs={} {}", b.inputs().size(), s));
        } else {
            finish(summarizeRuns(b.runResults()));
        }
    };

    if (mode == "fps-test") {
        s_noclip = true;
        s_frames.clear();
        s_frames.reserve(20000);
        s_fpsElapsed = 0.0;
        s_fpsWarmup = 3.f;
        auto scene = PlayLayer::scene(level, false, false);
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
        s_phase = Phase::Running;
        return;
    }
    if (mode == "play-hardcoded" || mode == "play-file") bot.requestPlay(level, inputs);
    else if (mode == "verify-hardcoded" || mode == "verify-file") bot.requestVerify(level, inputs, s_cfg.runs);
    else if (mode == "solve" || mode == "solve-play") { bot.requestSolve(level, mode == "solve-play", s_cfg.useCache); if (s_cfg.solveTimeout > 0.0) bot.setSolveTimeLimit(s_cfg.solveTimeout); }
    else if (mode == "snapshot-test") bot.requestExperiment(level, makeSnapshotTest(inputs, s_cfg.interval));
    else if (mode == "speed-test") bot.requestExperiment(level, makeSpeedTest(inputs));
    else if (mode == "restore-diff") bot.requestExperiment(level, makeRestoreDiff(inputs, s_cfg.snapTick, s_cfg.diffX, s_cfg.runTicks, s_cfg.diffTick));
    else if (mode == "reset-drift") bot.requestExperiment(level, makeResetDrift(inputs, s_cfg.runs, s_cfg.diffX, s_cfg.runTicks, s_cfg.churn, s_cfg.traceTicks));
    else if (mode == "dump-objects") bot.requestExperiment(level, makeObjectDump(makeLevelKey(level)));
    else { finish(fmt::format("error: unknown mode '{}'", mode)); return; }

    auto scene = PlayLayer::scene(level, false, false);
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
    s_phase = Phase::Running;
}

bool noclip() { return s_noclip; }

static void finishFpsTest() {
    auto pl = PlayLayer::get();
    int objects = pl && pl->m_objects ? (int) pl->m_objects->count() : 0;
    int nodes = countNodes(CCDirector::sharedDirector()->getRunningScene());
    s_noclip = false;
    if (s_frames.empty()) { finish("fail | fps-test recorded no frames"); return; }
    auto sorted = s_frames;
    std::sort(sorted.begin(), sorted.end());
    double total = 0.0;
    for (float f : s_frames) total += f;
    double avg = total / (double) s_frames.size();
    auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, (size_t) (p * sorted.size()))]; };
    float pct99 = pct(0.99), pct999 = pct(0.999), worst = sorted.back();
    float progress = pl ? pl->getCurrentPercent() : 0.f;
    finish(fmt::format(
        "pass | fps-test frames={} watched={:.1f}s avg={:.2f}ms ({:.0f} fps) p99={:.2f}ms p99.9={:.2f}ms "
        "worst={:.2f}ms objects={} nodes={} reached={:.1f}%",
        s_frames.size(), s_fpsElapsed, avg * 1000.0, 1.0 / avg, pct99 * 1000.f, pct999 * 1000.f,
        worst * 1000.f, objects, nodes, progress));
}

void tick(float dt) {
    switch (s_phase) {
        case Phase::Delay:
            s_timer -= dt;
            if (s_timer <= 0.f) { s_phase = Phase::Starting; start(); }
            break;
        case Phase::Running:
            if (s_noclip) {
                if (s_fpsWarmup > 0.f) { s_fpsWarmup -= dt; break; }
                if (!PlayLayer::get()) {
                    if (!s_frames.empty()) finishFpsTest();
                    break;
                }
                s_frames.push_back(dt);
                s_fpsElapsed += dt;
                if (s_fpsElapsed >= s_cfg.seconds) finishFpsTest();
            }
            break;
        default: break;
    }
}

}
