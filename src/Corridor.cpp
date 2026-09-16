#include "Corridor.hpp"
#include "BallPlan.hpp"

#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include "Engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace geode::prelude;

namespace gdpf {

float Corridor::s_moverSweep = 0.f;

namespace {
    bool blocksPlayer(GameObject* obj) {
        if (!obj) return false;
        if (obj->m_isDecoration || obj->m_isDecoration2 || obj->m_isTrigger || obj->m_isPassable || obj->m_isNoTouch) return false;
        switch (obj->m_objectType) {
            case GameObjectType::Solid:
            case GameObjectType::Hazard:
            case GameObjectType::AnimatedHazard:
            case GameObjectType::Slope:
            case GameObjectType::Breakable:
                return true;
            default:
                return false;
        }
    }

    bool changesHitboxes(int triggerID) {
        switch (triggerID) {
            case 901:
            case 1346:
            case 2067:
            case 1347:
            case 1814:
            case 3006:
            case 3007:
            case 3008:
            case 1049:
            case 1611:
            case 1811:
            case 1815:
            case 1595:
                return true;
            default:
                return false;
        }
    }

    bool currentlyOff(GameObject* obj) {
        return obj->m_isGroupDisabled || obj->m_isDisabled || obj->m_isDisabled2;
    }

    // 200 = 0.5x, 201 = 1x, 202 = 2x, 203 = 3x, 1334 = 4x. -1 for anything else.
    int speedRankOfId(int id) {
        switch (id) {
            case 200: return 0;
            case 201: return 1;
            case 202: return 2;
            case 203: return 3;
            case 1334: return 4;
            default: return -1;
        }
    }

    bool changesMotion(GameObject* obj) {
        switch (obj->m_objectType) {
            case GameObjectType::CubePortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::BallPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::GravityTogglePortal:
            case GameObjectType::GravityPad:
            case GameObjectType::GravityRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::InverseMirrorPortal:
            case GameObjectType::NormalMirrorPortal:
            case GameObjectType::RegularSizePortal:
            case GameObjectType::MiniSizePortal:
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
            case GameObjectType::TeleportPortal:
            case GameObjectType::TeleportOrb:
                return true;
            case GameObjectType::Modifier:
                switch (obj->m_objectID) {
                    case 200: case 201: case 202: case 203: case 1334: return true;
                    default: return false;
                }
            default:
                return false;
        }
    }

