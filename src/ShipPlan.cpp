#include "ShipPlan.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace gdpf {

int ShipPlan::s_yCells = 160;
int ShipPlan::s_windows = 48;
float ShipPlan::s_accTol = 1.0f;
float ShipPlan::s_bodyX = 1.f;
float ShipPlan::s_skyMargin = 0.f;

namespace {
    double nowSeconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    using Mode = ShipPlan::Mode;
    constexpr float kVyMin = -0.5f * ShipPlan::kVyStep * ShipPlan::kVyCellsMax;

    inline int cellIndex(int yi, int vj) { return vj * ShipPlan::s_yCells + yi; }
    inline int cellCount(int vCells) { return ShipPlan::s_yCells * vCells; }
    inline int wordCount(int vCells) { return (cellCount(vCells) + 63) / 64; }

    inline int yIndex(float y, float yBase) {
        int yi = (int) std::floor((y - yBase) / ShipPlan::kYStep);
        return yi >= 0 && yi < ShipPlan::s_yCells ? yi : -1;
    }

    inline int vIndexOf(Mode m, double vy, double slope) {
        if (m == Mode::Wave) return vy >= 0.0 ? 1 : 0;
        int vj = (int) std::floor((vy - kVyMin) / ShipPlan::kVyStep);
        return vj >= 0 && vj < ShipPlan::kVyCellsMax ? vj : -1;
    }
    inline double vValueOf(Mode m, int vj, double slope) {
        if (m == Mode::Wave) return vj == 1 ? slope : -slope;
        return kVyMin + (vj + 0.5) * ShipPlan::kVyStep;
    }

    inline void stepOne(Mode m, double& y, double& vy, bool hold, bool edge, ShipPlan::Params const& p) {
        if (m == Mode::Wave) {
            vy = hold ? p.slope : -p.slope;
            y += vy * p.perTick;
            return;
        }
        if (m == Mode::Ufo) {
            if (edge) vy = p.flap;
            y += vy * p.scale;
            vy += p.accRelease;
            if (vy > p.vMax) vy = p.vMax;
            if (vy < -p.vMax) vy = -p.vMax;
            return;
        }
        y += vy * p.scale;
        vy += hold ? p.accHold : p.accRelease;
        if (vy > p.vMax) vy = p.vMax;
        if (vy < -p.vMax) vy = -p.vMax;
    }

    inline int patternCount(Mode m, int ticks) {
        if (m == Mode::Wave) return 6;
        if (m == Mode::Ball || m == Mode::Ufo) return 6;
        return 8;
    }
    inline bool heldAt(Mode m, int pat, int t, int ticks) {
        int quarter = std::max(1, ticks / 4);
        int half = ticks / 2;
        if (m == Mode::Ball || m == Mode::Ufo) {
            if (pat == 0) return false;
            if (pat == 1) return true;
            int sw = pat == 2 ? quarter : pat == 3 ? half : pat == 4 ? (ticks - quarter) : (ticks - 1);
            return t >= sw;
        }
        if (pat == 0) return true;
        if (pat == 1) return false;
        int sw = (pat == 2 || pat == 5) ? quarter : (pat == 3 || pat == 6) ? half : ticks - quarter;
        return pat <= 4 ? (t < sw) : (t >= sw);
    }
}

bool ShipPlan::Plan::cell(int layer, int yi, int vj) const {
    if (layer < 0 || layer > windows || yi < 0 || vj < 0 || yi >= s_yCells || vj >= vCells) return false;
    size_t bit = (size_t) layer * (size_t) cells + (size_t) cellIndex(yi, vj);
    return (safe[bit >> 6] >> (bit & 63)) & 1u;
}

