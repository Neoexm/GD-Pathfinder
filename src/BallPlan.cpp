#include "BallPlan.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace gdpf {

namespace {
    double nowSeconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    inline int cellAt(double y, double yBase) {
        int yi = (int) std::floor((y - yBase) / (double) BallPlan::kYStep);
        return yi >= 0 && yi < BallPlan::kYCells ? yi : -1;
    }
    inline bool flipsAfter(int kind) { return kind == BallPlan::OrbBlue; }
    inline bool flipsBefore(int kind) { return kind == BallPlan::OrbGreen; }
    inline bool againstGravity(int kind) { return kind != BallPlan::OrbDrop; }
}

int BallPlan::orbAtLayer(Plan const& pl, int layer, double y) {
    if (!pl.anyOrb) return -1;
    for (int k = 0; k < OrbCount; k++) {
        double olo = (double) pl.orbLo[(size_t) layer * OrbCount + k];
        double ohi = (double) pl.orbHi[(size_t) layer * OrbCount + k];
        if (olo <= ohi && y >= olo && y <= ohi) return k;
    }
    return -1;
}

bool BallPlan::stepOne(Plan const& pl, double& y, double& dy, int& g, int layer, bool press,
                       int* minRoom) {
    double lo = (double) pl.solLo[(size_t) layer];
    double hi = (double) pl.solHi[(size_t) layer];
    if (hi < lo) return false;
    dy += g ? pl.accel : -pl.accel;

    bool buffered = false;
    if (press) {
        int kind = orbAtLayer(pl, layer, y);
        if (kind >= 0) {
            if (flipsBefore(kind)) g ^= 1;
            double mag = pl.orbDy[kind];
            int dirg = g ? -1 : 1;                   // dir = m_isUpsideDown ? -1 : +1
            dy = againstGravity(kind) ? dirg * mag : -dirg * mag;
            if (flipsAfter(kind)) g ^= 1;
        } else {
            bool onSurface = g ? (y >= hi - 0.02) : (y <= lo + 0.02);
            if (onSurface) {
                g ^= 1;
                dy = g ? pl.jumpDy : -pl.jumpDy;
            } else {
                buffered = true;
            }
        }
    }

    dy = std::clamp(dy, -pl.dyLim, pl.dyLim);
    int next = layer + 1;
    double nlo = (double) pl.solLo[(size_t) next];
    double nhi = (double) pl.solHi[(size_t) next];
    if (nhi < nlo) return false;
    double ny = y + dy;
    if (ny <= nlo) { ny = nlo; dy = 0.0; }           // resting on the surface below
    else if (ny >= nhi) { ny = nhi; dy = 0.0; }      // or the one above
    if (buffered) {
        int kind = orbAtLayer(pl, next, ny);
        if (kind >= 0) {
            // The impulse arrives after the move, so it does not move the ball this tick,
            // and it carries the gravity already applied under the OLD gravity.
            double grav = g ? pl.accel : -pl.accel;
            if (flipsBefore(kind)) g ^= 1;
            double mag = pl.orbDy[kind];
            int dirg = g ? -1 : 1;
            dy = (againstGravity(kind) ? dirg * mag : -dirg * mag) + grav;
            if (flipsAfter(kind)) g ^= 1;
            dy = std::clamp(dy, -pl.dyLim, pl.dyLim);
        }
    }
    int nyi = cellAt(ny, pl.yBase);
    if (nyi < 0) return false;
    if (!pl.freeHaz[(size_t) next * kYCells + (size_t) nyi]) return false;
    if (minRoom && !pl.room.empty()) {
        int r = (int) pl.room[(size_t) next * kYCells + (size_t) nyi];
        if (r < *minRoom) *minRoom = r;
    }
    y = ny;
    return true;
}