    bool isHazardType(GameObject* obj) {
        return obj->m_objectType == GameObjectType::Hazard || obj->m_objectType == GameObjectType::AnimatedHazard;
    }
}

void Corridor::addInterval(std::map<int, std::vector<Interval>>& cols, cocos2d::CCRect const& rect) {
    int c0 = (int) std::floor(rect.getMinX() / kColumnWidth);
    int c1 = (int) std::floor(rect.getMaxX() / kColumnWidth);
    for (int c = c0; c <= c1; c++) cols[c].push_back({rect.getMinY(), rect.getMaxY()});
}

void Corridor::mergeColumns(std::map<int, std::vector<Interval>>& cols) {
    for (auto& [c, list] : cols) {
        std::sort(list.begin(), list.end(), [](Interval const& a, Interval const& b) { return a.lo < b.lo; });
        std::vector<Interval> merged;
        for (auto const& iv : list) {
            if (!merged.empty() && iv.lo <= merged.back().hi + 0.5f) merged.back().hi = std::max(merged.back().hi, iv.hi);
            else merged.push_back(iv);
        }
        list = std::move(merged);
    }
}

bool Corridor::columnsHit(std::map<int, std::vector<Interval>> const& cols, float x0, float x1, float y0, float y1) {
    int c0 = (int) std::floor(x0 / kColumnWidth);
    int c1 = (int) std::floor(x1 / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = cols.find(c);
        if (it == cols.end()) continue;
        for (auto const& iv : it->second) {
            if (iv.lo > y1) break;
            if (iv.hi > y0) return true;
        }
    }
    return false;
}

bool Corridor::dynamicHit(float x0, float x1, float y0, float y1, bool hazard) const {
    if (m_movingBlocking.empty()) return false;
    int b0 = (int) std::floor(x0 / kDynamicBucket);
    int b1 = (int) std::floor(x1 / kDynamicBucket);
    for (int b = b0; b <= b1; b++) {
        auto it = m_dynamicIndex.find(b);
        if (it == m_dynamicIndex.end()) continue;
        for (auto idx : it->second) {
            auto obj = m_dynamic[idx];
            if (currentlyOff(obj) || isHazardType(obj) != hazard) continue;
            auto const& r = engine::peekRect(obj);
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
            float sx = 0.f, sy = 0.f;
            if (s_moverSweep > 0.f && m_haveMotion && idx < m_vel.size()) {
                sx = std::fabs(m_vel[idx].x) * s_moverSweep;
                sy = std::fabs(m_vel[idx].y) * s_moverSweep;
            }
            if (r.getMaxX() + sx < x0 || r.getMinX() - sx > x1 || r.getMaxY() + sy < y0 || r.getMinY() - sy > y1) continue;
            return true;
        }
    }
    return false;
}

bool Corridor::boxHitsHazard(float x0, float x1, float y0, float y1) const {
    return columnsHit(m_hazardColumns, x0, x1, y0, y1) || dynamicHit(x0, x1, y0, y1, true);
}

bool Corridor::boxHitsHazardExact(float x0, float x1, float y0, float y1) const {
    int c0 = (int) std::floor(x0 / kColumnWidth);
    int c1 = (int) std::floor(x1 / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = m_hazardRectIndex.find(c);
        if (it == m_hazardRectIndex.end()) continue;
        for (int i : it->second) {
            Rect4 const& r = m_hazardRects[(size_t) i];
            if (r.x1 < x0 || r.x0 > x1 || r.y1 < y0 || r.y0 > y1) continue;
            return true;
        }
    }
    return dynamicHit(x0, x1, y0, y1, true);
}

void Corridor::solidRectsAt(float x0, float x1, std::vector<std::pair<float, float>>& out) const {
    out.clear();
    int c0 = (int) std::floor(x0 / kColumnWidth);
    int c1 = (int) std::floor(x1 / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = m_solidRectIndex.find(c);
        if (it == m_solidRectIndex.end()) continue;
        for (int i : it->second) {
            Rect4 const& r = m_solidRects[(size_t) i];
            if (r.x1 < x0 || r.x0 > x1) continue;
            out.emplace_back(r.y0, r.y1);
        }
    }
}

bool Corridor::boxHitsSolid(float x0, float x1, float y0, float y1) const {
    return columnsHit(m_solidColumns, x0, x1, y0, y1) || dynamicHit(x0, x1, y0, y1, false);
}

bool Corridor::roomAt(float x0, float x1, float y, float cap, float& below, float& above) const {
    below = cap;
    above = cap;
    bool any = false;
    int c0 = (int) std::floor(x0 / kColumnWidth);
    int c1 = (int) std::floor(x1 / kColumnWidth);
    auto consider = [&](float lo, float hi) {
        any = true;
        if (hi <= y) below = std::min(below, y - hi);
        else if (lo >= y) above = std::min(above, lo - y);
        else { below = 0.f; above = 0.f; }
    };
    for (int c = c0; c <= c1; c++) {
        auto it = m_columns.find(c);
        if (it == m_columns.end()) continue;
        for (auto const& iv : it->second) consider(iv.lo, iv.hi);
    }
    if (!m_movingBlocking.empty()) {
        int b0 = (int) std::floor(x0 / kDynamicBucket);
        int b1 = (int) std::floor(x1 / kDynamicBucket);
        for (int b = b0; b <= b1; b++) {
            auto it = m_dynamicIndex.find(b);
            if (it == m_dynamicIndex.end()) continue;
            for (auto idx : it->second) {
                auto obj = m_dynamic[idx];
                if (currentlyOff(obj)) continue;
                auto const& r = engine::peekRect(obj);
                if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
                if (r.getMaxX() < x0 || r.getMinX() > x1) continue;
                consider(r.getMinY(), r.getMaxY());
            }
        }
    }
    if (y - m_groundY < below) { below = std::max(0.f, y - m_groundY); any = true; }
    return any;
}

static float s_dumpLo = 0.f;
static float s_dumpHi = -1.f;
void Corridor::setDumpWindow(float lo, float hi) { s_dumpLo = lo; s_dumpHi = hi; }

void Corridor::build(PlayLayer* pl) {
    m_columns.clear();
    m_hazardColumns.clear();
    m_solidColumns.clear();
    m_portals.clear();
    m_dynamic.clear();
    m_dynamicIndex.clear();
    m_indexX.clear();
    m_basePos.clear();
    m_movingBlocking.clear();
    m_prevPos.clear();
    m_vel.clear();
    m_haveMotion = false;
    m_objectCount = 0;
    m_items.clear();
    m_rings.clear();
    m_orbs.clear();
    m_speedPortals.clear();
    m_hazardRects.clear();
    m_solidRects.clear();
    m_hazardRectIndex.clear();
    m_solidRectIndex.clear();
    if (!pl || !pl->m_objects) return;

    std::unordered_set<int> dynamicGroups;
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj || !obj->m_isTrigger) continue;
        if (!changesHitboxes(obj->m_objectID)) continue;
        auto eff = static_cast<EffectGameObject*>(obj);
        if (eff->m_targetGroupID > 0) dynamicGroups.insert(eff->m_targetGroupID);
    }
    auto isDynamic = [&](GameObject* obj) {
        if (dynamicGroups.empty() || !obj->m_groups || obj->m_groupCount <= 0) return false;
        int n = std::min<int>(obj->m_groupCount, 10);
        for (int i = 0; i < n; i++) if (dynamicGroups.count((*obj->m_groups)[i])) return true;
        return false;
    };

    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj || obj->m_objectType == GameObjectType::Decoration) continue;
        if (changesMotion(obj)) m_portals.push_back(obj->getPositionX());
        if (int rank = speedRankOfId(obj->m_objectID); rank >= 0) {
            auto const& rc = engine::peekRect(obj);
            m_speedPortals.push_back(Item{rc.origin.x + rc.size.width * 0.5f,
                                          rc.origin.y + rc.size.height * 0.5f, rank});
        }
        if (int kind = orbKindOf(obj->m_objectType); kind >= 0) {
            auto const& rc = engine::peekRect(obj);
            m_orbs.push_back(Orb{rc.getMinX(), rc.getMaxX(), rc.getMinY(), rc.getMaxY(), kind});
        }
        switch (obj->m_objectType) {
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::SpiderOrb:
            case GameObjectType::TeleportOrb:
                m_rings.push_back({obj->getPositionX(), obj->getPositionY(), obj->m_objectID});
                break;
            default: break;
        }
        if (obj->m_objectType == GameObjectType::Collectible) {
            m_items.push_back({obj->getPositionX(), obj->getPositionY(), obj->m_objectID});
        }
        bool blocking = blocksPlayer(obj);
        auto rect = engine::peekRect(obj);
        bool hasRect = rect.size.width > 0.f && rect.size.height > 0.f;
        if (isDynamic(obj)) {
            uint32_t idx = (uint32_t) m_dynamic.size();
            m_dynamic.push_back(obj);
            if (blocking && hasRect) {
                m_objectCount++;
                m_movingBlocking.push_back(idx);
                float x = obj->getPositionX();
                int b0 = (int) std::floor((x - kDynamicRadius) / kDynamicBucket);
                int b1 = (int) std::floor((x + kDynamicRadius) / kDynamicBucket);
                for (int b = b0; b <= b1; b++) m_dynamicIndex[b].push_back(idx);
                if (m_indexX.size() <= idx) m_indexX.resize(idx + 1, 0.f);
                m_indexX[idx] = x;
                if (m_basePos.size() <= idx) m_basePos.resize(idx + 1, cocos2d::CCPoint(0.f, 0.f));
                m_basePos[idx] = obj->getPosition();
            }
            continue;
        }
        if (!blocking || !hasRect) continue;
        m_objectCount++;
        addInterval(m_columns, rect);
        addInterval(isHazardType(obj) ? m_hazardColumns : m_solidColumns, rect);
        {
            bool hz = isHazardType(obj);
            auto& vec = hz ? m_hazardRects : m_solidRects;
            auto& idx = hz ? m_hazardRectIndex : m_solidRectIndex;
            int at = (int) vec.size();
            vec.push_back(Rect4{rect.getMinX(), rect.getMaxX(), rect.getMinY(), rect.getMaxY()});
            int rc0 = (int) std::floor(rect.getMinX() / kColumnWidth);
            int rc1 = (int) std::floor(rect.getMaxX() / kColumnWidth);
            for (int cc = rc0; cc <= rc1; cc++) idx[cc].push_back(at);
        }
        if (s_dumpHi > s_dumpLo && rect.getMaxX() >= s_dumpLo && rect.getMinX() <= s_dumpHi) {
            geode::log::debug("[cdump] {} id={} x[{:.2f}..{:.2f}] y[{:.2f}..{:.2f}]",
                isHazardType(obj) ? "haz" : "solid", obj->m_objectID,
                rect.getMinX(), rect.getMaxX(), rect.getMinY(), rect.getMaxY());
        }
    }
    if (s_dumpHi > s_dumpLo) {
        for (auto const& o : m_orbs) {
            if (o.x1 < s_dumpLo || o.x0 > s_dumpHi) continue;
            geode::log::debug("[cdump] orb kind={} x[{:.2f}..{:.2f}] y[{:.2f}..{:.2f}]",
                o.kind, o.x0, o.x1, o.y0, o.y1);
        }
        for (auto const& sp : m_speedPortals)
            geode::log::debug("[pdump] speed rank={} x={:.2f} y={:.2f}", sp.id, sp.x, sp.y);
    }
    mergeColumns(m_columns);
    mergeColumns(m_hazardColumns);
    mergeColumns(m_solidColumns);
    std::sort(m_portals.begin(), m_portals.end());
    std::sort(m_speedPortals.begin(), m_speedPortals.end(),
        [](Item const& a, Item const& b) { return a.x < b.x; });
    if (!m_speedPortals.empty()) {
        std::string list;
        for (size_t i = 0; i < m_speedPortals.size() && i < 24; i++)
            list += fmt::format("{}{:.0f}@y{:.0f}=r{}", i ? " " : "", m_speedPortals[i].x, m_speedPortals[i].y,
                m_speedPortals[i].id);
        log::info("[corridor] {} speed portals: {}", m_speedPortals.size(), list);
    }
    std::sort(m_items.begin(), m_items.end(), [](Item const& a, Item const& b) { return a.x < b.x; });
    std::sort(m_rings.begin(), m_rings.end(), [](Item const& a, Item const& b) { return a.x < b.x; });
    std::sort(m_orbs.begin(), m_orbs.end(), [](Orb const& a, Orb const& b) { return a.x0 < b.x0; });
    log::info("[corridor] {} blocking objects in {} columns; {} movable/toggleable gameplay objects ({} dynamic groups)",
        m_objectCount, m_columns.size(), m_dynamic.size(), dynamicGroups.size());
    if (!m_items.empty()) {
        std::string s;
        for (size_t i = 0; i < m_items.size() && i < 12; i++) s += fmt::format("{}id{}@({:.0f},{:.0f})", i ? " " : "", m_items[i].id, m_items[i].x, m_items[i].y);
        log::info("[corridor] {} collectibles: {}", m_items.size(), s);
    }
    if (!m_orbs.empty()) {
        int per[BallPlan::OrbCount] = {0};
        for (auto const& o : m_orbs) per[o.kind]++;
        log::info("[corridor] {} modelled orbs: blue {} yellow {} pink {} red {} green {} drop {}",
                  m_orbs.size(), per[BallPlan::OrbBlue], per[BallPlan::OrbYellow],
                  per[BallPlan::OrbPink], per[BallPlan::OrbRed], per[BallPlan::OrbGreen],
                  per[BallPlan::OrbDrop]);
    }
    if (!m_rings.empty()) {
        log::info("[corridor] {} rings/orbs", m_rings.size());
    }
}

