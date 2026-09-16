#pragma once

#include <Geode/Geode.hpp>
#include <filesystem>
#include <string>

#include "Session.hpp"

namespace gdpf {

struct PathFile {
    int format = 1;
    std::string levelKey;
    std::string levelName;
    std::string modVersion;
    int ticks = 0;
    InputList inputs;

    matjson::Value toJson() const;
    static geode::Result<PathFile> fromJson(matjson::Value const& json);

    geode::Result<> save(std::filesystem::path const& path) const;
    static geode::Result<PathFile> load(std::filesystem::path const& path);
};

std::string describeInputs(InputList const& inputs, size_t maxEvents = 40);

}