void BallPlan::build(GapQuery const& gaps, Params const& p, int block, double u0, float yCentre,
                     Plan& out) {
    double t0 = nowSeconds();
    out.ok = false;
    out.block = block;
    out.builtTick = p.tick;
    out.perTick = p.perTick;
    out.accel = std::max(1e-4, p.accel);
    out.dyStep = 0.5 * out.accel;
    out.dyLim = std::min(p.dyMax, (double) kDyHalf * out.dyStep);
    out.jumpDy = p.jumpDy;
    for (int k = 0; k < OrbCount; k++) out.orbDy[k] = p.orbDy[k];
    out.yBase = (float) std::floor((double) yCentre - 0.5 * kYStep * kYCells);

    out.freeHaz.assign((size_t) (kTickLayers + 1) * kYCells, 0);
    out.solLo.assign((size_t) (kTickLayers + 1), out.yBase);
    out.solHi.assign((size_t) (kTickLayers + 1), out.yBase + (float) (kYCells * kYStep));
    out.orbLo.assign((size_t) (kTickLayers + 1) * OrbCount, 1.f);
    out.orbHi.assign((size_t) (kTickLayers + 1) * OrbCount, 0.f);
    out.layerU.assign((size_t) (kTickLayers + 2), 0.f);
    out.anyOrb = false;

    std::vector<std::pair<float, float>> blocked;
    bool anyContent = false;
    double planLo = (double) out.yBase + 1.0;
    double planHi = (double) out.yBase + (double) (kYCells * kYStep) - 1.0;
    double ref = (double) yCentre;
    double u = u0;
    double pt = (double) p.perTick;
    float qlo[OrbCount], qhi[OrbCount];
    for (int k = 0; k <= kTickLayers; k++) {
        out.layerU[(size_t) k] = (float) u;
        if (p.perTickAt) {
            float got = p.perTickAt((float) (p.dir >= 0.f ? u : -u), p.perTick);
            if (got > 0.1f) pt = (double) got;
        }
        double ua = u, ub = u + pt;
        u = ub;
        float a = (float) (p.dir >= 0.f ? ua : -ub);
        float b = (float) (p.dir >= 0.f ? ub : -ua);
        auto* mh = &out.freeHaz[(size_t) k * kYCells];
        for (int yi = 0; yi < kYCells; yi++) mh[yi] = 1;
        bool content = false;
        float top = 0.f;
        gaps(a - p.innerHalfW, b + p.innerHalfW, (float) k, true, blocked, content, top);
        if (content) anyContent = true;
        for (auto const& iv : blocked) {
            double blo = (double) iv.first - (double) p.innerHalf - (double) out.yBase - 0.5 * kYStep;
            double bhi = (double) iv.second + (double) p.innerHalf - (double) out.yBase - 0.5 * kYStep;
            int y0 = std::max(0, (int) std::ceil(blo / (double) kYStep));
            int y1 = std::min(kYCells - 1, (int) std::floor(bhi / (double) kYStep));
            for (int yi = y0; yi <= y1; yi++) mh[yi] = 0;
        }
        gaps(a - p.halfWidth, b + p.halfWidth, (float) k, false, blocked, content, top);
        if (content) anyContent = true;
        double lo = planLo, hi = planHi;
        for (auto const& iv : blocked) {
            double bl = (double) iv.first - (double) p.halfHeight;
            double bh = (double) iv.second + (double) p.halfHeight;
            if (bh <= ref) lo = std::max(lo, bh);
            else if (bl >= ref) hi = std::min(hi, bl);
            else if (ref - bl < bh - ref) hi = std::min(hi, bl);
            else lo = std::max(lo, bh);
        }
        out.solLo[(size_t) k] = (float) lo;
        out.solHi[(size_t) k] = (float) hi;
        if (hi >= lo) ref = std::clamp(ref, lo, hi);
        if (p.orbAt) {
            for (int j = 0; j < OrbCount; j++) { qlo[j] = 1.f; qhi[j] = 0.f; }
            if (p.orbAt(a - p.halfWidth, b + p.halfWidth, qlo, qhi)) {
                for (int j = 0; j < OrbCount; j++) {
                    if (qlo[j] > qhi[j]) continue;
                    out.orbLo[(size_t) k * OrbCount + j] = qlo[j] - p.halfHeight;
                    out.orbHi[(size_t) k * OrbCount + j] = qhi[j] + p.halfHeight;
                    out.anyOrb = true;
                }
            }
        }
    }
    out.layerU[(size_t) (kTickLayers + 1)] = (float) u;
    if (!anyContent) { planSeconds += nowSeconds() - t0; return; }
    out.room.assign((size_t) (kTickLayers + 1) * kYCells, 0);
    for (int k = 0; k <= kTickLayers; k++) {
        auto const* mh = &out.freeHaz[(size_t) k * kYCells];
        auto* rm = &out.room[(size_t) k * kYCells];
        int loCell = std::max(0, (int) std::ceil((double) out.solLo[(size_t) k] - (double) out.yBase - 0.5));
        int hiCell = std::min(kYCells - 1, (int) std::floor((double) out.solHi[(size_t) k] - (double) out.yBase - 0.5));
        int run = 0;
        for (int yi = 0; yi < kYCells; yi++) {
            bool ok = mh[yi] && yi >= loCell && yi <= hiCell;
            run = ok ? std::min(run + 1, kRoomCap) : 0;
            rm[yi] = (uint8_t) run;
        }
        run = 0;
        for (int yi = kYCells - 1; yi >= 0; yi--) {
            bool ok = mh[yi] && yi >= loCell && yi <= hiCell;
            run = ok ? std::min(run + 1, kRoomCap) : 0;
            rm[yi] = (uint8_t) std::min<int>(rm[yi], run);
        }
    }

    size_t const layerCells = (size_t) 2 * kDyCells * kYCells;
    out.depth.assign((size_t) (kStrides + 1) * layerCells, 0);
    out.value.assign((size_t) (kStrides + 1) * layerCells, 0);
    std::fill(out.value.begin() + (size_t) kStrides * layerCells, out.value.end(),
              (uint8_t) kRoomCap);
    for (int s = kStrides - 1; s >= 0; s--) {
        int tb = s * kStride;
        auto const* mh0 = &out.freeHaz[(size_t) tb * kYCells];
        auto const* d1 = &out.depth[(size_t) (s + 1) * layerCells];
        auto* d0 = &out.depth[(size_t) s * layerCells];
        auto const* v1 = &out.value[(size_t) (s + 1) * layerCells];
        auto* v0 = &out.value[(size_t) s * layerCells];
        double lo0 = (double) out.solLo[(size_t) tb], hi0 = (double) out.solHi[(size_t) tb];
        int reach = std::min(kStrides - s, kHorizonStrides);
        for (int g0 = 0; g0 < 2; g0++) {
            for (int dyj = 0; dyj < kDyCells; dyj++) {
                double dy0 = (double) (dyj - kDyHalf) * out.dyStep;
                for (int yi = 0; yi < kYCells; yi++) {
                    if (!mh0[yi]) continue;
                    double cb = (double) out.yBase + (double) yi * kYStep;
                    double ct = cb + kYStep;
                    if (lo0 >= ct || hi0 < cb) continue;
                    double yc = std::clamp(cb + 0.5 * kYStep, lo0, hi0);
                    int best = 0, bestVal = 0;
                    for (int at = -1; at < kStride; at++) {
                        double y = yc, dy = dy0;
                        int g = g0;
                        int mr = kRoomCap;
                        bool alive = true;
                        for (int t = 0; t < kStride && alive; t++)
                            alive = stepOne(out, y, dy, g, tb + t, t == at, &mr);
                        if (!alive) continue;
                        int yi1 = cellAt(y, out.yBase);
                        if (yi1 < 0) continue;
                        int dj1 = std::clamp((int) std::lround(dy / out.dyStep) + kDyHalf, 0, kDyCells - 1);
                        size_t idx = ((size_t) (g * kDyCells + dj1)) * kYCells + (size_t) yi1;
                        int d = 1 + (int) d1[idx];
                        if (d > best) best = d;
                        int val = std::min(mr, (int) v1[idx]);
                        if (val > bestVal) bestVal = val;
                        if (best >= reach && bestVal >= kRoomCap) break;
                    }
                    size_t o = ((size_t) (g0 * kDyCells + dyj)) * kYCells + (size_t) yi;
                    d0[o] = (uint8_t) std::min(best, 255);
                    v0[o] = (uint8_t) std::min(bestVal, kRoomCap);
                }
            }
        }
    }
    out.ok = true;
    plansBuilt++;
    if (trace > 0 && (int) traces.size() < 12) {
        std::string line = std::string("[bplan] block ") + std::to_string(block)
                         + " u0 " + std::to_string((long long) u0) + " yBase "
                         + std::to_string((int) out.yBase) + " orbs " + (out.anyOrb ? "y" : "n")
                         + " |";
        for (int s = 0; s < kStrides; s += 2) {
            int reach = std::min(kStrides - s, kHorizonStrides);
            auto const* d = &out.depth[(size_t) s * layerCells];
            int lo = -1, hi = -1;
            for (int yi = 0; yi < kYCells; yi++)
                for (int g = 0; g < 2; g++)
                    if ((int) d[((size_t) (g * kDyCells + kDyHalf)) * kYCells + yi] >= reach) {
                        if (lo < 0) lo = yi;
                        hi = yi;
                    }
            char buf[48];
            if (lo < 0) std::snprintf(buf, sizeof buf, " %d:-", s * kStride);
            else std::snprintf(buf, sizeof buf, " %d:%d-%d", s * kStride,
                               (int) out.yBase + lo, (int) out.yBase + hi);
            line += buf;
        }
        char sb[64];
        std::snprintf(sb, sizeof sb, " | sol %.0f..%.0f", out.solLo[0], out.solHi[0]);
        line += sb;
        traces.push_back(std::move(line));
    }
    planSeconds += nowSeconds() - t0;
}