void Corridor::liveRectsNear(float x0, float x1, std::vector<LiveRect>& out) const {
    out.clear();
    float lo = std::min(x0, x1), hi = std::max(x0, x1);
    for (uint32_t idx = 0; idx < m_dynamic.size(); idx++) {
        auto obj = m_dynamic[idx];
        if (!obj) continue;
        auto const& r = engine::peekRect(obj);
        if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
        if (r.getMaxX() < lo || r.getMinX() > hi) continue;
        bool moved = false;
        if (idx < m_basePos.size()) {
            auto p = obj->getPosition();
            moved = std::fabs(p.x - m_basePos[idx].x) >= 1.f || std::fabs(p.y - m_basePos[idx].y) >= 1.f;
        }
        auto ty = obj->m_objectType;
        bool interesting = ty == GameObjectType::Solid || ty == GameObjectType::Hazard
            || ty == GameObjectType::AnimatedHazard || ty == GameObjectType::Slope
            || ty == GameObjectType::Breakable;
        if (!interesting) continue;
        out.push_back({obj->m_objectID, (int) obj->m_objectType, r.getMinX(), r.getMaxX(),
                       r.getMinY(), r.getMaxY(), isHazardType(obj), moved, currentlyOff(obj)});
        if (out.size() >= 3000) return;
    }
}