void ShipPlan::build(GapQuery const& gaps, Params const& p, int window, float yCentre, Plan& out) {
    double t0 = nowSeconds();
    out.mode = p.mode;
    out.builtTick = p.tick;
    out.dynamic = p.dynamic;
    out.vCells = vCells(p.mode);
    out.cells = cellCount(out.vCells);
    int const kCells = out.cells;
    int const kVyCellsRT = out.vCells;
    int const kWords = wordCount(out.vCells);
    out.window = window;
    out.windows = std::clamp(s_windows, 8, kWindowsMax);
    out.step = std::max(20.f, p.perTick * 15.f);
    out.ticks = std::max(4, (int) std::lround(out.step / std::max(0.05f, p.perTick)));
    out.yBase = std::floor((yCentre - 0.5f * kYStep * s_yCells) / kYStep) * kYStep;
    out.accHold = p.accHold;
    out.accRelease = p.accRelease;
    out.scale = p.scale;
    int const kW = out.windows;
    out.safe.assign((size_t) (kW + 1) * kWords, 0);
    out.margin.assign((size_t) kDepthLayers * kCells, 0);
    out.reach = 0;

    std::vector<uint8_t> free((size_t) (kW + 1) * s_yCells, 0);
    std::vector<std::pair<float, float>> blocked;
    bool anyContent = false;
    for (int k = 0; k <= kW; k++) {
        float w = (float) (window + k);
        float a = p.dir > 0.f ? w * out.step : -(w + 1.f) * out.step;
        float b = a + out.step;
        auto* row = &free[(size_t) k * s_yCells];
        for (int yi = 0; yi < s_yCells; yi++) row[yi] = 1;
        bool content = false;
        float top = -1e30f;
        float hw = s_bodyX * p.halfHeight;
        for (int kind = 0; kind < 2; kind++) {
            bool hazard = kind == 1;
            bool c = false;
            float t2 = 0.f;
            gaps(a - hw, b + hw, (float) (k * out.ticks), hazard, blocked, c, t2);
            if (c) top = std::max(top, t2);
            content = content || c;
            float hh = std::max(0.f, (hazard ? p.innerHalf : p.halfHeight) - 0.5f * kYStep);
            for (auto const& iv : blocked) {
                int y0 = std::max(0, (int) std::floor((iv.first - hh - out.yBase) / kYStep - 0.5f) + 1);
                int y1 = std::min(s_yCells - 1, (int) std::ceil((iv.second + hh - out.yBase) / kYStep - 0.5f) - 1);
                for (int yi = y0; yi <= y1; yi++) row[yi] = 0;
            }
        }
        if (content) {
            anyContent = true;
            int yg = (int) std::floor((p.groundY + p.halfHeight - 0.5f * kYStep - out.yBase) / kYStep - 0.5f);
            for (int yi = 0; yi < std::min(s_yCells, yg); yi++) row[yi] = 0;
            if (s_skyMargin > 0.f && top > -1e29f) {
                int yc = (int) std::ceil((top + s_skyMargin - p.halfHeight - out.yBase) / kYStep - 0.5f);
                for (int yi = std::max(0, yc); yi < s_yCells; yi++) row[yi] = 0;
            }
        }
        if (k == 0) {
            int yc = (int) std::floor((yCentre - out.yBase) / kYStep);
            for (int d = -1; d <= 1; d++) if (yc + d >= 0 && yc + d < s_yCells) row[yc + d] = 1;
        }
    }
    if (!anyContent) return;

    auto setSafe = [&](int layer, int yi, int vj) {
        size_t bit = (size_t) layer * kCells + (size_t) cellIndex(yi, vj);
        out.safe[bit >> 6] |= 1ull << (bit & 63);
    };
    out.depth.assign((size_t) kDepthLayers * kCells, 0);
    out.yDepth.assign((size_t) s_yCells, 0);
    std::vector<uint8_t> depthNext((size_t) kCells, 0), depthNow((size_t) kCells, 0);
    for (int vj = 0; vj < kVyCellsRT; vj++)
        for (int yi = 0; yi < s_yCells; yi++)
            if (free[(size_t) kW * s_yCells + yi]) { setSafe(kW, yi, vj); depthNext[cellIndex(yi, vj)] = 0; }

    int half = out.ticks / 2;
    for (int k = kW - 1; k >= 0; k--) {
        auto const* rowNow = &free[(size_t) k * s_yCells];
        auto const* rowNext = &free[(size_t) (k + 1) * s_yCells];
        bool any = false;
        int const pats = patternCount(p.mode, out.ticks);
        for (int vj = 0; vj < kVyCellsRT; vj++) {
            double vy0 = vValueOf(p.mode, vj, p.slope);
            for (int yi = 0; yi < s_yCells; yi++) {
                if (!rowNow[yi]) continue;
                double y0 = out.yBase + (yi + 0.5) * kYStep;
                int best = 0;
                for (int pat = 0; pat < pats; pat++) {
                    double y = y0, vy = vy0;
                    bool alive = true;
                    bool prevHold = false;
                    for (int t = 0; t < out.ticks; t++) {
                        bool hold = heldAt(p.mode, pat, t, out.ticks);
                        stepOne(p.mode, y, vy, hold, hold && !prevHold, p);
                        prevHold = hold;
                        int yk = (int) std::floor((y - out.yBase) / kYStep);
                        if (yk < 0 || yk >= s_yCells) { alive = false; break; }
                        if (!(t < half ? rowNow[yk] : (rowNow[yk] && rowNext[yk]))) { alive = false; break; }
                    }
                    if (!alive) continue;
                    int yk = (int) std::floor((y - out.yBase) / kYStep);
                    int vk = vIndexOf(p.mode, vy, p.slope);
                    if (yk < 0 || vk < 0) continue;
                    for (int dv = -1; dv <= 1; dv++)
                        for (int dy = -1; dy <= 1; dy++) {
                            int y2 = yk + dy, v2 = vk + dv;
                            if (y2 < 0 || y2 >= s_yCells || v2 < 0 || v2 >= kVyCellsRT) continue;
                            if (!free[(size_t) (k + 1) * s_yCells + y2]) continue;
                            best = std::max(best, 1 + (int) depthNext[cellIndex(y2, v2)]);
                        }
                }
                if (best > 0) {
                    depthNow[cellIndex(yi, vj)] = (uint8_t) std::min(best, 255);
                    if (best >= kW - k) { setSafe(k, yi, vj); any = true; }
                }
            }
        }
        if (any) out.reach = std::max(out.reach, kW - k);
        if (k < kDepthLayers) std::copy(depthNow.begin(), depthNow.end(), out.depth.begin() + (size_t) k * kCells);
        depthNext.swap(depthNow);
        std::fill(depthNow.begin(), depthNow.end(), (uint8_t) 0);
    }

    for (int yi = 0; yi < s_yCells; yi++) {
        int best = 0;
        for (int vj = 0; vj < kVyCellsRT; vj++) best = std::max(best, (int) out.depth[cellIndex(yi, vj)]);
        out.yDepth[yi] = (uint8_t) std::min(best, 255);
    }

    for (int layer = 0; layer < kDepthLayers; layer++) {
        auto* m = &out.margin[(size_t) layer * kCells];
        for (int vj = 0; vj < kVyCellsRT; vj++)
            for (int yi = 0; yi < s_yCells; yi++)
                m[cellIndex(yi, vj)] = out.cell(layer, yi, vj) ? (uint8_t) kMargin : 0;
        for (int d = 1; d <= kMargin; d++) {
            for (int vj = 0; vj < kVyCellsRT; vj++) {
                for (int yi = 0; yi < s_yCells; yi++) {
                    auto& c = m[cellIndex(yi, vj)];
                    if (c < d) continue;
                    bool ok = true;
                    for (int dv = -1; dv <= 1 && ok; dv++)
                        for (int dy = -1; dy <= 1 && ok; dy++) {
                            int y2 = yi + dy, v2 = vj + dv;
                            if (y2 < 0 || y2 >= s_yCells || v2 < 0 || v2 >= kVyCellsRT) { ok = false; break; }
                            if (m[cellIndex(y2, v2)] < d - 1) ok = false;
                        }
                    if (!ok) c = (uint8_t) (d - 1);
                }
            }
        }
    }
    planSeconds += nowSeconds() - t0;
}

