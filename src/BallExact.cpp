#include "BallExact.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace gdpf {
namespace ballexact {

namespace {

inline double r3(double v) {
    return v >= 0.0 ? std::floor(v * 1000.0 + 0.5) / 1000.0
                    : -(std::floor(-v * 1000.0 + 0.5) / 1000.0);
}

constexpr int kMaskBits = 48;

int s_trace = 0;

// Measured x per tick by speed rank. 3x is 1.949215, not the 1.95 the nominal 468 units a
// second gives, and 0.5x is 1.046843; both are fitted over 400 engine ticks.
double perTickForRank(int rank) {
    static constexpr double kPer[5] = {1.046843, 1.298250, 1.614250, 1.949215, 2.400000};
    return kPer[rank < 0 ? 0 : rank > 4 ? 4 : rank];
}

// m_yStart by speed rank. It is NOT size dependent but it IS speed dependent, and every
// impulse in the ball's frame is a fixed fraction of it. 10.62 at 0.5x and 11.23 at 3x are
// read straight out of the game; the rest are linear in x-per-tick between them.
double yStartForRank(int rank) {
    static constexpr double kYs[5] = {10.62, 10.79, 11.00, 11.23, 11.54};
    return kYs[rank < 0 ? 0 : rank > 4 ? 4 : rank];
}

struct Node {
    double   y, vy;
    uint8_t  g, buf, held;
    uint64_t mask;      // rings consumed, relative to base
    int      base;      // index of the first ring not yet behind us
    int      parent;
    uint8_t  act;
};

struct Key {
    int64_t  y, vy;
    uint64_t mask;
    int32_t  base;
    uint8_t  g, buf, held;
    bool operator==(Key const& o) const {
        return y == o.y && vy == o.vy && mask == o.mask && base == o.base
            && g == o.g && buf == o.buf && held == o.held;
    }
};

struct KeyHash {
    size_t operator()(Key const& k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        mix((uint64_t) k.y);
        mix((uint64_t) k.vy);
        mix(k.mask);
        mix((uint64_t) k.base);
        mix((uint64_t) (k.g | (k.buf << 1) | (k.held << 2)));
        return (size_t) h;
    }
};

// The free y interval for the player CENTRE at this x, containing y. Empty when the lane
// closes, which ends that line.
struct Lane { double lo, hi; bool ok; };

}   // namespace

void setTrace(int v) { s_trace = v; }

bool Plan::matches(int tick, double yy, double vvy, int gg, int hheld, int bbuf, double eps) const {
    if (!covers(tick)) return false;
    size_t i = (size_t) (tick - tick0);
    return std::abs((double) y[i] - yy) <= eps && std::abs((double) vy[i] - vvy) <= eps
        && g[i] == (uint8_t) gg && held[i] == (uint8_t) hheld && buf[i] == (uint8_t) bbuf;
}

Plan build(Corridor const& cor, Start const& st, Params const& prm,
           int horizon, int beam, double stopX) {
    Plan out;
    out.tick0 = st.tick;
    out.x0 = st.x;
    if (horizon <= 0 || beam <= 0) return out;

    // rings, sorted by the x they stop being reachable at, so `base` only ever advances
    struct Ring { double x0, x1, y0, y1; int kind; };
    std::vector<Ring> rings;
    rings.reserve(cor.orbs().size());
    for (auto const& o : cor.orbs()) {
        if (o.kind < 0 || o.kind >= 8) continue;
        if (prm.orbBase[o.kind] <= 0.0) continue;       // not a modelled gravity ring
        double ahead = prm.dir >= 0.0 ? (double) o.x1 - st.x : st.x - (double) o.x0;
        if (ahead < -80.0) continue;                    // already well behind
        rings.push_back({(double) o.x0, (double) o.x1, (double) o.y0, (double) o.y1, o.kind});
    }
    std::sort(rings.begin(), rings.end(), [&prm](Ring const& a, Ring const& b) {
        return prm.dir >= 0.0 ? a.x1 < b.x1 : a.x0 > b.x0;
    });
    int const nr = (int) rings.size();

    auto ringHit = [&](double x, double y, int base, uint64_t mask) -> int {
        double a = x - prm.hw, b = x + prm.hw, lo = y - prm.hh, hi = y + prm.hh;
        for (int i = base; i < nr && i - base < kMaskBits; i++) {
            Ring const& r = rings[(size_t) i];
            if (r.x1 < a || r.x0 > b) continue;
            if (r.y1 < lo || r.y0 > hi) continue;
            if (mask & (1ull << (i - base))) continue;
            return i;
        }
        return -1;
    };

    // one lane query per tick, shared by every state on that tick
    std::vector<std::pair<float, float>> blocked;
    auto laneAt = [&](double y) -> Lane {
        double lo = -1e30, hi = 1e30;
        for (auto const& b : blocked) {
            if ((double) b.second <= y + 1e-6 && (double) b.second > lo) lo = b.second;
            if ((double) b.first  >= y - 1e-6 && (double) b.first  < hi) hi = b.first;
        }
        if (lo < -1e29 || hi > 1e29) return {0.0, 0.0, false};
        double flo = lo + prm.hh, fhi = hi - prm.hh;
        if (fhi < flo - 1e-6) return {0.0, 0.0, false};
        return {flo, fhi, true};
    };

    std::vector<std::vector<Node>> layers;
    layers.reserve((size_t) horizon + 1);
    int base0 = 0;
    {
        double a = st.x - prm.hw;
        while (base0 < nr && (prm.dir >= 0.0 ? rings[(size_t) base0].x1 < a
                                             : rings[(size_t) base0].x0 > st.x + prm.hw))
            base0++;
    }
    layers.push_back({Node{st.y, st.vy, (uint8_t) st.g, (uint8_t) st.buf, (uint8_t) st.held,
                           0ull, base0, -1, (uint8_t) st.held}});

    // speed portals ahead, in the order they will be crossed. A portal changes both the x
    // per tick and m_yStart, so every impulse after it is different - planning through one
    // with the old numbers is what made the first version of this die 12 units short of the
    // spike it had already cleared.
    struct Sp { double x; int rank; };
    std::vector<Sp> sps;
    for (auto const& sp : cor.speedPortals()) {
        double ahead = prm.dir >= 0.0 ? (double) sp.x - st.x : st.x - (double) sp.x;
        if (ahead <= 0.0) continue;
        sps.push_back({(double) sp.x, sp.id});
    }
    std::sort(sps.begin(), sps.end(), [&prm](Sp const& a, Sp const& b) {
        return prm.dir >= 0.0 ? a.x < b.x : a.x > b.x;
    });
    size_t spi = 0;
    double per = prm.perTick;
    double ys = prm.yStart;

    double x = st.x;
    std::unordered_map<Key, int, KeyHash> seen;
    int t = 0;
    for (; t < horizon; t++) {
        if (stopX > 0.0 && (prm.dir >= 0.0 ? x >= stopX : x <= stopX)) break;
        while (spi < sps.size()
               && (prm.dir >= 0.0 ? x >= sps[spi].x - prm.portalLead
                                  : x <= sps[spi].x + prm.portalLead)) {
            per = perTickForRank(sps[spi].rank);
            ys = yStartForRank(sps[spi].rank);
            spi++;
        }
        double nx = x + prm.dir * per;
        blocked.clear();
        cor.solidRectsAt((float) (x - prm.hw), (float) (x + prm.hw), blocked);
        std::vector<Node> next;
        next.reserve((size_t) beam * 2);
        seen.clear();
        auto const& curl = layers.back();
        for (int pi = 0; pi < (int) curl.size(); pi++) {
            Node const& n = curl[(size_t) pi];
            Lane lane = laneAt(n.y);
            if (!lane.ok) continue;
            for (int b = 0; b <= 1; b++) {
                int edge = (b && !n.held) ? 1 : 0;
                // a released button cannot resolve a live buffer either: the release
                // clears it before the jump check runs.
                int live = (edge || (n.buf && b)) ? 1 : 0;
                int g = n.g;
                int ng = g;
                // The jump buffer only survives while the button is DOWN - releaseButton
                // clears it. So an arming press has to be HELD until it resolves, and a
                // held button fires at the first contact of anything, ring or surface.
                // That is a real constraint on the plan, not a detail: a one-tick arming
                // tap sets nothing at all.
                uint8_t nbuf = (uint8_t) ((b && (n.buf || edge)) ? 1 : 0);
                uint64_t mask = n.mask;
                double grav = g ? prm.grav : -prm.grav;
                double nvy = n.vy;
                bool resolved = false;
                if (live) {
                    int ri = ringHit(x, n.y, n.base, mask);
                    bool on = g == 0 ? n.y <= lane.lo + 1e-9 : n.y >= lane.hi - 1e-9;
                    if (ri >= 0 || on) {
                        ng = g ^ 1;
                        double bs = ri >= 0 ? prm.orbBase[rings[(size_t) ri].kind] * ys
                                            : prm.surfBase * ys;
                        double v = ng ? bs : -bs;
                        nvy = edge ? r3(v + (ng ? prm.grav : -prm.grav)) : v;
                        if (ri >= 0) mask |= 1ull << (ri - n.base);
                        nbuf = 0;
                        resolved = true;
                    }
                }
                if (!resolved) nvy = r3(n.vy + grav);
                nvy = std::clamp(nvy, -prm.vyMax, prm.vyMax);
                double ny = n.y + nvy * prm.vyToY;
                if (ny <= lane.lo) { ny = lane.lo; nvy = 0.0; }
                if (ny >= lane.hi) { ny = lane.hi; nvy = 0.0; }
                if (live && !resolved) {
                    int ri = ringHit(nx, ny, n.base, mask);
                    if (ri >= 0) {
                        ng = g ^ 1;
                        double bs = prm.orbBase[rings[(size_t) ri].kind] * ys;
                        nvy = ng ? bs : -bs;
                        mask |= 1ull << (ri - n.base);
                        nbuf = 0;
                    }
                    // a landing never fires the buffer: see the header
                }
                if (cor.boxHitsHazardExact((float) (nx - prm.hw), (float) (nx + prm.hw),
                                           (float) (ny - prm.hh), (float) (ny + prm.hh)))
                    continue;
                int nbase = n.base;
                double aa = nx - prm.hw, bb = nx + prm.hw;
                while (nbase < nr && (prm.dir >= 0.0 ? rings[(size_t) nbase].x1 < aa
                                                     : rings[(size_t) nbase].x0 > bb)) {
                    mask >>= 1;
                    nbase++;
                }
                Key k{(int64_t) std::llround(ny * 1e6), (int64_t) std::llround(nvy * 1e4),
                      mask, nbase, (uint8_t) ng, nbuf, (uint8_t) b};
                if (seen.find(k) != seen.end()) continue;
                seen.emplace(k, 1);
                next.push_back(Node{ny, nvy, (uint8_t) ng, nbuf, (uint8_t) b, mask, nbase,
                                    pi, (uint8_t) b});
            }
        }
        if (next.empty()) break;
        if ((int) next.size() > beam) {
            // keep the states furthest from BOTH surfaces: in a spike corridor the middle of
            // the lane is the only place that is never in a hazard's band, so this is what
            // keeps the frontier alive rather than a distance-to-goal score.
            Lane lane = laneAt(next.front().y);
            double mid = lane.ok ? 0.5 * (lane.lo + lane.hi) : next.front().y;
            std::nth_element(next.begin(), next.begin() + beam, next.end(),
                [mid](Node const& a, Node const& b) {
                    return std::abs(a.y - mid) < std::abs(b.y - mid);
                });
            next.resize((size_t) beam);
        }
        if (s_trace) {
            double ylo = 1e30, yhi = -1e30;
            for (auto const& nd : next) { ylo = std::min(ylo, nd.y); yhi = std::max(yhi, nd.y); }
            if ((t % 25) == 0 || (t > 390 && t < 420))
                geode::log::info("[btrace] t={} x={:.2f} n={} y {:.2f}..{:.2f} lane {:.1f}..{:.1f}",
                    t + 1, nx, next.size(), ylo, yhi, laneAt(next.front().y).lo, laneAt(next.front().y).hi);
        }
        layers.push_back(std::move(next));
        x = nx;
    }

    {
        double ylo = 1e30, yhi = -1e30;
        for (auto const& nd : layers.back()) { ylo = std::min(ylo, nd.y); yhi = std::max(yhi, nd.y); }
        geode::log::info("[bexact] beam end t={} x={:.2f} layers={} last={} y {:.2f}..{:.2f}"
                         " rings={} r0=[{:.1f}..{:.1f} y {:.1f}..{:.1f} k{}] yStart={:.2f} per={:.5f}"
                         " hw={:.1f} hh={:.1f}",
            t, x, layers.size(), layers.back().size(), ylo, yhi, nr,
            nr ? rings[0].x0 : 0.0, nr ? rings[0].x1 : 0.0, nr ? rings[0].y0 : 0.0,
            nr ? rings[0].y1 : 0.0, nr ? rings[0].kind : -1,
            prm.yStart, prm.perTick, prm.hw, prm.hh);
    }
    if (layers.size() <= 1) return out;
    out.depth = (int) layers.size() - 1;
    out.ok = out.depth >= horizon || (stopX > 0.0 && (prm.dir >= 0.0 ? x >= stopX : x <= stopX));
    out.reachedEnd = out.ok;
    out.endX = x;
    // walk the parent chain back from the state closest to the middle of its lane
    int idx = 0;
    {
        auto const& last = layers.back();
        blocked.clear();
        cor.solidRectsAt((float) (x - prm.hw), (float) (x + prm.hw), blocked);
        Lane lane = laneAt(last.front().y);
        double mid = lane.ok ? 0.5 * (lane.lo + lane.hi) : last.front().y;
        double bestd = 1e30;
        for (int i = 0; i < (int) last.size(); i++) {
            double d = std::abs(last[(size_t) i].y - mid);
            if (d < bestd) { bestd = d; idx = i; }
        }
    }
    size_t n = layers.size();
    out.act.resize(n - 1);
    out.y.resize(n - 1);
    out.vy.resize(n - 1);
    out.g.resize(n - 1);
    out.held.resize(n - 1);
    out.buf.resize(n - 1);
    for (size_t li = n - 1; li >= 1; li--) {
        Node const& nd = layers[li][(size_t) idx];
        Node const& pr = layers[li - 1][(size_t) nd.parent];
        out.act[li - 1]  = nd.act;
        out.y[li - 1]    = (float) pr.y;
        out.vy[li - 1]   = (float) pr.vy;
        out.g[li - 1]    = pr.g;
        out.held[li - 1] = pr.held;
        out.buf[li - 1]  = pr.buf;
        idx = nd.parent;
    }
    return out;
}

}   // namespace ballexact
}   // namespace gdpf