uint64_t Corridor::movedSignature(float x0, float x1, float quant) const {
    if (m_movingBlocking.empty() || m_basePos.empty()) return 0;
    if (quant <= 0.f) quant = 8.f;
    float lo = std::min(x0, x1), hi = std::max(x0, x1);
    int b0 = (int) std::floor(lo / kDynamicBucket);
    int b1 = (int) std::floor(hi / kDynamicBucket);
    uint32_t seen[96];
    int nseen = 0;
    uint64_t h = 0;
    for (int b = b0; b <= b1; b++) {
        auto it = m_dynamicIndex.find(b);
        if (it == m_dynamicIndex.end()) continue;
        for (auto idx : it->second) {
            if (idx >= m_basePos.size()) continue;
            bool dup = false;
            for (int i = 0; i < nseen; i++) if (seen[i] == idx) { dup = true; break; }
            if (dup) continue;
            if (nseen < 96) seen[nseen++] = idx;
            auto obj = m_dynamic[idx];
            auto p = obj->getPosition();
            // Undisplaced objects are wherever the level built them in every timeline.
            if (std::fabs(p.x - m_basePos[idx].x) < 1.f && std::fabs(p.y - m_basePos[idx].y) < 1.f) continue;
            auto const& r = engine::peekRect(obj);
            if (r.getMaxX() < lo || r.getMinX() > hi) continue;
            if (currentlyOff(obj)) continue;
            h += (uint64_t) (idx + 1) * 2654435761ull
               + (uint64_t) (int64_t) std::llround(p.x / quant) * 40503ull
               + (uint64_t) (int64_t) std::llround(p.y / quant) * 25717ull;
        }
    }
    return h;
}

void Corridor::rebuildDynamicIndex() {
    m_dynamicIndex.clear();
    if (m_indexX.size() < m_dynamic.size()) m_indexX.resize(m_dynamic.size(), 0.f);
    for (auto i : m_movingBlocking) {
        float x = m_dynamic[i]->getPositionX();
        int b0 = (int) std::floor((x - kDynamicRadius) / kDynamicBucket);
        int b1 = (int) std::floor((x + kDynamicRadius) / kDynamicBucket);
        for (int b = b0; b <= b1; b++) m_dynamicIndex[b].push_back(i);
        m_indexX[i] = x;
    }
    m_indexRebuilds++;
    if (m_indexRebuilds <= 8) {
        log::debug("[corridor] dynamic index rebuilt ({}) for {} moving blockers", m_indexRebuilds, m_movingBlocking.size());
    }
}