ShipPlan::Plan const* ShipPlan::planFor(GapQuery const& gaps, Params const& p, float x, float y) {
    float step = std::max(20.f, p.perTick * 15.f);
    int window = (int) std::floor(p.dir * x / step);
    int key = (window * 4 + (p.accHold >= 0.0 ? 1 : 0) + (p.dir >= 0.f ? 2 : 0)) * 2 + (p.slot != 0 ? 1 : 0);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        auto const& pl = it->second;
        if (pl.mode != p.mode || pl.windows != std::clamp(s_windows, 8, kWindowsMax)) { reMode++; m_cache.erase(it); it = m_cache.end(); }
    }
    if (it != m_cache.end()) {
        auto const& pl = it->second;
        double tol = (double) s_accTol;
        auto close = [tol](double a, double b) { return std::fabs(a - b) <= tol * std::max(std::fabs(a), std::fabs(b)); };
        bool stale = (pl.dynamic || p.dynamic) && std::abs(p.tick - pl.builtTick) > kStaleTicks;
        bool yok = y >= pl.yBase + 2.f * kYStep && y < pl.yBase + (s_yCells - 2) * kYStep;
        bool stepok = std::fabs(pl.step - step) < 0.01f;
        bool accok = close(pl.accHold, p.accHold) && close(pl.accRelease, p.accRelease) && close(pl.scale, p.scale);
        if (!stale && stepok && yok && accok) return &pl;
        if (stale) reStale++; else if (!stepok) reStep++; else if (!yok) reY++; else reAcc++;
        m_cache.erase(it);
    }
    if (m_cache.size() >= kCacheCap) {
        auto farthest = m_cache.begin();
        for (auto j = m_cache.begin(); j != m_cache.end(); ++j)
            if (std::abs(j->first - key) > std::abs(farthest->first - key)) farthest = j;
        m_cache.erase(farthest);
    }
    Plan plan;
    build(gaps, p, window, y, plan);
    plansBuilt++;
    return &(m_cache[key] = std::move(plan));
}

