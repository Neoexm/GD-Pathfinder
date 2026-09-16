#include "Profile.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <vector>

using namespace geode::prelude;

namespace gdpf::profile {

static bool s_enabled = false;
static std::map<std::string, Entry> s_entries;

std::map<std::string, Entry>& entries() { return s_entries; }
bool enabled() { return s_enabled; }
void setEnabled(bool on) { s_enabled = on; }
void reset() { s_entries.clear(); }

std::string report() {
    std::vector<std::pair<std::string, Entry>> v(s_entries.begin(), s_entries.end());
    std::sort(v.begin(), v.end(), [](auto const& a, auto const& b) { return a.second.seconds > b.second.seconds; });
    std::string out;
    for (auto const& [name, e] : v) {
        out += fmt::format(" | {} x{} {:.2f}ms total {:.3f}ms avg", name, e.calls, e.seconds * 1000.0, e.calls ? e.seconds * 1000.0 / e.calls : 0.0);
    }
    return out;
}

}

using gdpf::profile::Scope;

#define GDPF_PROFILED(cls, ret, name, sig, call) \
    ret name sig { Scope scope_(#cls "::" #name); return cls::name call; }

class $modify(GDPFProfPlayLayer, PlayLayer) {
    GDPF_PROFILED(PlayLayer, void, resetLevel, (), ())
    GDPF_PROFILED(PlayLayer, void, loadFromCheckpoint, (CheckpointObject* cp), (cp))
    GDPF_PROFILED(PlayLayer, void, startMusic, (), ())
    GDPF_PROFILED(PlayLayer, void, prepareMusic, (bool a), (a))
    GDPF_PROFILED(PlayLayer, void, updateAttempts, (), ())
    GDPF_PROFILED(PlayLayer, void, spawnCircle, (), ())
    GDPF_PROFILED(PlayLayer, void, showHint, (), ())
    GDPF_PROFILED(PlayLayer, void, updateProgressbar, (), ())
    GDPF_PROFILED(PlayLayer, void, updateInfoLabel, (), ())
    GDPF_PROFILED(PlayLayer, void, resetLevelFromStart, (), ())
    GDPF_PROFILED(PlayLayer, void, updateVisibility, (float dt), (dt))
    GDPF_PROFILED(PlayLayer, void, postUpdate, (float dt), (dt))
};

class $modify(GDPFProfBaseLayer, GJBaseGameLayer) {
    GDPF_PROFILED(GJBaseGameLayer, void, resetLevelVariables, (), ())
    GDPF_PROFILED(GJBaseGameLayer, void, resetPlayer, (), ())
    GDPF_PROFILED(GJBaseGameLayer, void, updateCamera, (float dt), (dt))
    GDPF_PROFILED(GJBaseGameLayer, int, checkCollisions, (PlayerObject* p, float dt, bool d), (p, dt, d))
    GDPF_PROFILED(GJBaseGameLayer, void, processCommands, (float dt, bool a, bool b), (dt, a, b))
};

class $modify(GDPFProfEffectManager, GJEffectManager) {
    GDPF_PROFILED(GJEffectManager, void, reset, (), ())
};

class $modify(GDPFProfPlayer, PlayerObject) {
    GDPF_PROFILED(PlayerObject, void, resetObject, (), ())
    GDPF_PROFILED(PlayerObject, void, update, (float dt), (dt))
};

class $modify(GDPFProfFMOD, FMODAudioEngine) {
    GDPF_PROFILED(FMODAudioEngine, void, setMusicTimeMS, (unsigned int t, bool w, int id), (t, w, id))
    GDPF_PROFILED(FMODAudioEngine, void, playMusic, (gd::string p, bool l, float f, int c), (p, l, f, c))
    GDPF_PROFILED(FMODAudioEngine, void, stopAllMusic, (bool c), (c))
    GDPF_PROFILED(FMODAudioEngine, void, stopAllEffects, (), ())
};
