#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>

namespace gdpf {

class ProgressPopup : public geode::Popup {
public:
    static ProgressPopup* create();
    static ProgressPopup* current();
    void refresh();
    void dismiss();

protected:
    bool init();
    void update(float dt) override;
    void onCancel(cocos2d::CCObject*);
    void onClose(cocos2d::CCObject*) override;
    void keyBackClicked() override;

    cocos2d::CCLabelBMFont* m_percentLabel = nullptr;
    cocos2d::CCLabelBMFont* m_infoLabel = nullptr;
    bool m_closing = false;
};

void showBotAlert(std::string const& title, std::string const& text);

}
