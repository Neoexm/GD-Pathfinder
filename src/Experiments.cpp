#include "Experiments.hpp"

#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/utils/file.hpp>
#include <chrono>
#include <map>
#include <unordered_set>
#include <cmath>
#include <cstring>
#include <vector>

#include "Engine.hpp"
#include "Layout.hpp"
#include "Profile.hpp"
#include "Snapshot.hpp"

using namespace geode::prelude;

namespace gdpf {

namespace {
    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    struct Sample {
        float x = 0, y = 0;
        double vy = 0;
        float rot = 0;
        bool ground = false;
    };

    Sample sample(PlayLayer* pl) {
        Sample s;
        auto p = pl->m_player1;
        if (!p) return s;
        s.x = p->getPositionX();
        s.y = p->getPositionY();
        s.vy = p->m_yVelocity;
        s.rot = p->getRotation();
        s.ground = p->m_isOnGround;
        return s;
    }

    bool same(Sample const& a, Sample const& b) {
        return a.x == b.x && a.y == b.y && a.vy == b.vy && a.ground == b.ground;
    }

    size_t cursorFor(InputList const& inputs, int tick) {
        size_t c = 0;
        while (c < inputs.size() && inputs[c].tick < tick) c++;
        return c;
    }

    struct WorldObj { float x, y, rot; double px, py; bool off; bool act; };

    std::vector<WorldObj> sampleWorld(std::vector<GameObject*> const& dyn) {
        std::vector<WorldObj> w;
        w.reserve(dyn.size());
        auto pl = PlayLayer::get();
        for (auto obj : dyn)
            w.push_back({obj->getPositionX(), obj->getPositionY(), obj->getRotation(),
                         obj->m_positionX + obj->m_positionXOffset, obj->m_positionY + obj->m_positionYOffset,
                         obj->m_isGroupDisabled || obj->m_isDisabled,
                         pl && pl->m_player1 ? obj->hasBeenActivatedByPlayer(pl->m_player1) : false});
        return w;
    }

    int compareWorld(std::vector<GameObject*> const& dyn, std::vector<WorldObj> const& ref, std::vector<WorldObj> const& got, char const* what, int tick) {
        int bad = 0;
        for (size_t i = 0; i < dyn.size() && i < ref.size() && i < got.size(); i++) {
            auto const& a = ref[i];
            auto const& b = got[i];
            if (std::fabs(a.rot - b.rot) < 1e-3f && a.off == b.off && a.act == b.act && std::fabs(a.px - b.px) < 1e-4 && std::fabs(a.py - b.py) < 1e-4) continue;
            if (a.off && b.off && a.act == b.act) continue;
            if (++bad <= 3) {
                log::warn("[snaptest] {} world mismatch at tick {}: obj {} uid {} ref ({:.3f},{:.3f} real {:.3f},{:.3f} rot {:.3f}{}{}) got ({:.3f},{:.3f} real {:.3f},{:.3f} rot {:.3f}{}{})",
                    what, tick, dyn[i]->m_objectID, dyn[i]->m_uniqueID, a.x, a.y, a.px, a.py, a.rot, a.off ? " off" : "", a.act ? " activated" : "",
                    b.x, b.y, b.px, b.py, b.rot, b.off ? " off" : "", b.act ? " activated" : "");
            }
        }
        return bad;
    }

    class SnapshotTest final : public Experiment {
    public:
        SnapshotTest(InputList inputs, int interval) : m_inputs(std::move(inputs)), m_interval(std::max(1, interval)) {}

        void start(PlayLayer* pl) override {
            m_phase = Phase::Reference;
            m_cursor = 0;
            m_ref.clear();
            m_refPlayer.clear();
            m_snaps.clear();
            m_results.clear();
            m_refWorld.clear();
            m_dynamic = engine::collectSnapshotObjects(pl);
            engine::setDynamicObjects(m_dynamic);
            engine::resetToStart(pl);
            log::info("[snaptest] {} dynamic objects tracked", m_dynamic.size());
            m_started = now();
        }

