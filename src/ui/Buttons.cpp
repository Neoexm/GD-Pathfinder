#include <Geode/Geode.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelPage.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>

#include "../Bot.hpp"

using namespace geode::prelude;
using namespace gdpf;

namespace {
    CCMenuItemSpriteExtra* makePathfindButton(CCObject* target, SEL_MenuHandler handler, CircleBaseSize size) {
        auto spr = CircleButtonSprite::createWithSpriteFrameName("gj_robotBtn_on_001.png", 1.f, CircleBaseColor::Green, size);
        auto btn = CCMenuItemSpriteExtra::create(spr, target, handler);
        btn->setID("pathfind-button"_spr);
        return btn;
    }
}

class $modify(GDPFLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        auto btn = makePathfindButton(this, menu_selector(GDPFLevelInfoLayer::onPathfind), CircleBaseSize::Medium);
        if (auto menu = this->getChildByID("left-side-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto ownMenu = CCMenu::create();
            ownMenu->setID("pathfind-menu"_spr);
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            ownMenu->setPosition({30.f, winSize.height / 2 - 100.f});
            ownMenu->addChild(btn);
            this->addChild(ownMenu);
        }
        return true;
    }

    void onPlay(CCObject* sender) {
        if (!m_fields->m_fromPathfind) Bot::get().clearPending();
        LevelInfoLayer::onPlay(sender);
    }

    void onPathfind(CCObject*) {
        Bot::get().requestSolve(m_level);
        m_fields->m_fromPathfind = true;
        this->onPlay(nullptr);
        m_fields->m_fromPathfind = false;
    }

    struct Fields {
        bool m_fromPathfind = false;
    };
};

class $modify(GDPFLevelPage, LevelPage) {
    bool init(GJGameLevel* level) {
        if (!LevelPage::init(level)) return false;
        if (!level || level->m_levelID.value() <= 0) return true;
        auto btn = makePathfindButton(this, menu_selector(GDPFLevelPage::onPathfind), CircleBaseSize::Small);
        auto menu = CCMenu::create();
        menu->setID("pathfind-menu"_spr);
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({winSize.width - 35.f, 35.f});
        menu->addChild(btn);
        this->addChild(menu, 10);
        return true;
    }

    void onPlay(CCObject* sender) {
        if (!m_fields->m_fromPathfind) Bot::get().clearPending();
        LevelPage::onPlay(sender);
    }

    void onPathfind(CCObject*) {
        Bot::get().requestSolve(m_level);
        m_fields->m_fromPathfind = true;
        this->onPlay(nullptr);
        m_fields->m_fromPathfind = false;
    }

    struct Fields {
        bool m_fromPathfind = false;
    };
};

class $modify(GDPFEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        auto btn = makePathfindButton(this, menu_selector(GDPFEditLevelLayer::onPathfind), CircleBaseSize::Medium);
        if (auto menu = this->getChildByID("level-actions-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto ownMenu = CCMenu::create();
            ownMenu->setID("pathfind-menu"_spr);
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            ownMenu->setPosition({winSize.width - 30.f, winSize.height / 2 - 100.f});
            ownMenu->addChild(btn);
            this->addChild(ownMenu);
        }
        return true;
    }

    void onPlay(CCObject* sender) {
        if (!m_fields->m_fromPathfind) Bot::get().clearPending();
        EditLevelLayer::onPlay(sender);
    }

    void onPathfind(CCObject*) {
        Bot::get().requestSolve(m_level);
        m_fields->m_fromPathfind = true;
        this->onPlay(nullptr);
        m_fields->m_fromPathfind = false;
    }

    struct Fields {
        bool m_fromPathfind = false;
    };
};
