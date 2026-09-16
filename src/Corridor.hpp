#pragma once

#include <Geode/Geode.hpp>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdpf {

class Corridor {
public:
    static constexpr float kColumnWidth = 10.f;
    static constexpr float kDynamicBucket = 100.f;
    static constexpr float kDynamicRadius = 400.f;
    static float s_moverSweep;

    void build(PlayLayer* pl);
    bool empty() const { return m_columns.empty() && m_dynamic.empty(); }

    bool freeBand(float x0, float x1, float y, float& bottom, float& top, float predictTicks = 0.f) const;

    bool freeBandFollowing(float x0, float x1, float curLo, float curHi, float y, float& bottom, float& top,
                           float predictTicks = 0.f) const;

    enum class Steer {
        Near,
        Blend,
        Far,
    };

    float targetY(float x, float y, float speed, Steer steer, float dir = 1.f, float slope = 0.f, float halfHeight = 8.f) const;

    static constexpr float kRouteStep = 20.f;
    static constexpr int kRouteWindows = 24;
    int routeTarget(float x, float y, float speed, float dir, float slope, float halfHeight,
                    float target[kRouteWindows + 1]) const;

    bool contentTop(float x0, float x1, float& top) const;
    bool contentBottom(float x0, float x1, float& bot) const;

    bool roomAt(float x0, float x1, float y, float cap, float& below, float& above) const;

    bool boxHitsHazard(float x0, float x1, float y0, float y1) const;
    bool boxHitsHazardExact(float x0, float x1, float y0, float y1) const;
    void solidRectsAt(float x0, float x1, std::vector<std::pair<float, float>>& out) const;
    void gapsAt(float x0, float x1, float predictTicks, std::vector<std::pair<float, float>>& out) const;
    void blockedAt(float x0, float x1, float predictTicks, bool hazard, std::vector<std::pair<float, float>>& out) const;
    bool hasContent(float x0, float x1) const;
    bool hasDynamic(float x0, float x1) const;
    uint64_t dynamicIndexRebuilds() const { return m_indexRebuilds; }
    uint64_t movedSignature(float x0, float x1, float quant) const;
    struct LiveRect { int id; int type; float x0, x1, y0, y1; bool hazard, moved, off; };
    void liveRectsNear(float x0, float x1, std::vector<LiveRect>& out) const;
    bool hasPortal(float x0, float x1) const;
    bool boxHitsSolid(float x0, float x1, float y0, float y1) const;

    struct Item { float x, y; int id; };
    std::vector<Item> const& items() const { return m_items; }
    std::vector<Item> const& rings() const { return m_rings; }
    struct Orb { float x0, x1, y0, y1; int kind; };
    std::vector<Orb> const& orbs() const { return m_orbs; }
    static int orbKindOf(GameObjectType type);
    static void setDumpWindow(float lo, float hi);
    static void setGeoDump(int tickLo, int tickHi, float x0, float x1, std::string path);
    bool orbSpans(float x0, float x1, float* lo, float* hi) const;
    std::vector<Item> const& speedPortals() const { return m_speedPortals; }
    size_t portalCount() const { return m_portals.size(); }
    size_t objectCount() const { return m_objectCount; }
    size_t dynamicCount() const { return m_dynamic.size(); }
    std::vector<GameObject*> const& dynamicObjects() const { return m_dynamic; }

    void sampleMotion();
    void resetMotion();

private:
    struct Interval { float lo, hi; };
    void collectGaps(float x0, float x1, float predictTicks, std::vector<Interval>& gaps) const;
    std::map<int, std::vector<Interval>> m_columns;
    std::map<int, std::vector<Interval>> m_hazardColumns;
    std::map<int, std::vector<Interval>> m_solidColumns;
    static void addInterval(std::map<int, std::vector<Interval>>& cols, cocos2d::CCRect const& rect);
    static void mergeColumns(std::map<int, std::vector<Interval>>& cols);
    static bool columnsHit(std::map<int, std::vector<Interval>> const& cols, float x0, float x1, float y0, float y1);
    bool dynamicHit(float x0, float x1, float y0, float y1, bool hazard) const;
    std::vector<GameObject*> m_dynamic;
    std::map<int, std::vector<uint32_t>> m_dynamicIndex;
    std::vector<float> m_indexX;
    std::vector<cocos2d::CCPoint> m_basePos;
    uint64_t m_indexRebuilds = 0;
    void rebuildDynamicIndex();
    std::vector<uint32_t> m_movingBlocking;
    std::vector<cocos2d::CCPoint> m_prevPos;
    std::vector<cocos2d::CCPoint> m_vel;
    bool m_haveMotion = false;
    std::vector<float> m_portals;
    std::vector<Item> m_items;
    std::vector<Item> m_rings;
    std::vector<Orb> m_orbs;
    struct Rect4 { float x0, x1, y0, y1; };
    std::vector<Rect4> m_hazardRects;
    std::vector<Rect4> m_solidRects;
    std::unordered_map<int, std::vector<int>> m_hazardRectIndex;
    std::unordered_map<int, std::vector<int>> m_solidRectIndex;
    std::vector<Item> m_speedPortals;
    size_t m_objectCount = 0;
    float m_groundY = 90.f;

};

}