        bool slice(PlayLayer* pl, double budget) override {
            double t0 = now();
            auto& st = engine::state();
            while (now() - t0 < budget) {
                if (m_phase == Phase::Reference) {
                    m_ref.push_back(sample(pl));
                    { engine::PlayerSnap ps; ps.capture(pl->m_player1); m_refPlayer.push_back(std::move(ps)); }
                    if (st.tick % m_interval == 0) {
                        if (auto s = engine::takeSnapshot(pl)) m_snaps.push_back(s);
                        m_refWorld[st.tick] = sampleWorld(m_dynamic);
                    }
                    engine::applyDueInputs(pl, m_inputs, m_cursor);
                    engine::step(pl);
                    if (st.diedThisStep || st.reachedEndThisStep || st.tick > 240 * 600) {
                        m_refEnd = st.tick;
                        m_refDied = st.diedThisStep;
                        log::info("[snaptest] reference run: {} ticks, died={} end={} snapshots={}", st.tick, st.diedThisStep, st.reachedEndThisStep, m_snaps.size());
                        m_phase = Phase::Restore;
                        m_index = 0;
                        if (!beginRestore(pl)) return finish();
                    }
                } else {
                    if (st.tick > m_cur.snapTick && st.tick % m_interval == 0) {
                        auto it = m_refWorld.find(st.tick);
                        if (it != m_refWorld.end()) {
                            int bad = compareWorld(m_dynamic, it->second, sampleWorld(m_dynamic), "later", st.tick);
                            if (bad > 0 && m_cur.firstWorldMismatch < 0) m_cur.firstWorldMismatch = st.tick;
                            m_cur.worldMismatches += bad;
                        }
                    }
                    if (st.tick < (int) m_ref.size()) {
                        auto s = sample(pl);
                        auto const& r = m_ref[st.tick];
                        if (!same(s, r)) {
                            if (m_cur.firstMismatch < 0) m_cur.firstMismatch = st.tick;
                            m_cur.mismatches++;
                            if (m_cur.mismatches <= 3) {
                                engine::PlayerSnap cur; cur.capture(pl->m_player1);
                                log::warn("[snaptest] snap@{} mismatch at tick {}: ref ({:.4f},{:.4f} vy {:.5f} g{}) got ({:.4f},{:.4f} vy {:.5f} g{}); differing player fields: {}",
                                    m_cur.snapTick, st.tick, r.x, r.y, r.vy, r.ground, s.x, s.y, s.vy, s.ground,
                                    st.tick < (int) m_refPlayer.size() ? cur.diff(m_refPlayer[st.tick]) : std::string("?"));
                            }
                        } else if (st.tick < (int) m_refPlayer.size() && m_cur.mismatches == 0 && m_cur.fieldDiffs < 3) {
                            engine::PlayerSnap cur; cur.capture(pl->m_player1);
                            auto d = cur.diff(m_refPlayer[st.tick]);
                            if (!d.empty()) {
                                m_cur.fieldDiffs++;
                                std::string detail;
                                if (d.find("m_touchedRings") != std::string::npos) {
                                    auto show = [&](std::unordered_set<int> const& set) {
                                        std::string o = "{";
                                        for (int id : set) {
                                            o += fmt::format(" {}", id);
                                            for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
                                                if (obj && obj->m_uniqueID == id) {
                                                    auto raw = reinterpret_cast<unsigned char const*>(obj);
                                                    o += fmt::format("(obj {} activated={} poweredOn={} groupDisabled={} b978={} b1460={} b1461={} activatedByPlayer={} at {:.0f},{:.0f})",
                                                        obj->m_objectID, obj->m_isActivated, static_cast<EnhancedGameObject*>(obj)->m_poweredOn,
                                                        obj->m_isGroupDisabled, raw[978], raw[1460], raw[1461],
                                                        obj->hasBeenActivatedByPlayer(pl->m_player1), obj->getPositionX(), obj->getPositionY());
                                                    break;
                                                }
                                            }
                                        }
                                        return o + " }";
                                    };
                                    detail = fmt::format(" touchedRings ref {} got {}", show(m_refPlayer[st.tick].m_touchedRings), show(cur.m_touchedRings));
                                }
                                log::warn("[snaptest] snap@{} tick {}: positions agree but player fields differ: {}{}", m_cur.snapTick, st.tick, d, detail);
                            }
                        }
                    }
                    engine::applyDueInputs(pl, m_inputs, m_cursor);
                    engine::step(pl);
                    bool ended = st.diedThisStep || st.reachedEndThisStep || st.tick >= m_refEnd;
                    if (ended) {
                        m_cur.endTick = st.tick;
                        m_cur.endMatches = (st.tick == m_refEnd) && (st.diedThisStep == m_refDied);
                        m_results.push_back(m_cur);
                        log::info("[snaptest] snap@{} -> end {} (ref {}) mismatches={} first={} worldMismatches={} firstWorld={} restoreWorld={}",
                            m_cur.snapTick, st.tick, m_refEnd, m_cur.mismatches, m_cur.firstMismatch, m_cur.worldMismatches, m_cur.firstWorldMismatch, m_cur.restoreWorldMismatches);
                        m_index++;
                        if (!beginRestore(pl)) return finish();
                    }
                }
            }
            return false;
        }

        std::string summary() const override { return m_summary; }

    private:
        enum class Phase { Reference, Restore };
        struct Result { int snapTick = 0; int endTick = 0; int mismatches = 0; int firstMismatch = -1; bool endMatches = false; bool restoreOk = true;
                        int worldMismatches = 0; int firstWorldMismatch = -1; int restoreWorldMismatches = 0; int fieldDiffs = 0; };

