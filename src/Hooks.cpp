#include <Geode/Geode.hpp>
#include <Geode/modify/AppDelegate.hpp>
#include <Geode/modify/CCActionManager.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cmath>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/TeleportPortalObject.hpp>

#include "Autorun.hpp"
#include "Bot.hpp"
#include "Engine.hpp"
#include <Geode/binding/SequenceTriggerGameObject.hpp>
#include <Geode/binding/RandTriggerGameObject.hpp>
#include <Geode/binding/ChanceTriggerGameObject.hpp>
#include <Geode/binding/CountTriggerGameObject.hpp>
#include <Geode/binding/ItemTriggerGameObject.hpp>
#include <Geode/binding/EnhancedTriggerObject.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <intrin.h>
#endif

using namespace geode::prelude;
using namespace gdpf;

namespace {
    std::string gameCallers(int maxCount) {
        static uintptr_t base = 0, end = 0;
        if (!base) {
            auto h = GetModuleHandleA(nullptr);
            if (!h) return "";
            base = reinterpret_cast<uintptr_t>(h);
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(h);
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(h) + dos->e_lfanew);
            end = base + nt->OptionalHeader.SizeOfImage;
        }
        auto sp = reinterpret_cast<uintptr_t*>(_AddressOfReturnAddress());
        auto top = reinterpret_cast<uintptr_t*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        std::string out;
        int n = 0;
        uintptr_t last = 0;
        for (auto p = sp; p < top && p < sp + 8192 && n < maxCount; p++) {
            uintptr_t v = *p;
            if (v >= base && v < end && v != last) {
                out += fmt::format(" +0x{:x}", v - base);
                last = v;
                n++;
            }
        }
        return out;
    }
}

#ifndef GDPF_GMD_PRIORITY
#define GDPF_GMD_PRIORITY (-1000000000)
#endif