namespace {
    int s_geoLo = -1, s_geoHi = -1;
    float s_geoX0 = 0.f, s_geoX1 = 0.f;
    std::string s_geoPath;
    int s_geoLast = -1;
}

void Corridor::setGeoDump(int tickLo, int tickHi, float x0, float x1, std::string path) {
    s_geoLo = tickLo; s_geoHi = tickHi;
    s_geoX0 = std::min(x0, x1); s_geoX1 = std::max(x0, x1);
    s_geoPath = std::move(path);
    s_geoLast = -1;
}

void Corridor::sampleMotion() {
    if (s_geoLo >= 0 && !s_geoPath.empty()) {
        int t = engine::state().tick;
        if (t >= s_geoLo && t <= s_geoHi && t != s_geoLast) {
            s_geoLast = t;
            if (auto f = std::fopen(s_geoPath.c_str(), "a")) {
                // static columns first: 10-unit column index -> stacked intervals
                int c0 = (int) std::floor(s_geoX0 / kColumnWidth);
                int c1 = (int) std::floor(s_geoX1 / kColumnWidth);
                for (int c = c0; c <= c1; c++) {
                    auto hit = m_hazardColumns.find(c);
                    if (hit != m_hazardColumns.end())
                        for (auto const& iv : hit->second)
                            std::fprintf(f, "t%d staticHaz col%d x%.1f y[%.2f..%.2f]\n", t, c, c * kColumnWidth, iv.lo, iv.hi);
                    auto sit = m_solidColumns.find(c);
                    if (sit != m_solidColumns.end())
                        for (auto const& iv : sit->second)
                            std::fprintf(f, "t%d staticSol col%d x%.1f y[%.2f..%.2f]\n", t, c, c * kColumnWidth, iv.lo, iv.hi);
                }
                std::vector<LiveRect> live;
                liveRectsNear(s_geoX0, s_geoX1, live);
                for (auto const& r : live)
                    std::fprintf(f, "t%d live id%d ty%d x[%.2f..%.2f] y[%.2f..%.2f] haz%d moved%d off%d\n",
                                 t, r.id, r.type, r.x0, r.x1, r.y0, r.y1, r.hazard ? 1 : 0, r.moved ? 1 : 0, r.off ? 1 : 0);
                std::fclose(f);
            }
        }
    }
    if (m_movingBlocking.empty()) return;
    if (m_prevPos.size() != m_dynamic.size()) {
        m_prevPos.assign(m_dynamic.size(), cocos2d::CCPoint(0.f, 0.f));
        m_vel.assign(m_dynamic.size(), cocos2d::CCPoint(0.f, 0.f));
        m_haveMotion = false;
    }
    if (m_indexX.size() < m_dynamic.size()) m_indexX.resize(m_dynamic.size(), 0.f);
    float drift = 0.f;
    for (auto i : m_movingBlocking) {
        float d = std::fabs(m_dynamic[i]->getPositionX() - m_indexX[i]);
        if (d > drift) drift = d;
    }
    if (drift > kDynamicRadius * 0.5f) rebuildDynamicIndex();
    if (!m_haveMotion) {
        for (auto i : m_movingBlocking) m_prevPos[i] = m_dynamic[i]->getPosition();
        m_haveMotion = true;
        return;
    }
    for (auto i : m_movingBlocking) {
        auto p = m_dynamic[i]->getPosition();
        m_vel[i] = cocos2d::CCPoint(p.x - m_prevPos[i].x, p.y - m_prevPos[i].y);
        m_prevPos[i] = p;
    }
}

void Corridor::resetMotion() {
    m_haveMotion = false;
}

void Corridor::collectGaps(float x0, float x1, float predictTicks, std::vector<Interval>& gaps) const {
    gaps.clear();
    std::vector<Interval> blocked;
    int c0 = (int) std::floor(x0 / kColumnWidth);
    int c1 = (int) std::floor(x1 / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = m_columns.find(c);
        if (it == m_columns.end()) continue;
        blocked.insert(blocked.end(), it->second.begin(), it->second.end());
    }
    if (!m_dynamic.empty()) {
        int b0 = (int) std::floor(x0 / kDynamicBucket);
        int b1 = (int) std::floor(x1 / kDynamicBucket);
        for (int b = b0; b <= b1; b++) {
            auto it = m_dynamicIndex.find(b);
            if (it == m_dynamicIndex.end()) continue;
            for (auto idx : it->second) {
                auto obj = m_dynamic[idx];
                if (currentlyOff(obj)) continue;
                auto const& r = engine::peekRect(obj);
                if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
                float dx = 0.f, dy = 0.f;
                if (predictTicks > 0.f && m_haveMotion && idx < m_vel.size()) {
                    dx = m_vel[idx].x * predictTicks;
                    dy = m_vel[idx].y * predictTicks;
                }
                if (r.getMaxX() + dx < x0 || r.getMinX() + dx > x1) continue;
                blocked.push_back({r.getMinY() + dy, r.getMaxY() + dy});
            }
        }
    }
    if (blocked.empty()) return;
    std::sort(blocked.begin(), blocked.end(), [](Interval const& a, Interval const& b) { return a.lo < b.lo; });
    constexpr float kMinGap = 20.f;
    float cursor = m_groundY;
    for (auto const& iv : blocked) {
        if (iv.lo - cursor >= kMinGap) gaps.push_back({cursor, iv.lo});
        cursor = std::max(cursor, iv.hi);
    }
    gaps.push_back({cursor, 1e9f});
}