        bool beginRestore(PlayLayer* pl) {
            if (m_index >= m_snaps.size()) return false;
            auto const& snap = *m_snaps[m_index];
            m_cur = Result{};
            m_cur.snapTick = snap.tick;
            m_cur.restoreOk = engine::restoreSnapshot(pl, snap);
            m_cursor = cursorFor(m_inputs, snap.tick);
            auto it = m_refWorld.find(snap.tick);
            if (it != m_refWorld.end()) m_cur.restoreWorldMismatches = compareWorld(m_dynamic, it->second, sampleWorld(m_dynamic), "restore", snap.tick);
            return true;
        }

        bool finish() {
            int bad = 0;
            for (auto const& r : m_results) if (r.mismatches > 0 || !r.endMatches || !r.restoreOk || r.worldMismatches > 0 || r.restoreWorldMismatches > 0) bad++;
            auto const& ss = engine::snapshotStats();
            m_passed = bad == 0 && !m_results.empty();
            m_summary = fmt::format("snapshot-test {}: {} snapshots, {} bad, ref end {} | take avg {:.3f}ms restore avg {:.3f}ms restoreMismatch={} | elapsed {:.1f}s",
                m_passed ? "pass" : "fail", m_results.size(), bad, m_refEnd,
                ss.taken ? ss.takeSeconds / ss.taken * 1000.0 : 0.0,
                ss.restored ? ss.restoreSeconds / ss.restored * 1000.0 : 0.0,
                ss.restoreMismatches, now() - m_started);
            for (auto const& r : m_results) {
                if (r.mismatches > 0 || !r.endMatches || !r.restoreOk || r.worldMismatches > 0 || r.restoreWorldMismatches > 0)
                    m_summary += fmt::format(" | snap@{}: end {} mism {} first {} restoreOk {} world {} (first {}, at restore {})",
                        r.snapTick, r.endTick, r.mismatches, r.firstMismatch, r.restoreOk, r.worldMismatches, r.firstWorldMismatch, r.restoreWorldMismatches);
            }
            return true;
        }

        InputList m_inputs;
        int m_interval;
        Phase m_phase = Phase::Reference;
        size_t m_cursor = 0;
        std::vector<Sample> m_ref;
        std::vector<engine::PlayerSnap> m_refPlayer;
        std::vector<GameObject*> m_dynamic;
        std::map<int, std::vector<WorldObj>> m_refWorld;
        std::vector<engine::SnapshotPtr> m_snaps;
        std::vector<Result> m_results;
        Result m_cur;
        size_t m_index = 0;
        int m_refEnd = 0;
        bool m_refDied = false;
        std::string m_summary;
        double m_started = 0;
    };

    class SpeedTest final : public Experiment {
    public:
        explicit SpeedTest(InputList inputs) : m_inputs(std::move(inputs)) {}

        void start(PlayLayer* pl) override {
            m_cursor = 0;
            m_steps = 0;
            m_stepSeconds = 0;
            m_started = now();
            m_phase = 0;
            engine::setDynamicObjects(engine::collectSnapshotObjects(pl));
            engine::resetToStart(pl);
            log::info("[speedtest] {} objects tracked", engine::dynamicObjects().size());
            profile::reset();
            profile::setEnabled(true);
        }

        bool slice(PlayLayer* pl, double budget) override {
            double t0 = now();
            auto& st = engine::state();
            while (now() - t0 < budget) {
                if (m_phase == 0) {
                    double a = now();
                    engine::applyDueInputs(pl, m_inputs, m_cursor);
                    engine::step(pl);
                    m_stepSeconds += now() - a;
                    m_steps++;
                    if (st.diedThisStep || st.reachedEndThisStep || st.tick >= 12000) {
                        log::info("[speedtest] stepping profile:{}", profile::report());
                        profile::reset();
                        m_phase = 1;
                        m_iter = 0;
                    }
                } else if (m_phase == 1) {
                    auto s = engine::takeSnapshot(pl);
                    engine::restoreSnapshot(pl, *s);
                    if (++m_iter >= 50) {
                        log::info("[speedtest] restore profile:{}", profile::report());
                        profile::setEnabled(false);
                        auto const& ss = engine::snapshotStats();
                        m_passed = true;
                        m_summary = fmt::format("speed-test: {} steps in {:.3f}s = {:.0f} steps/s ({:.3f} ms/step) | snapshot avg {:.3f} ms | restore avg {:.3f} ms | mismatches {} | elapsed {:.1f}s",
                            m_steps, m_stepSeconds, m_steps / std::max(1e-9, m_stepSeconds), m_stepSeconds / std::max<uint64_t>(1, m_steps) * 1000.0,
                            ss.taken ? ss.takeSeconds / ss.taken * 1000.0 : 0.0,
                            ss.restored ? ss.restoreSeconds / ss.restored * 1000.0 : 0.0,
                            ss.restoreMismatches, now() - m_started);
                        return true;
                    }
                }
            }
            return false;
        }

        std::string summary() const override { return m_summary; }

    private:
        InputList m_inputs;
        size_t m_cursor = 0;
        uint64_t m_steps = 0;
        double m_stepSeconds = 0;
        double m_started = 0;
        int m_phase = 0;
        int m_iter = 0;
        std::string m_summary;
    };
}

