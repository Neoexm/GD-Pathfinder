#include "ProgressPopup.hpp"

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/FLAlertLayer.hpp>

#include "../Bot.hpp"
#include "../Solver.hpp"

using namespace geode::prelude;

namespace gdpf {

static ProgressPopup* s_current = nullptr;

ProgressPopup* ProgressPopup::create() {
    auto ret = new ProgressPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

ProgressPopup* ProgressPopup::current() { return s_current; }

bool ProgressPopup::init() {
    if (!Popup::init(220.f, 130.f)) return false;
    s_current = this;
    this->setID("progress-popup"_spr);
    this->setTitle("Pathfinding");
    if (m_closeBtn) m_closeBtn->setVisible(false);
    this->setOpacity(0);
    if (m_bgSprite) m_bgSprite->setOpacity(90);

    m_percentLabel = CCLabelBMFont::create("0.0%", "bigFont.fnt");
    m_percentLabel->setScale(0.9f);
    m_mainLayer->addChildAtPosition(m_percentLabel, Anchor::Center, {0.f, 8.f});

    m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_infoLabel->setScale(0.6f);
    m_infoLabel->setOpacity(180);
    m_mainLayer->addChildAtPosition(m_infoLabel, Anchor::Center, {0.f, -14.f});

    auto cancelSpr = ButtonSprite::create("Cancel", "goldFont.fnt", "GJ_button_06.png", 0.8f);
    auto cancelBtn = CCMenuItemSpriteExtra::create(cancelSpr, this, menu_selector(ProgressPopup::onCancel));
    cancelBtn->setID("cancel-button"_spr);
    m_buttonMenu->addChildAtPosition(cancelBtn, Anchor::Bottom, {0.f, 22.f});

    this->scheduleUpdate();
    return true;
}

void ProgressPopup::update(float) {
    refresh();
}

void ProgressPopup::refresh() {
    auto& bot = Bot::get();
    if (!bot.solving()) {
        if (!m_closing) dismiss();
        return;
    }
    if (!m_percentLabel) return;
    m_percentLabel->setString(fmt::format("{:.1f}%", bot.progressPercent()).c_str());
    auto routes = bot.progressRoutes();
    auto count = routes >= 10000 ? fmt::format("{}k", routes / 1000) : fmt::format("{}", routes);
    m_infoLabel->setString(fmt::format("{:.0f}s  {} paths  attempt {}", bot.solveSeconds(),
                                       count, bot.progressAttempts()).c_str());
}

void ProgressPopup::dismiss() {
    if (m_closing) return;
    m_closing = true;
    s_current = nullptr;
    this->unscheduleUpdate();
    Loader::get()->queueInMainThread([self = Ref(this)] { self->removeFromParentAndCleanup(true); });
}

void ProgressPopup::onCancel(CCObject*) {
    Bot::get().cancel();
    dismiss();
}

void ProgressPopup::onClose(CCObject*) {
    onCancel(nullptr);
}

void ProgressPopup::keyBackClicked() {
    onCancel(nullptr);
}

void showBotAlert(std::string const& title, std::string const& text) {
    auto alert = FLAlertLayer::create(title.c_str(), text, "OK");
    alert->m_noElasticity = true;
    alert->show();
}

}
