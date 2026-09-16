#pragma once

#include <Geode/Geode.hpp>
#include <string>

namespace gdpf {

struct LeanLevelInfo {
    geode::Ref<GJGameLevel> level;
    size_t objectsBefore = 0;
    size_t objectsAfter = 0;
};

LeanLevelInfo buildLeanLevel(PlayLayer* pl, bool dump = false);

}