std::unique_ptr<Experiment> makeSnapshotTest(InputList inputs, int interval) {
    return std::make_unique<SnapshotTest>(std::move(inputs), interval);
}

std::unique_ptr<Experiment> makeSpeedTest(InputList inputs) {
    return std::make_unique<SpeedTest>(std::move(inputs));
}

namespace {
    class ObjectDump final : public Experiment {
    public:
        explicit ObjectDump(std::string key) : m_key(std::move(key)) {}

        void start(PlayLayer* pl) override {
            auto dir = Mod::get()->getSaveDir() / "dumps";
            (void) file::createDirectoryAll(dir);
            auto path = dir / (m_key + ".txt");
            std::string out = "# uid id type x y rot scaleX scaleY rect(minX maxX minY maxY) flags groups target\n";
            int n = 0;
            for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
                if (!obj || obj->m_objectType == GameObjectType::Decoration) continue;
                if (obj->m_isDecoration && !obj->m_isTrigger) continue;
                auto const& r = engine::peekRect(obj);
                std::string flags;
                if (obj->m_isTrigger) {
                    auto eff = static_cast<EffectGameObject*>(obj);
                    flags += eff->m_isTouchTriggered ? "touch," : eff->m_isSpawnTriggered ? "spawn," : "pos,";
                    if (eff->m_isMultiTriggered) flags += "multi,";
                }
                if (obj->m_isPassable) flags += "passable,";
                if (obj->m_isNoTouch) flags += "notouch,";
                if (obj->m_isGroupDisabled || obj->m_isDisabled) flags += "off,";
                if (obj->m_hasExtendedCollision) flags += "extcol,";
                std::string groups;
                if (obj->m_groups) for (int i = 0; i < std::min<int>(obj->m_groupCount, 10); i++) groups += fmt::format("{}{}", i ? "." : "", (*obj->m_groups)[i]);
                int target = obj->m_isTrigger ? static_cast<EffectGameObject*>(obj)->m_targetGroupID : 0;
                out += fmt::format("{} {} {} {:.2f} {:.2f} {:.1f} {:.3f} {:.3f} {:.2f} {:.2f} {:.2f} {:.2f} {} {} {}\n",
                    obj->m_uniqueID, obj->m_objectID, (int) obj->m_objectType, obj->getPositionX(), obj->getPositionY(),
                    obj->getRotation(), obj->m_scaleX, obj->m_scaleY, r.getMinX(), r.getMaxX(), r.getMinY(), r.getMaxY(),
                    flags.empty() ? "-" : flags, groups.empty() ? "-" : groups, target);
                n++;
            }
            auto res = file::writeString(path, out);
            m_passed = res.isOk();
            m_summary = fmt::format("object dump: {} objects -> {}{}", n, path.string(), res.isOk() ? "" : " (write failed: " + res.unwrapErr() + ")");
        }
        bool slice(PlayLayer*, double) override { return true; }
        std::string summary() const override { return m_summary; }
    private:
        std::string m_key;
        std::string m_summary;
    };
}

namespace {
    struct Region { std::string name; char const* cls; uintptr_t base; std::vector<unsigned char> bytes; };

