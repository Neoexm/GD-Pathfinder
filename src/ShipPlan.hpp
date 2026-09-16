#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace gdpf {

class ShipPlan {
public:
    enum class Mode : uint8_t { Ship, Wave, Ball, Ufo };

    struct Params {
        Mode mode = Mode::Ship;
        double accHold = 0.35;
        double accRelease = -0.35;
        double scale = 0.2165;
        double vMax = 1e9;
        float perTick = 1.3f;
        float dir = 1.f;
        float halfHeight = 15.f;
        float innerHalf = 4.5f;
        float groundY = 90.f;
        double slope = 1.1;
        double flap = 8.0;
        int tick = 0;
        int slot = 0;
        bool dynamic = false;
    };

    static int vCells(Mode m) { return m == Mode::Wave ? 2 : kVyCellsMax; }
    using GapQuery = std::function<void(float x0, float x1, float predictTicks, bool hazard, std::vector<std::pair<float, float>>& blocked, bool& content, float& top)>;

    static constexpr int kWindowsMax = 96;
    static int s_windows;
    static float s_accTol;
    static float s_bodyX;
    static float s_skyMargin;
    static constexpr float kYStep = 4.f;
    static constexpr float kVyStep = 0.7f;
    static int s_yCells;
    static constexpr int kYCellsMax = 512;
    static constexpr int kVyCellsMax = 48;
    static constexpr int kMargin = 6;
    static constexpr int kDepthLayers = 3;

    static constexpr int kStaleTicks = 30;

    struct Plan {
        Mode mode = Mode::Ship;
        int builtTick = 0;
        bool dynamic = false;
        int vCells = kVyCellsMax;
        int cells = s_yCells * kVyCellsMax;
        int window = 0;
        int windows = 24;
        float step = 20.f;
        int ticks = 15;
        float yBase = 0.f;
        double accHold = 0.f, accRelease = 0.f, scale = 0.f;
        int reach = 0;
        std::vector<uint64_t> safe;
        std::vector<uint8_t> margin;
        std::vector<uint8_t> depth;
        std::vector<uint8_t> yDepth;
        bool cell(int layer, int yi, int vj) const;
    };

    struct Advice { bool ok = false; int curDepth = 0; int bestDepth = 0; float bestY = 0.f; };
    Advice advise(GapQuery const& gaps, Params const& p, float x, float y, double vy);

    int prefer(GapQuery const& gaps, Params const& p, float x, float y, double vy, int K,
               bool& doomed, int& holdScore, int& releaseScore);

    void clear() { m_cache.clear(); }
    size_t cacheSize() const { return m_cache.size(); }
    uint64_t plansBuilt = 0;
    uint64_t offAxis = 0;
    uint64_t offGrid = 0;
    uint64_t reMode = 0, reStale = 0, reStep = 0, reY = 0, reAcc = 0;
    double planSeconds = 0.0;

private:
    Plan const* planFor(GapQuery const& gaps, Params const& p, float x, float y);
    void build(GapQuery const& gaps, Params const& p, int window, float yCentre, Plan& out);
    std::map<int, Plan> m_cache;
    static constexpr size_t kCacheCap = 400;
};

}