void Corridor::gapsAt(float x0, float x1, float predictTicks, std::vector<std::pair<float, float>>& out) const {
    out.clear();
    if (empty()) return;
    std::vector<Interval> gaps;
    collectGaps(std::min(x0, x1), std::max(x0, x1), predictTicks, gaps);
    for (auto const& g : gaps) out.emplace_back(g.lo, g.hi);
}

void Corridor::blockedAt(float x0, float x1, float predictTicks, bool hazard, std::vector<std::pair<float, float>>& out) const {
    out.clear();
    auto const& cols = hazard ? m_hazardColumns : m_solidColumns;
    int c0 = (int) std::floor(std::min(x0, x1) / kColumnWidth);
    int c1 = (int) std::floor(std::max(x0, x1) / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = cols.find(c);
        if (it == cols.end()) continue;
        for (auto const& iv : it->second) out.emplace_back(iv.lo, iv.hi);
    }
    if (m_movingBlocking.empty()) return;
    int b0 = (int) std::floor(std::min(x0, x1) / kDynamicBucket);
    int b1 = (int) std::floor(std::max(x0, x1) / kDynamicBucket);
    for (int b = b0; b <= b1; b++) {
        auto it = m_dynamicIndex.find(b);
        if (it == m_dynamicIndex.end()) continue;
        for (auto idx : it->second) {
            auto obj = m_dynamic[idx];
            if (currentlyOff(obj) || isHazardType(obj) != hazard) continue;
            auto const& r = engine::peekRect(obj);
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
            float dx = 0.f, dy = 0.f;
            if (predictTicks > 0.f && m_haveMotion && idx < m_vel.size()) {
                dx = m_vel[idx].x * predictTicks;
                dy = m_vel[idx].y * predictTicks;
            }
            float sx = 0.f, sy = 0.f;
            if (s_moverSweep > 0.f && m_haveMotion && idx < m_vel.size()) {
                sx = std::fabs(m_vel[idx].x) * s_moverSweep;
                sy = std::fabs(m_vel[idx].y) * s_moverSweep;
            }
            if (r.getMaxX() + dx + sx < std::min(x0, x1) || r.getMinX() + dx - sx > std::max(x0, x1)) continue;
            out.emplace_back(std::min(r.getMinY(), r.getMinY() + dy) - sy, std::max(r.getMaxY(), r.getMaxY() + dy) + sy);
        }
    }
}

bool Corridor::hasDynamic(float x0, float x1) const {
    if (m_dynamic.empty()) return false;
    int b0 = (int) std::floor((std::min(x0, x1) - kDynamicRadius) / kDynamicBucket);
    int b1 = (int) std::floor((std::max(x0, x1) + kDynamicRadius) / kDynamicBucket);
    for (int b = b0; b <= b1; b++) {
        auto it = m_dynamicIndex.find(b);
        if (it != m_dynamicIndex.end() && !it->second.empty()) return true;
    }
    return false;
}

bool Corridor::hasPortal(float x0, float x1) const {
    if (m_portals.empty()) return false;
    float a = std::min(x0, x1), b = std::max(x0, x1);
    auto it = std::lower_bound(m_portals.begin(), m_portals.end(), a - 15.f);
    return it != m_portals.end() && *it <= b + 15.f;
}

bool Corridor::hasContent(float x0, float x1) const {
    int c0 = (int) std::floor(std::min(x0, x1) / kColumnWidth);
    int c1 = (int) std::floor(std::max(x0, x1) / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = m_columns.find(c);
        if (it != m_columns.end() && !it->second.empty()) return true;
    }
    if (m_movingBlocking.empty()) return false;
    int b0 = (int) std::floor(std::min(x0, x1) / kDynamicBucket);
    int b1 = (int) std::floor(std::max(x0, x1) / kDynamicBucket);
    for (int b = b0; b <= b1; b++) {
        auto it = m_dynamicIndex.find(b);
        if (it == m_dynamicIndex.end()) continue;
        for (auto idx : it->second) {
            auto obj = m_dynamic[idx];
            if (currentlyOff(obj)) continue;
            auto const& r = engine::peekRect(obj);
            if (r.getMaxX() >= std::min(x0, x1) && r.getMinX() <= std::max(x0, x1)) return true;
        }
    }
    return false;
}