    inline void dumpRegions(PlayLayer* pl, float diffX, std::vector<Region>& out) {
        out.clear();
        auto add = [&](std::string name, char const* cls, void const* base, size_t size) {
            Region r;
            r.name = std::move(name);
            r.cls = cls;
            r.base = reinterpret_cast<uintptr_t>(base);
            r.bytes.assign(reinterpret_cast<unsigned char const*>(base), reinterpret_cast<unsigned char const*>(base) + size);
            out.push_back(std::move(r));
        };
        add("layer", "PlayLayer", pl, sizeof(PlayLayer));
        if (pl->m_player1) add("player1", "PlayerObject", pl->m_player1, sizeof(PlayerObject));
        if (pl->m_effectManager) add("effectManager", "GJEffectManager", pl->m_effectManager, sizeof(GJEffectManager));
        int n = 0;
        auto const& dyn = engine::dynamicObjects();
        for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!obj || obj->m_objectType == GameObjectType::Decoration) continue;
            if (std::fabs(obj->getPositionX() - diffX) > 60.f) continue;
            if (++n > 40) break;
            char const* cls = "GameObject";
            size_t size = sizeof(GameObject);
            if (typeinfo_cast<EffectGameObject*>(obj)) { cls = "EffectGameObject"; size = sizeof(EffectGameObject); }
            else if (typeinfo_cast<EnhancedGameObject*>(obj)) { cls = "EnhancedGameObject"; size = sizeof(EnhancedGameObject); }
            add(fmt::format("obj{}uid{}({:.0f},{:.0f}){}", obj->m_objectID, obj->m_uniqueID, obj->getPositionX(), obj->getPositionY(),
                std::find(dyn.begin(), dyn.end(), obj) != dyn.end() ? "T" : ""), cls, obj, size);
        }
    }

    inline int diffRegions(std::vector<Region> const& a, std::vector<Region> const& b, char const* when,
                           char const* aName, char const* bName) {
        int total = 0;
        for (size_t i = 0; i < a.size(); i++) {
            auto const& ra = a[i];
            auto it = std::find_if(b.begin(), b.end(), [&](Region const& r) { return r.name == ra.name; });
            if (it == b.end()) {
                log::warn("[statediff] {} {}: only on the {} side", when, ra.name, aName);
                continue;
            }
            auto const& rb = *it;
            if (ra.bytes.size() != rb.bytes.size()) {
                log::warn("[statediff] {} {}: region sizes differ ({} vs {})", when, ra.name, ra.bytes.size(), rb.bytes.size());
                continue;
            }
            size_t n = ra.bytes.size();
            int ranges = 0;
            for (size_t off = 0; off < n;) {
                if (ra.bytes[off] == rb.bytes[off]) { off++; continue; }
                size_t end = off;
                while (end < n && end - off < 64 && (ra.bytes[end] != rb.bytes[end] || (end + 1 < n && ra.bytes[end + 1] != rb.bytes[end + 1]))) end++;
                if (end == off) end = off + 1;
                total++;
                if (++ranges <= 40) {
                    size_t w0 = off & ~(size_t) 7, w1 = std::min(n, ((end + 7) & ~(size_t) 7));
                    std::string ha, hb, na, nb;
                    for (size_t k = w0; k < w1; k++) { ha += fmt::format("{:02x}", ra.bytes[k]); hb += fmt::format("{:02x}", rb.bytes[k]); }
                    for (size_t k = w0; k + 4 <= w1; k += 4) {
                        int32_t ia, ib; float fa, fb;
                        std::memcpy(&ia, &ra.bytes[k], 4); std::memcpy(&ib, &rb.bytes[k], 4);
                        std::memcpy(&fa, &ra.bytes[k], 4); std::memcpy(&fb, &rb.bytes[k], 4);
                        na += fmt::format(" {}|{:.4g}", ia, fa);
                        nb += fmt::format(" {}|{:.4g}", ib, fb);
                    }
                    log::warn("[statediff] {} {} +{}..{} ({}): {} {} [{}] {} {} [{}]", when, ra.name, off, end,
                        layout::describe(ra.cls, off), aName, ha, na, bName, hb, nb);
                }
                off = end;
            }
            if (ranges > 40) log::warn("[statediff] {} {}: {} more differing ranges not shown", when, ra.name, ranges - 40);
        }
        return total;
    }

    class RestoreDiff final : public Experiment {
    public:
        RestoreDiff(InputList inputs, int snapTick, float diffX, int runTicks, int diffTick)
            : m_inputs(std::move(inputs)), m_snapTick(std::max(1, snapTick)), m_diffX(diffX), m_runTicks(std::max(1, runTicks)),
              m_diffTick(diffTick > 0 ? diffTick : std::max(1, snapTick) + 1) {}

        void start(PlayLayer* pl) override {
            m_phase = Phase::Reference;
            m_cursor = 0;
            m_ref.clear();
            m_dynamic = engine::collectSnapshotObjects(pl);
            engine::setDynamicObjects(m_dynamic);
            engine::resetToStart(pl);
            m_wasFast = engine::fastRestore();
            m_started = now();
            log::info("[restorediff] {} dynamic objects tracked; snapshot at tick {}, {} ticks after it compared, objects within 60 of x {:.0f} dumped",
                m_dynamic.size(), m_snapTick, m_runTicks, m_diffX);
            {
                static int const kObj[] = {40, 360, 628, 632, 636, 654, 744, 774, 848, 850, 872, 908, 924, 940, 978, 1004, 1005, 1012, 1016, 1024, 1052, 1216, 1232, 1240, 1248, 1256, 1299, 1302, 1360, 1364, 1372, 1380, 1384, 1420, 1433, 1441, 1444, 1457, 1458, 1459, 1460, 1461, 1792, 1796};
                static int const kPlayer[] = {1232, 2016, 2437, 2489, 2490, 2491, 2493, 2494, 2495, 2499, 2500, 2532, 2560, 2688, 2704, 2728, 2928, 2932, 2936, 2940, 2944, 2952, 2956, 2968};
                static int const kLayer[] = {408, 464, 828, 992, 1000, 1004, 1024, 1032, 1058, 1244, 1264, 1292, 1309, 1312, 2144, 2168, 2182, 2186, 3488, 3496, 3504, 3512, 4136, 12416, 12420, 12428, 12444, 12446, 12673, 12680, 12688, 12784, 12785, 12832, 12837, 12840, 12848, 12849, 12850, 12874, 12888, 12889, 12912, 12952, 12964, 12976, 13016, 13024, 13032, 13040, 13048, 13052, 13056, 13320, 13352, 13384, 13632, 13664, 13672, 13680, 13720, 13744, 13888, 13912, 13960, 13984, 13988, 14182, 14208, 14210, 14264, 14270, 14272, 14420, 14422, 14456, 14460, 14464, 14472, 14604, 14608, 14708, 14717, 14724, 14728, 14736, 14737, 14784, 14792, 14808, 14816, 14824, 14948, 14952};
                std::string o, q, l;
                for (int off : kObj) o += fmt::format(" {}={}", off, layout::describe("EffectGameObject", off));
                for (int off : kPlayer) q += fmt::format(" {}={}", off, layout::describe("PlayerObject", off));
                for (int off : kLayer) l += fmt::format(" {}={}", off, layout::describe("PlayLayer", off));
                log::info("[restorediff] layout EffectGameObject:{}", o);
                log::info("[restorediff] layout PlayerObject:{}", q);
                log::info("[restorediff] layout PlayLayer:{}", l);
            }
        }

        bool slice(PlayLayer* pl, double budget) override {
            double t0 = now();
            auto& st = engine::state();
            while (now() - t0 < budget) {
                if (m_phase == Phase::Reference) {
                    if (st.tick == m_snapTick && !m_snap) {
                        m_snap = engine::takeSnapshot(pl);
                        if (!m_snap) { m_summary = "could not take the snapshot"; return true; }
                    }
                    if (st.tick >= m_snapTick) m_ref.push_back(sample(pl));
                    engine::applyDueInputs(pl, m_inputs, m_cursor);
                    engine::step(pl);
                    if (st.diedThisStep || st.reachedEndThisStep || st.tick >= m_snapTick + m_runTicks) {
                        m_refEnd = st.tick;
                        if (!m_snap) { m_summary = fmt::format("the reference run ended at tick {} before the snapshot tick {}", st.tick, m_snapTick); return true; }
                        log::info("[restorediff] reference run recorded to tick {}", st.tick);
                        beginRestore(pl, false);
                    }
                } else {
                    int idx = st.tick - m_snapTick;
                    if (idx >= 0 && idx < (int) m_ref.size()) {
                        auto s = sample(pl);
                        if (!same(s, m_ref[idx]) && m_firstMismatch[m_phase == Phase::Fast] < 0) {
                            m_firstMismatch[m_phase == Phase::Fast] = st.tick;
                            log::warn("[restorediff] {} restore: first divergence from the reference at tick {}: ref ({:.4f},{:.4f} vy {:.5f} g{}) got ({:.4f},{:.4f} vy {:.5f} g{})",
                                m_phase == Phase::Fast ? "fast" : "exact", st.tick, m_ref[idx].x, m_ref[idx].y, m_ref[idx].vy, m_ref[idx].ground, s.x, s.y, s.vy, s.ground);
                        }
                    }
                    engine::applyDueInputs(pl, m_inputs, m_cursor);
                    engine::step(pl);
                    if (st.tick == m_diffTick) dumpRegions(pl, m_diffX, m_phase == Phase::Fast ? m_fastAfterStep : m_exactAfterStep);
                    if (st.diedThisStep || st.reachedEndThisStep || st.tick >= m_refEnd) {
                        m_end[m_phase == Phase::Fast] = st.tick;
                        if (m_phase == Phase::Exact) beginRestore(pl, true);
                        else return finish();
                    }
                }
            }
            return false;
        }

        std::string summary() const override { return m_summary; }

    private:
        enum class Phase { Reference, Exact, Fast };

        void beginRestore(PlayLayer* pl, bool fast) {
            m_phase = fast ? Phase::Fast : Phase::Exact;
            engine::setFastRestore(fast);
            bool ok = engine::restoreSnapshot(pl, *m_snap);
            m_cursor = cursorFor(m_inputs, m_snap->tick);
            log::info("[restorediff] {} restore of the tick-{} snapshot: {}", fast ? "fast" : "exact", m_snap->tick, ok ? "ok" : "mismatch");
            dumpRegions(pl, m_diffX, fast ? m_fast : m_exact);
        }

        bool finish() {
            engine::setFastRestore(m_wasFast);
            int d0 = diffRegions(m_exact, m_fast, "after restore", "exact", "fast");
            int d1 = diffRegions(m_exactAfterStep, m_fastAfterStep, fmt::format("at tick {}", m_diffTick).c_str(), "exact", "fast");
            m_passed = m_firstMismatch[1] < 0 && m_firstMismatch[0] < 0;
            m_summary = fmt::format("restore-diff: snapshot tick {} | exact: first divergence {} end {} | fast: first divergence {} end {} (ref end {}) | {} differing ranges after restore, {} at tick {} (see the log) | sizes layer {} player {} em {} object {} effect {} | elapsed {:.1f}s",
                m_snapTick, m_firstMismatch[0], m_end[0], m_firstMismatch[1], m_end[1], m_refEnd, d0, d1, m_diffTick,
                sizeof(PlayLayer), sizeof(PlayerObject), sizeof(GJEffectManager), sizeof(GameObject), sizeof(EffectGameObject), now() - m_started);
            return true;
        }

        InputList m_inputs;
        int m_snapTick;
        float m_diffX;
        int m_runTicks;
        int m_diffTick;
        Phase m_phase = Phase::Reference;
        size_t m_cursor = 0;
        std::vector<Sample> m_ref;
        std::vector<GameObject*> m_dynamic;
        engine::SnapshotPtr m_snap;
        int m_refEnd = 0;
        int m_firstMismatch[2] = {-1, -1};
        int m_end[2] = {0, 0};
        bool m_wasFast = false;
        std::vector<Region> m_exact, m_fast, m_exactAfterStep, m_fastAfterStep;
        std::string m_summary;
        double m_started = 0;
    };
}