int BallPlan::tickOffOf(Plan const& pl, Params const& p, float x) {
    double u = (double) p.dir * (double) x;
    int hi = kBlockStrides * kStride;
    if (u < (double) pl.layerU[0] - 1.0) return -1;
    auto begin = pl.layerU.begin();
    auto it = std::upper_bound(begin, begin + hi + 1, (float) u);
    int k = (int) (it - begin) - 1;
    if (k < 0) k = 0;
    if (k >= hi) return -1;
    return k;
}

BallPlan::Plan const* BallPlan::planFor(GapQuery const& gaps, Params const& p, float x, float y) {
    double u = (double) p.dir * (double) x;
    double bw = kBlockUnits;
    int block = (int) std::floor(u / bw);
    int yb = (int) std::floor((double) y / kYBucket);
    float yBase = (float) (yb * (int) kYBucket - 20);
    int key = ((block * 1024) + (yb & 1023)) * 2 + (p.dir >= 0.f ? 1 : 0);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        auto const& pl = it->second;
        bool yok = y >= pl.yBase + 8.f && y < pl.yBase + (float) (kYCells - 8) * kYStep;
        bool xok = !pl.layerU.empty()
                 && u >= (double) pl.layerU[0] - 1.0
                 && u < (double) pl.layerU[(size_t) (kBlockStrides * kStride)];
        if (yok && xok && std::fabs(pl.perTick - p.perTick) < 0.01f) return &it->second;
        m_cache.erase(it);
    }
    if (m_cache.size() >= cacheCap) {
        auto worst = m_cache.begin();
        for (auto j = m_cache.begin(); j != m_cache.end(); ++j)
            if (std::abs(j->first - key) > std::abs(worst->first - key)) worst = j;
        m_cache.erase(worst);
    }
    Plan plan;
    build(gaps, p, block, (double) block * bw, yBase + 0.5f * (float) (kYCells * kYStep), plan);
    if (!plan.ok) return nullptr;
    return &(m_cache[key] = std::move(plan));
}

