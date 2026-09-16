#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gdpf {
class BallPlan {
public:
    using GapQuery = std::function<void(float x0, float x1, float predictTicks, bool hazard,
                                        std::vector<std::pair<float, float>>& blocked,
                                        bool& content, float& top)>;

    enum Orb { OrbBlue = 0, OrbYellow, OrbPink, OrbRed, OrbGreen, OrbDrop, OrbCount };
    using OrbQuery = std::function<bool(float x0, float x1, float* lo, float* hi)>;

    struct Params {
        float perTick = 1.9f;
        float dir = 1.f;
        float halfHeight = 9.f;   // body half-height, for surface contact
        float innerHalf = 5.f;    // hazard half-height
        float halfWidth = 9.f;    // an obstacle ahead in x is a collision once the BODY
        float innerHalfW = 5.f;   // reaches it, not once the centre does
        double accel = 0.029115;  // ballistic dy per tick^2 (0.1294 vy x 0.225)
        double dyMax = 3.375;     // the game clamps vy to +-15, and y gains 0.225 per unit
        double jumpDy = 0.6356;   // a surface press: measured 2.825 vy, and it flips gravity
        // Measured for a mini ball (m_yStart 11.23): blue orb 2.645 vy. The rest are
        // ringJump's multipliers anchored to the measured blue. Solver overrides all of it.
        double orbDy[OrbCount] = {0.5951, 0.7439, 0.5728, 0.9968, 0.7439, 3.375};
        bool flipped = false;     // current gravity, true = pulled up
        int tick = 0;
        std::function<float(float x, float fallback)> perTickAt;
        OrbQuery orbAt;
    };

    static constexpr int kYStep = 1;
    static constexpr int kYCells = 80;
    static constexpr int kDyHalf = 152;
    static constexpr int kDyCells = kDyHalf * 2 + 1;
    static constexpr int kStride = 4;                    // ticks per transition
    static constexpr int kHorizonStrides = 110;
    static constexpr int kBlockStrides = 22;             // 88 ticks of layers to spare
    static constexpr double kBlockUnits = 64.0;          // x served by one plan
    static constexpr double kYBucket = 32.0;             // y served by one plan
    static constexpr int kRoomCap = 16;                  // reachable clearance cap
    static constexpr int kStrides = kHorizonStrides + kBlockStrides;
    static constexpr int kTickLayers = kStrides * kStride;
    static constexpr int kHorizonTicks = kHorizonStrides * kStride;

    struct Verdict {
        bool ok = false;
        int depth = 0;        // ticks survivable from here, capped at kHorizonTicks
        int pressDepth = 0;   // ... if the next tick presses
        int idleDepth = 0;    // ... if it does not
        int pressRoom = 0;    // widest-path bottleneck clearance
        int idleRoom = 0;
    };

    struct Advice {
        bool ok = false;
        float bestY = 0.f;
        int lo = 0;
        int hi = 0;
    };

    Verdict query(GapQuery const& gaps, Params const& p, float x, float y, double dy);
    Advice advise(GapQuery const& gaps, Params const& p, float x, float y, int aheadTicks);
    void clear() { m_cache.clear(); }

    int trace = 0;
    std::vector<std::string> traces;

    uint64_t plansBuilt = 0;
    uint64_t offGrid = 0;
    uint64_t queries = 0;
    double planSeconds = 0.0;

private:
    struct Plan {
        bool ok = false;
        int block = 0;
        int builtTick = 0;
        float perTick = 1.9f;
        float yBase = 0.f;
        double accel = 0.029115;
        double dyStep = 0.0145;
        double dyLim = 3.48;
        double jumpDy = 0.64;
        double orbDy[OrbCount] = {0};
        std::vector<float> layerU;     // u (= dir*x) at the start of each tick layer
        std::vector<uint8_t> depth;    // strides survivable: [((stride*2+g)*kDyCells+dyj)*kYCells+yi]
        std::vector<uint8_t> value;    // widest-path bottleneck clearance, same indexing
        std::vector<uint8_t> freeHaz;  // per tick layer: [tick*kYCells + yi]
        std::vector<uint8_t> room;     // per tick layer: cells to the nearest blocked one
        std::vector<float> solLo;      // exact free interval, so contact is flush
        std::vector<float> solHi;
        std::vector<float> orbLo;      // [tick*OrbCount + kind]
        std::vector<float> orbHi;
        bool anyOrb = false;
    };
    static bool stepOne(Plan const& pl, double& y, double& dy, int& g, int layer, bool press,
                        int* minRoom);
    static int orbAtLayer(Plan const& pl, int layer, double y);
    static int tickOffOf(Plan const& pl, Params const& p, float x);
    Plan const* planFor(GapQuery const&, Params const&, float x, float y);
    void build(GapQuery const&, Params const&, int block, double u0, float yCentre, Plan& out);
    std::map<int, Plan> m_cache;

public:
    size_t cacheCap = 16;
};

}