std::unique_ptr<Experiment> makeRestoreDiff(InputList inputs, int snapTick, float diffX, int runTicks, int diffTick) {
    return std::make_unique<RestoreDiff>(std::move(inputs), snapTick, diffX, runTicks, diffTick);
}

namespace {
    class ResetDrift final : public Experiment {
    public:
        ResetDrift(InputList inputs, int cycles, float diffX, int runTicks, int churn, int traceTicks)
            : m_inputs(std::move(inputs)), m_cycles(std::max(2, cycles)), m_diffX(diffX),
              m_runTicks(std::max(2, runTicks)), m_churn(churn) { m_traceTicks = traceTicks; }

        void start(PlayLayer* pl) override {
            m_started = now();
            m_dynamic = engine::collectSnapshotObjects(pl);
            engine::setDynamicObjects(m_dynamic);
            engine::resetToStart(pl);
            log::info("[drift] {} tracked objects; {} cycles of {} ticks with {} restores each, objects within 60 of x {:.0f} dumped",
                m_dynamic.size(), m_cycles, m_runTicks, m_churn, m_diffX);
            beginCycle(pl);
        }

        bool slice(PlayLayer* pl, double budget) override {
            double t0 = now();
            auto& st = engine::state();
            while (now() - t0 < budget) {
                st.traceActivations = m_traceTicks > 0 && st.tick < m_traceTicks;
                if (auto p = pl->m_player1) {
                    TrajPoint tp{engine::playerX(pl), engine::playerY(pl), p->m_yVelocity, p->m_playerSpeed,
                                 (uint32_t) ((p->m_isUpsideDown ? 1 : 0) | (p->m_isOnGround ? 2 : 0) | (p->m_isShip ? 4 : 0)
                                             | (p->m_isDart ? 8 : 0) | (p->m_isBird ? 16 : 0) | (p->m_isBall ? 32 : 0))};
                    if (m_cycle == 1) m_refTraj.push_back(tp);
                    else if (m_firstDiff < 0 && st.tick < (int) m_refTraj.size()) {
                        auto const& r = m_refTraj[st.tick];
                        if (r.x != tp.x || r.y != tp.y || r.vy != tp.vy || r.speed != tp.speed || r.flags != tp.flags) {
                            m_firstDiff = st.tick;
                            log::warn("[drift] cycle {} first leaves cycle 1 at tick {}: ({:.4f},{:.4f}) vy {:.5f} speed {:.3f} flags {:#x}"
                                      "  vs cycle 1 ({:.4f},{:.4f}) vy {:.5f} speed {:.3f} flags {:#x}",
                                m_cycle, st.tick, tp.x, tp.y, tp.vy, tp.speed, tp.flags, r.x, r.y, r.vy, r.speed, r.flags);
                        }
                    }
                }
                if (m_churn > 0 && st.tick == m_runTicks / 2) {
                    auto snap = engine::takeSnapshot(pl);
                    if (snap) {
                        for (int i = 0; i < m_churn; i++) {
                            engine::restoreSnapshot(pl, *snap);
                            for (int k = 0; k < 4; k++) engine::step(pl);
                        }
                        engine::restoreSnapshot(pl, *snap);
                        m_cursor = cursorFor(m_inputs, st.tick);
                    }
                }
                engine::applyDueInputs(pl, m_inputs, m_cursor);
                engine::step(pl);
                m_endX = engine::playerX(pl);
                if (st.diedThisStep || st.reachedEndThisStep || st.tick >= m_runTicks) {
                    m_endTick = st.tick;
                    m_died = st.diedThisStep;
                    m_results.push_back({m_cycle, m_endTick, m_endX, m_died, worldHash()});
                    log::info("[drift] cycle {}: ended tick {} at x {:.3f} died={} worldHash {:016x}",
                        m_cycle, m_endTick, m_endX, m_died, m_results.back().hash);
                    if (++m_cycle > m_cycles) return finish();
                    engine::resetToStart(pl);
                    beginCycle(pl);
                }
            }
            return false;
        }