BallPlan::Verdict BallPlan::query(GapQuery const& gaps, Params const& p, float x, float y,
                                  double dy) {
    Verdict v;
    auto const* plan = planFor(gaps, p, x, y);
    if (!plan || plan->depth.empty()) return v;
    queries++;
    int tickOff = tickOffOf(*plan, p, x);
    if (tickOff < 0) { offGrid++; return v; }
    if (cellAt((double) y, plan->yBase) < 0) { offGrid++; return v; }

    int rem = tickOff % kStride;
    int n = rem == 0 ? kStride : kStride - rem;
    int sb = (tickOff + n) / kStride;
    size_t const layerCells = (size_t) 2 * kDyCells * kYCells;
    int g0 = p.flipped ? 1 : 0;
    int out[2] = {0, 0};
    int room[2] = {0, 0};
    uint32_t const tail = n > 1 ? (1u << (n - 1)) : 1u;
    for (int choice = 0; choice < 2; choice++) {
        int best = 0, bestRoom = 0;
        for (uint32_t tb = 0; tb < tail; tb++) {
            uint32_t bits = (tb << 1) | (uint32_t) choice;
            double yy = (double) y, vv = dy;
            int g = g0;
            int mr = kRoomCap;
            bool alive = true;
            for (int t = 0; t < n && alive; t++)
                alive = stepOne(*plan, yy, vv, g, tickOff + t, ((bits >> t) & 1u) != 0, &mr);
            if (!alive) continue;
            int yi1 = cellAt(yy, plan->yBase);
            if (yi1 < 0) continue;
            int dj1 = std::clamp((int) std::lround(vv / plan->dyStep) + kDyHalf, 0, kDyCells - 1);
            size_t idx = (size_t) sb * layerCells + ((size_t) (g * kDyCells + dj1)) * kYCells + (size_t) yi1;
            int d = n + kStride * (int) plan->depth[idx];
            int r = std::min(mr, (int) plan->value[idx]);
            if (d > best) best = d;
            if (r > bestRoom) bestRoom = r;
        }
        out[choice] = std::min(best, kHorizonTicks);
        room[choice] = bestRoom;
    }
    v.ok = true;
    v.idleDepth = out[0];
    v.pressDepth = out[1];
    v.idleRoom = room[0];
    v.pressRoom = room[1];
    v.depth = std::max(out[0], out[1]);
    return v;
}