class $modify(GDPFBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void) self.setHookPriority("GJBaseGameLayer::handleButton", Priority::First);
        (void) self.setHookPriority("GJBaseGameLayer::getModifiedDelta", GDPF_GMD_PRIORITY);
        (void) self.setHookPriority("GJBaseGameLayer::update", GDPF_GMD_PRIORITY);
    }

    void update(float dt) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            auto& st = engine::state();
            if (st.takeover && !st.internalStep) {
                Bot::get().onLayerUpdate(pl, dt);
                return;
            }
        }
        GJBaseGameLayer::update(dt);
    }

    double getModifiedDelta(float dt) {
        auto& st = engine::state();
        if (st.takeover && st.internalStep) {
            float raw = m_gameState.m_timeWarp;
            if (!(raw > 0.f)) raw = 1.f;
            raw = std::round(raw * 10000.f) / 10000.f;
            m_gameState.m_timeWarp = raw;
            double stepLen = (double) std::min(1.f, raw) * (1.0 / 240.0);
            double total = (double) engine::kStepDt + m_extraDelta;
            int steps = (int) std::round(total / stepLen);
            if (steps < 0) steps = 0;
            double out = steps * stepLen;
            m_extraDelta = total - out;
            if (auto expected = engine::eclipseExpectedTicks()) *expected = (uint32_t) std::max(1, steps);
            return out;
        }
        return GJBaseGameLayer::getModifiedDelta(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (Bot::get().inputBlocked()) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        engine::state().commandsThisStep++;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    static std::string describeUID(GJBaseGameLayer* layer, int uid) {
        if (!layer || !layer->m_objects) return "?";
        for (auto obj : CCArrayExt<GameObject*>(layer->m_objects)) {
            if (!obj || obj->m_uniqueID != uid) continue;
            char const* cls = "GameObject";
            if (typeinfo_cast<SequenceTriggerGameObject*>(obj)) cls = "SequenceTrigger";
            else if (typeinfo_cast<RandTriggerGameObject*>(obj)) cls = "RandTrigger";
            else if (typeinfo_cast<ChanceTriggerGameObject*>(obj)) cls = "ChanceTrigger";
            else if (typeinfo_cast<CountTriggerGameObject*>(obj)) cls = "CountTrigger";
            else if (typeinfo_cast<ItemTriggerGameObject*>(obj)) cls = "ItemTrigger";
            else if (typeinfo_cast<EnhancedTriggerObject*>(obj)) cls = "EnhancedTrigger";
            else if (typeinfo_cast<EffectGameObject*>(obj)) cls = "EffectGameObject";
            else if (typeinfo_cast<EnhancedGameObject*>(obj)) cls = "EnhancedGameObject";
            return fmt::format("id {} {} at ({:.0f},{:.0f})", obj->m_objectID, cls, obj->getPositionX(), obj->getPositionY());
        }
        return "uid not in m_objects";
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        auto& st = engine::state();
        if (st.traceActivations)
            log::info("[trace] tick {} spawnGroup {} delay {:.3f} ordered {} trigger {} [{}] control {} remaps {}",
                st.tick, group, delay, ordered, triggerID, describeUID(this, triggerID), controlID, remapKeys.size());
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void toggleGroup(int id, bool activate) {
        auto& st = engine::state();
        if (st.traceActivations) log::info("[trace] tick {} toggleGroup {} -> {}", st.tick, id, activate);
        GJBaseGameLayer::toggleGroup(id, activate);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (engine::state().takeover) engine::noteActivation(object, 1);
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (engine::state().takeover) engine::noteActivation(object, 2);
        GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        auto& st = engine::state();
        if (!st.takeover) { GJBaseGameLayer::teleportPlayer(object, player); return; }
        float bx = player ? player->getPositionX() : 0.f, by = player ? player->getPositionY() : 0.f;
        bool bu = player && player->m_isUpsideDown;
        GJBaseGameLayer::teleportPlayer(object, player);
        if (object) engine::noteActivation(object, 3);
        if (st.traceActivations) {
            log::info("[trace] tick {} teleport obj {} (uid {}) at ({:.1f},{:.1f}) ignoreX={} ignoreY={} gravityMode={} target {}: "
                      "player ({:.2f},{:.2f}){} -> ({:.2f},{:.2f}){}",
                st.tick, object ? object->m_objectID : 0, object ? object->m_uniqueID : 0,
                object ? object->getPositionX() : 0.f, object ? object->getPositionY() : 0.f,
                object ? object->m_ignoreX : false, object ? object->m_ignoreY : false, object ? object->m_gravityMode : 0,
                object ? object->m_targetGroupID : 0,
                bx, by, bu ? " flipped" : "", player ? player->getPositionX() : 0.f, player ? player->getPositionY() : 0.f,
                player && player->m_isUpsideDown ? " flipped" : "");
        }
    }

};

class $modify(GDPFAppDelegate, AppDelegate) {
    void applicationDidEnterBackground() {
        if (Bot::get().active()) { log::info("[hooks] ignored background event"); return; }
        AppDelegate::applicationDidEnterBackground();
    }
    void applicationWillResignActive() {
        if (Bot::get().active()) return;
        AppDelegate::applicationWillResignActive();
    }
};

class $modify(GDPFPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void) self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
        (void) self.setHookPriority("PlayLayer::levelComplete", Priority::First);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        Bot::get().onPlayLayerInit(this);
        return true;
    }

    void startGame() {
        PlayLayer::startGame();
        Bot::get().onLevelStarted(this);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::get().onAfterReset(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object && object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        if (autorun::noclip()) return;
        auto& bot = Bot::get();
        if (bot.activeOn(this)) {
            auto& st = engine::state();
            if (!object) {
                st.deathCallers = gameCallers(6);
                if (st.traceActivations)
                    log::info("[trace] tick {} death with no object at ({:.2f},{:.2f}); game callers:{}", st.tick,
                        player ? player->getPositionX() : 0.f, player ? player->getPositionY() : 0.f, st.deathCallers);
            } else {
                st.deathCallers.clear();
            }
            if (bot.onDeath(this, player, object)) return;
            bool wasTest = m_isTestMode;
            m_isTestMode = true;
            PlayLayer::destroyPlayer(player, object);
            m_isTestMode = wasTest;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(CCPoint pos) {
        auto& bot = Bot::get();
        if (bot.activeOn(this) && bot.onReachedEnd(this)) return;
        PlayLayer::playEndAnimationToPos(pos);
    }

    void levelComplete() {
        auto& bot = Bot::get();
        if (bot.activeOn(this)) {
            if (getCurrentPercent() < 90.f) {
                log::warn("[hooks] early levelComplete at {:.1f}% (tick {}, x {:.0f}), ignored",
                    getCurrentPercent(), engine::state().tick, engine::playerX(this));
                return;
            }
            bool swallow = bot.onReachedEnd(this);
            if (swallow) return;
            bool wasTest = m_isTestMode;
            m_isTestMode = true;
            PlayLayer::levelComplete();
            m_isTestMode = wasTest;
            bot.onLevelComplete(this);
            return;
        }
        PlayLayer::levelComplete();
    }

    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        if (Bot::get().activeOn(this)) return;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }

    void onQuit() {
        Bot::get().onQuit(this);
        PlayLayer::onQuit();
    }

    void showEndLayer() {
        if (Bot::get().fastStepping()) return;
        PlayLayer::showEndLayer();
    }
};

class $modify(GDPFPlayerObject, PlayerObject) {
    void incrementJumps() {
        if (Bot::get().active()) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(GDPFAudio, FMODAudioEngine) {
    void setMusicTimeMS(unsigned int time, bool dontWait, int musicID) {
        if (Bot::get().fastStepping()) return;
        FMODAudioEngine::setMusicTimeMS(time, dontWait, musicID);
    }

    int playEffect(gd::string path) {
        if (Bot::get().fastStepping()) return 0;
        return FMODAudioEngine::playEffect(path);
    }

    int playEffect(gd::string path, float speed, float p2, float volume) {
        if (Bot::get().fastStepping()) return 0;
        return FMODAudioEngine::playEffect(path, speed, p2, volume);
    }

    int playEffectAdvanced(gd::string path, float speed, float p2, float volume, float pitch, bool fft, bool reverb,
                           int startMillis, int endMillis, int fadeIn, int fadeOut, bool loop, int effectID, bool override,
                           bool noPreload, int channelID, int uniqueID, float minInterval, int sfxGroup) {
        if (Bot::get().fastStepping()) return 0;
        return FMODAudioEngine::playEffectAdvanced(path, speed, p2, volume, pitch, fft, reverb, startMillis, endMillis,
            fadeIn, fadeOut, loop, effectID, override, noPreload, channelID, uniqueID, minInterval, sfxGroup);
    }
};

class $modify(GDPFActionManager, CCActionManager) {
    void update(float dt) {
        auto& st = engine::state();
        if (st.takeover && !st.internalAction && Bot::get().fastStepping()) return;
        CCActionManager::update(dt);
    }
};

class $modify(GDPFScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);
        Bot::get().onFrame(dt);
        autorun::tick(dt);
    }
};