bool Corridor::contentBottom(float x0, float x1, float& bot) const {
    bool any = false;
    bot = 1e30f;
    int c0 = (int) std::floor(x0 / kColumnWidth);
    int c1 = (int) std::floor(x1 / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = m_columns.find(c);
        if (it == m_columns.end() || it->second.empty()) continue;
        bot = std::min(bot, it->second.front().lo);
        any = true;
    }
    for (auto idx : m_movingBlocking) {
        auto obj = m_dynamic[idx];
        if (currentlyOff(obj)) continue;
        auto const& r = engine::peekRect(obj);
        if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
        if (r.getMaxX() < x0 || r.getMinX() > x1) continue;
        bot = std::min(bot, r.getMinY());
        any = true;
    }
    return any;
}

bool Corridor::contentTop(float x0, float x1, float& top) const {
    bool any = false;
    top = -1e30f;
    int c0 = (int) std::floor(x0 / kColumnWidth);
    int c1 = (int) std::floor(x1 / kColumnWidth);
    for (int c = c0; c <= c1; c++) {
        auto it = m_columns.find(c);
        if (it == m_columns.end() || it->second.empty()) continue;
        top = std::max(top, it->second.back().hi);
        any = true;
    }
    for (auto idx : m_movingBlocking) {
        auto obj = m_dynamic[idx];
        if (currentlyOff(obj)) continue;
        auto const& r = engine::peekRect(obj);
        if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
        if (r.getMaxX() < x0 || r.getMinX() > x1) continue;
        top = std::max(top, r.getMaxY());
        any = true;
    }
    return any;
}

bool Corridor::freeBand(float x0, float x1, float y, float& bottom, float& top, float predictTicks) const {
    bottom = m_groundY;
    top = 1e9f;
    std::vector<Interval> gaps;
    collectGaps(x0, x1, predictTicks, gaps);
    if (gaps.empty()) return false;
    Interval best = gaps.back();
    float bestDist = 1e30f;
    for (auto const& g : gaps) {
        float d = y < g.lo ? g.lo - y : y > g.hi ? y - g.hi : 0.f;
        if (d < bestDist) { bestDist = d; best = g; }
    }
    bottom = best.lo;
    top = best.hi;
    return true;
}

bool Corridor::freeBandFollowing(float x0, float x1, float curLo, float curHi, float y, float& bottom, float& top,
                                 float predictTicks) const {
    bottom = m_groundY;
    top = 1e9f;
    std::vector<Interval> gaps;
    collectGaps(x0, x1, predictTicks, gaps);
    if (gaps.empty()) return false;
    for (auto const& g : gaps) {
        if (y >= g.lo && y <= g.hi) {
            bottom = g.lo;
            top = g.hi;
            return true;
        }
    }
    Interval best{0.f, 0.f};
    float bestOverlap = 0.f;
    for (auto const& g : gaps) {
        float ov = std::min(g.hi, curHi) - std::max(g.lo, curLo);
        if (ov > bestOverlap) { bestOverlap = ov; best = g; }
    }
    if (bestOverlap <= 0.f) {
        float bestDist = 1e30f;
        best = gaps.back();
        for (auto const& g : gaps) {
            float d = y < g.lo ? g.lo - y : y > g.hi ? y - g.hi : 0.f;
            if (d < bestDist) { bestDist = d; best = g; }
        }
    }
    bottom = best.lo;
    top = best.hi;
    return true;
}

int Corridor::routeTarget(float x, float y, float speed, float dir, float slope, float halfHeight,
                          float target[kRouteWindows + 1]) const {
    for (int k = 0; k <= kRouteWindows; k++) target[k] = y;
    if (empty()) return 0;
    float perTick = std::max(0.01f, speed / 240.f);
    float step = std::max(kRouteStep, perTick * 15.f);
    struct Slice { float lo, hi; int prev; float narrowest; };
    std::vector<Slice> layers[kRouteWindows + 1];
    layers[0].push_back({y, y, -1, 1e9f});
    std::vector<Interval> gaps;
    int reach = 0;
    for (int i = 1; i <= kRouteWindows; i++) {
        float xc = x + dir * step * (float) i;
        float predictTicks = step * (float) i / perTick;
        collectGaps(xc - step * 0.5f, xc + step * 0.5f, predictTicks, gaps);
        if (gaps.empty()) gaps.push_back({m_groundY, 1e9f});
        auto& layer = layers[i];
        for (auto const& g : gaps) {
            float glo = g.lo + halfHeight;
            float ghi = g.hi > 1e8f ? 1e9f : g.hi - halfHeight;
            if (ghi < glo) continue;
            Slice s{1e9f, -1e9f, -1, -1.f};
            for (int p = 0; p < (int) layers[i - 1].size(); p++) {
                auto const& prev = layers[i - 1][p];
                float lo = std::max(glo, prev.lo - slope * step);
                float hi = std::min(ghi, prev.hi + slope * step);
                if (hi < lo) continue;
                s.lo = std::min(s.lo, lo);
                s.hi = std::max(s.hi, hi);
                float narrowest = std::min(prev.narrowest, hi - lo);
                if (narrowest > s.narrowest) { s.narrowest = narrowest; s.prev = p; }
            }
            if (s.prev >= 0) layer.push_back(s);
        }
        if (layer.empty()) break;
        reach = i;
    }
    if (reach == 0) return 0;
    int best = 0;
    for (int j = 1; j < (int) layers[reach].size(); j++)
        if (layers[reach][j].narrowest > layers[reach][best].narrowest) best = j;
    Slice chain[kRouteWindows + 1];
    for (int i = reach, j = best; i >= 0; i--) {
        chain[i] = layers[i][j];
        j = layers[i][j].prev;
    }
    for (int k = 1; k <= reach; k++) {
        float lo = chain[k].lo, hi = chain[k].hi;
        for (int m = k + 1; m <= std::min(reach, k + 2); m++) {
            float lo2 = std::max(lo, chain[m].lo), hi2 = std::min(hi, chain[m].hi);
            if (hi2 < lo2) break;
            lo = lo2; hi = hi2;
        }
        target[k] = hi > 1e8f ? std::max(y, lo + 37.f) : (lo + hi) * 0.5f;
    }
    for (int k = reach + 1; k <= kRouteWindows; k++) target[k] = target[reach];
    return reach;
}