        std::string summary() const override { return m_summary; }

    private:
        struct Result { int cycle; int endTick; float endX; bool died; uint64_t hash; };
        struct TrajPoint { float x, y; double vy; float speed; uint32_t flags; };

        void beginCycle(PlayLayer* pl) {
            m_cursor = 0;
            uint64_t h = worldHash();
            log::info("[drift] cycle {}: after reset, worldHash {:016x} player ({:.3f},{:.3f}) speed {:.3f}",
                m_cycle, h, engine::playerX(pl), engine::playerY(pl), pl->m_player1 ? pl->m_player1->m_playerSpeed : 0.f);
            if (m_traceTicks > 0) log::info("[drift] ---- cycle {} activation trace ({} ticks) ----", m_cycle, m_traceTicks);
            m_resetHashes.push_back(h);
            if (m_cycle == 1) dumpRegions(pl, m_diffX, m_first);
            if (m_cycle == 2) dumpRegions(pl, m_diffX, m_second);
            if (m_cycle == m_cycles) dumpRegions(pl, m_diffX, m_last);
        }

        uint64_t worldHash() const {
            uint64_t h = 0xcbf29ce484222325ull;
            auto mix = [&](uint64_t v) { h ^= v; h *= 0x100000001b3ull; };
            auto mixf = [&](double d) { uint64_t u; float f = (float) d; std::memcpy(&u, &f, 4); mix(u & 0xffffffffull); };
            for (auto o : m_dynamic) {
                if (!o) continue;
                mixf(o->m_positionX + o->m_positionXOffset);
                mixf(o->m_positionY + o->m_positionYOffset);
                mixf(o->getRotation());
                mixf(o->getScaleX());
                mixf(o->getScaleY());
                mix((uint64_t) (uint32_t) o->m_enabledGroupsCounter);
                mix((o->m_isGroupDisabled ? 1u : 0u) | (o->m_isDisabled ? 2u : 0u) | (o->m_isDisabled2 ? 4u : 0u));
            }
            if (auto pl = PlayLayer::get()) {
                if (auto p = pl->m_player1) {
                    mixf(p->getPositionX()); mixf(p->getPositionY()); mixf(p->m_yVelocity);
                    mixf(p->m_playerSpeed); mixf(p->m_gravityMod);
                    mix((p->m_isUpsideDown ? 1u : 0u) | (p->m_isOnGround ? 2u : 0u) | (p->m_isDart ? 4u : 0u)
                        | (p->m_isShip ? 8u : 0u) | (p->m_isBird ? 16u : 0u) | (p->m_isBall ? 32u : 0u));
                }
            }
            return h;
        }