BallPlan::Advice BallPlan::advise(GapQuery const& gaps, Params const& p, float x, float y,
                                  int aheadTicks) {
    Advice a;
    auto const* plan = planFor(gaps, p, x, y);
    if (!plan || plan->depth.empty()) return a;
    int tickOff = tickOffOf(*plan, p, x);
    if (tickOff < 0) return a;
    int sa = std::clamp((tickOff + std::max(0, aheadTicks)) / kStride, 0, kStrides - 1);
    int reach = std::min(kStrides - sa, kHorizonStrides);
    size_t const layerCells = (size_t) 2 * kDyCells * kYCells;
    auto const* d = &plan->depth[(size_t) sa * layerCells];
    int yiNow = cellAt((double) y, plan->yBase);
    if (yiNow < 0) return a;
    int bestLo = -1, bestHi = -1, curLo = -1;
    int nearLo = -1, nearHi = -1, nearDist = 1 << 30;
    for (int yi = 0; yi <= kYCells; yi++) {
        bool alive = false;
        if (yi < kYCells)
            for (int g = 0; g < 2 && !alive; g++)
                for (int dyj = 0; dyj < kDyCells && !alive; dyj++)
                    if ((int) d[((size_t) (g * kDyCells + dyj)) * kYCells + yi] >= reach) alive = true;
        if (alive) { if (curLo < 0) curLo = yi; continue; }
        if (curLo >= 0) {
            int lo = curLo, hi = yi - 1;
            int dist = yiNow < lo ? lo - yiNow : (yiNow > hi ? yiNow - hi : 0);
            if (dist < nearDist || (dist == nearDist && hi - lo > nearHi - nearLo)) {
                nearDist = dist; nearLo = lo; nearHi = hi;
            }
            if (hi - lo > bestHi - bestLo) { bestLo = lo; bestHi = hi; }
            curLo = -1;
        }
    }
    if (nearLo < 0) { if (bestLo < 0) return a; nearLo = bestLo; nearHi = bestHi; }
    a.ok = true;
    // In cells; scale if kYStep ever stops being one unit.
    a.lo = (int) std::lround((double) plan->yBase + (double) nearLo * kYStep);
    a.hi = (int) std::lround((double) plan->yBase + (double) nearHi * kYStep);
    a.bestY = (float) ((double) plan->yBase
                       + (0.5 * (double) (nearLo + nearHi) + 0.5) * kYStep);
    return a;
}

}