float Corridor::targetY(float x, float y, float speed, Steer steer, float dir, float slope, float halfHeight) const {
    if (empty()) return y;
    float lead = std::max(60.f, speed * 0.3f);
    if (slope > 0.f) {
        float t[kRouteWindows + 1];
        int reach = routeTarget(x, y, speed, dir, slope, halfHeight, t);
        if (reach >= 1) {
            float step = std::max(kRouteStep, std::max(0.01f, speed / 240.f) * 15.f);
            auto at = [&](float dist) { return t[std::clamp((int) std::lround(dist / step), 1, reach)]; };
            float nearT = at(lead), midT = at(2.f * lead), farT = at(3.5f * lead);
            if (steer == Steer::Near) return nearT;
            if (steer == Steer::Far) return 0.20f * nearT + 0.30f * midT + 0.50f * farT;
            return 0.45f * nearT + 0.30f * midT + 0.25f * farT;
        }
    }
    float span = std::max(60.f, speed * 0.2f);
    float curLo = y - 1.f, curHi = y + 1.f;
    {
        float b, t;
        if (freeBand(x - 6.f, x + 6.f, y, b, t)) { curLo = b; curHi = t; }
    }
    float perTick = std::max(0.01f, speed / 240.f);
    float aLo = curLo, aHi = curHi;
    auto stepBand = [&](float x0, float& out) {
        float bottom, top;
        float w0 = std::min(x0, x0 + dir * span), w1 = std::max(x0, x0 + dir * span);
        float predictTicks = std::max(0.f, std::fabs(x0 + dir * span * 0.5f - x) / perTick);
        if (!freeBandFollowing(w0, w1, aLo, aHi, y, bottom, top, predictTicks)) return false;
        aLo = bottom; aHi = top;
        out = top > 1e8f ? std::max(y, bottom + 45.f) : (bottom + top) * 0.5f;
        return true;
    };
    float nearT = y, midT = y, farT = y;
    bool hasNear = stepBand(x + dir * lead, nearT);
    bool wide = steer != Steer::Near;
    bool hasMid = wide && stepBand(x + dir * 2.f * lead, midT);
    bool hasFar = wide && stepBand(x + dir * 3.5f * lead, farT);
    bool leadFar = steer == Steer::Far;
    if (hasNear && hasMid && hasFar) {
        return leadFar ? 0.20f * nearT + 0.30f * midT + 0.50f * farT
                   : 0.45f * nearT + 0.30f * midT + 0.25f * farT;
    }
    if (hasNear && hasMid) return leadFar ? 0.35f * nearT + 0.65f * midT : 0.6f * nearT + 0.4f * midT;
    if (hasNear) return nearT;
    if (hasMid) return midT;
    return y;
}
int Corridor::orbKindOf(GameObjectType type) {
    switch (type) {
        case GameObjectType::GravityRing:    return BallPlan::OrbBlue;
        case GameObjectType::YellowJumpRing: return BallPlan::OrbYellow;
        case GameObjectType::PinkJumpRing:   return BallPlan::OrbPink;
        case GameObjectType::RedJumpRing:    return BallPlan::OrbRed;
        case GameObjectType::GreenRing:      return BallPlan::OrbGreen;
        case GameObjectType::DropRing:       return BallPlan::OrbDrop;
        default: return -1;
    }
}

bool Corridor::orbSpans(float x0, float x1, float* lo, float* hi) const {
    for (int k = 0; k < BallPlan::OrbCount; k++) { lo[k] = 1.f; hi[k] = 0.f; }
    if (m_orbs.empty()) return false;
    float a = std::min(x0, x1), b = std::max(x0, x1);
    bool any = false;
    for (auto const& o : m_orbs) {
        if (o.x0 > b) break;
        if (o.x1 < a || o.kind < 0 || o.kind >= BallPlan::OrbCount) continue;
        if (lo[o.kind] > hi[o.kind]) { lo[o.kind] = o.y0; hi[o.kind] = o.y1; }
        else { lo[o.kind] = std::min(lo[o.kind], o.y0); hi[o.kind] = std::max(hi[o.kind], o.y1); }
        any = true;
    }
    return any;
}

}
