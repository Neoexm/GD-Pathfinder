#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>

#include "Autorun.hpp"

using namespace geode::prelude;

namespace {
    using RefuseNode = SettingValueNodeV3<BoolSettingV3>;

    bool s_refusePopupOpen = false;
    std::optional<ListenerHandle> s_refuseListener;

    void warnAboutRefusingFatalClicks(Ref<SettingNodeV3> node) {
        s_refusePopupOpen = true;
        createQuickPopup(
            "Are you sure?",
            "The bot will <cr>skip</c> presses it predicts are fatal, instead of just trying them last.\n"
            "The prediction is rough - it can throw away the only route through a section and leave the "
            "bot <cr>stuck</c> on levels it would otherwise beat.\n"
            "Leave this off unless you are testing.",
            "Cancel", "Enable",
            400.f,
            [node](FLAlertLayer*, bool enable) {
                s_refusePopupOpen = false;
                if (enable) return;
                if (auto n = typeinfo_cast<RefuseNode*>(node.data())) n->setValue(false, nullptr);
            }
        );
    }
}

$on_mod(Loaded) {
    log::info("GD Pathfinder {} loaded", Mod::get()->getVersion().toVString());
    gdpf::autorun::init();
    // fires the moment the checkbox is ticked, before Apply
    s_refuseListener = SettingNodeValueChangeEventV3(Mod::get(), "refuse-fatal-clicks").listen(
        [](SettingNodeV3* node, bool isCommit) {
            if (isCommit || s_refusePopupOpen) return;
            auto n = typeinfo_cast<RefuseNode*>(node);
            if (!n) { log::warn("[settings] refuse-fatal-clicks node cast failed, no warning shown"); return; }
            if (n->getValue()) warnAboutRefusingFatalClicks(node);
        });
}

class $modify(GDPFMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        gdpf::autorun::onMenuLayer();
        return true;
    }
};