ShipPlan::Advice ShipPlan::advise(GapQuery const& gaps, Params const& p, float x, float y, double vy) {
    Advice out;
    auto const* plan = planFor(gaps, p, x, y);
    if (!plan || plan->yDepth.empty()) return out;
    int yi0 = yIndex(y, plan->yBase);
    if (yi0 < 0) return out;
    out.ok = true;
    out.curDepth = (int) plan->yDepth[yi0];
    int best = 0;
    for (int yi = 0; yi < s_yCells; yi++) best = std::max(best, (int) plan->yDepth[yi]);
    out.bestDepth = best;
    out.bestY = y;
    if (best <= out.curDepth) return out;
    int want = best - best / 16;
    int pick = -1;
    for (int d = 0; d < s_yCells; d++) {
        int a2 = yi0 - d, b2 = yi0 + d;
        if (a2 >= 0 && (int) plan->yDepth[a2] >= want) { pick = a2; break; }
        if (b2 < s_yCells && (int) plan->yDepth[b2] >= want) { pick = b2; break; }
    }
    if (pick >= 0) out.bestY = plan->yBase + (pick + 0.5f) * kYStep;
    return out;
}

int ShipPlan::prefer(GapQuery const& gaps, Params const& p, float x, float y, double vy, int K,
                     bool& doomed, int& holdScore, int& releaseScore) {
    doomed = false;
    holdScore = releaseScore = -1;
    auto const* plan = planFor(gaps, p, x, y);
    if (!plan || plan->depth.empty()) return 0;
    int yi0 = yIndex(y, plan->yBase), vj0 = vIndexOf(p.mode, vy, p.slope);
    if (yi0 < 0 || vj0 < 0) { if (vj0 < 0) offAxis++; else offGrid++; return 0; }

    auto successor = [&](bool hold, int& depthOut, int& marginOut) {
        double sy = y, svy = vy;
        for (int t = 0; t < K; t++) stepOne(p.mode, sy, svy, hold, hold && t == 0, p);
        float sx = x + p.dir * p.perTick * (float) K;
        int layer = (int) std::floor(p.dir * sx / plan->step) - plan->window;
        int yi = yIndex((float) sy, plan->yBase), vj = vIndexOf(p.mode, svy, p.slope);
        if (layer < 0 || layer > plan->windows || yi < 0 || vj < 0) { if (vj < 0) offAxis++; depthOut = marginOut = -1; return; }
        if (layer < kDepthLayers) {
            size_t c = (size_t) layer * (size_t) plan->cells + (size_t) cellIndex(yi, vj);
            depthOut = plan->depth[c];
            marginOut = plan->margin[c];
        } else {
            bool s = plan->cell(layer, yi, vj);
            depthOut = s ? plan->windows : 0;
            marginOut = s ? kMargin : 0;
        }
    };
    int hm = 0, rm = 0;
    successor(true, holdScore, hm);
    successor(false, releaseScore, rm);
    if (holdScore < 0 && releaseScore < 0) return 0;
    doomed = holdScore <= 0 && releaseScore <= 0;
    if (holdScore != releaseScore) return holdScore > releaseScore ? 1 : -1;
    if (hm != rm) return hm > rm ? 1 : -1;
    return 0;
}

}