        bool finish() {
            int badReset = 0, badEnd = 0;
            for (size_t i = 1; i < m_resetHashes.size(); i++) if (m_resetHashes[i] != m_resetHashes[0]) badReset++;
            for (size_t i = 1; i < m_results.size(); i++)
                if (m_results[i].endTick != m_results[0].endTick || m_results[i].hash != m_results[0].hash
                    || m_results[i].endX != m_results[0].endX) badEnd++;
            int ranges = 0;
            int ranges2 = 0;
            if (!m_first.empty() && !m_second.empty())
                ranges2 = diffRegions(m_first, m_second, "after reset (1 vs 2)", "cycle1", "cycle2");
            if (!m_first.empty() && !m_last.empty())
                ranges = diffRegions(m_first, m_last, "after reset", "cycle1", "cycleN");
            log::info("[drift] {} differing byte ranges cycle1 vs cycle2, {} cycle1 vs cycleN", ranges2, ranges);
            m_passed = badReset == 0 && badEnd == 0;
            m_summary = fmt::format("reset-drift {}: {} cycles | {} resets differ from the first, {} runs differ | first divergence from cycle 1 at tick {} | {} differing byte ranges between the first and last reset (see the log) | cycle1 end tick {} x {:.3f} | elapsed {:.1f}s",
                m_passed ? "pass" : "fail", m_results.size(), badReset, badEnd, m_firstDiff, ranges,
                m_results.empty() ? 0 : m_results[0].endTick, m_results.empty() ? 0.f : m_results[0].endX, now() - m_started);
            return true;
        }

        InputList m_inputs;
        int m_cycles;
        float m_diffX;
        int m_runTicks;
        int m_churn;
        int m_cycle = 1;
        size_t m_cursor = 0;
        int m_endTick = 0;
        float m_endX = 0.f;
        bool m_died = false;
        std::vector<GameObject*> m_dynamic;
        std::vector<uint64_t> m_resetHashes;
        std::vector<Result> m_results;
        std::vector<Region> m_first, m_second, m_last;
        std::vector<TrajPoint> m_refTraj;
        int m_firstDiff = -1;
        int m_traceTicks = 0;
        std::string m_summary;
        double m_started = 0;
    };
}

std::unique_ptr<Experiment> makeResetDrift(InputList inputs, int cycles, float diffX, int runTicks, int churn, int traceTicks) {
    return std::make_unique<ResetDrift>(std::move(inputs), cycles, diffX, runTicks, churn, traceTicks);
}

std::unique_ptr<Experiment> makeObjectDump(std::string levelKey) {
    return std::make_unique<ObjectDump>(std::move(levelKey));
}

}
