#include "LeanLevel.hpp"

#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace gdpf {

namespace {
    bool objectValue(std::string_view obj, std::string_view key, std::string_view& out) {
        size_t pos = 0;
        while (pos < obj.size()) {
            size_t kEnd = obj.find(',', pos);
            if (kEnd == std::string_view::npos) return false;
            size_t vEnd = obj.find(',', kEnd + 1);
            std::string_view k = obj.substr(pos, kEnd - pos);
            std::string_view v = obj.substr(kEnd + 1, vEnd == std::string_view::npos ? std::string_view::npos : vEnd - kEnd - 1);
            if (k == key) { out = v; return true; }
            if (vEnd == std::string_view::npos) return false;
            pos = vEnd + 1;
        }
        return false;
    }

    int toInt(std::string_view v) {
        char buf[24];
        size_t n = std::min(v.size(), sizeof(buf) - 1);
        std::memcpy(buf, v.data(), n);
        buf[n] = 0;
        return std::atoi(buf);
    }

    float toFloat(std::string_view v) {
        char buf[32];
        size_t n = std::min(v.size(), sizeof(buf) - 1);
        std::memcpy(buf, v.data(), n);
        buf[n] = 0;
        return (float) std::atof(buf);
    }
}

LeanLevelInfo buildLeanLevel(PlayLayer* pl, bool dump) {
    LeanLevelInfo info;
    if (!pl || !pl->m_level || !pl->m_objects) return info;
    auto level = pl->m_level;

    std::unordered_set<int> keepTypes;
    std::unordered_set<int> refGroups;
    float maxX = 0.f;
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj) continue;
        maxX = std::max(maxX, obj->getPositionX());
        if (obj->m_isTrigger) {
            keepTypes.insert(obj->m_objectID);
            auto eff = static_cast<EffectGameObject*>(obj);
            for (int g : {eff->m_centerGroupID, eff->m_targetModCenterID, eff->m_rotationTargetID, eff->m_specialTarget})
                if (g > 0) refGroups.insert(g);
            if (obj->m_objectID == 3033 || obj->m_objectID == 3016 || obj->m_objectID == 1347) {
                if (eff->m_targetGroupID > 0) refGroups.insert(eff->m_targetGroupID);
            }
            continue;
        }
        if (obj->m_objectID == 2902) {
            int g = static_cast<EffectGameObject*>(obj)->m_targetGroupID;
            if (g > 0) refGroups.insert(g);
        }
        if (obj->m_objectType != GameObjectType::Decoration || obj->m_hasExtendedCollision) keepTypes.insert(obj->m_objectID);
    }

    std::string raw = level->m_levelString;
    if (raw.empty()) return info;
    std::string plain = raw.rfind("H4sI", 0) == 0 ? std::string(ZipUtils::decompressString(raw, false, 0)) : raw;
    if (plain.empty()) { log::warn("[lean] could not decompress the level string"); return info; }

    std::vector<std::string_view> parts;
    {
        size_t pos = 0;
        while (pos <= plain.size()) {
            size_t e = plain.find(';', pos);
            if (e == std::string::npos) { parts.push_back(std::string_view(plain).substr(pos)); break; }
            parts.push_back(std::string_view(plain).substr(pos, e - pos));
            pos = e + 1;
        }
    }
    if (parts.empty()) return info;

    std::string out;
    out.reserve(plain.size() / 3);
    out += parts[0];
    size_t kept = 0, total = 0;
    for (size_t i = 1; i < parts.size(); i++) {
        auto obj = parts[i];
        if (obj.empty()) continue;
        total++;
        std::string_view v;
        bool keep = false;
        if (objectValue(obj, "1", v) && keepTypes.count(toInt(v))) keep = true;
        if (!keep && objectValue(obj, "2", v) && toFloat(v) >= maxX - 300.f) keep = true;
        if (!keep && objectValue(obj, "2", v) && toFloat(v) <= 150.f) keep = true;
        if (!keep && !refGroups.empty() && objectValue(obj, "57", v)) {
            size_t p = 0;
            while (p < v.size()) {
                size_t e = v.find('.', p);
                int g = toInt(v.substr(p, e == std::string_view::npos ? std::string_view::npos : e - p));
                if (refGroups.count(g)) { keep = true; break; }
                if (e == std::string_view::npos) break;
                p = e + 1;
            }
        }
        if (!keep) continue;
        out += ';';
        out += obj;
        kept++;
    }
    out += ';';

    if (dump) {
        auto dir = Mod::get()->getSaveDir() / "dumps";
        (void) file::createDirectoryAll(dir);
        auto stem = fmt::format("{}", level->m_levelID.value());
        (void) file::writeString(dir / (stem + ".real.txt"), plain);
        (void) file::writeString(dir / (stem + ".lean.txt"), out);
        log::info("[lean] level strings written to {}", (dir / stem).string());
    }

    auto lean = GJGameLevel::create(CCDictionary::create(), false);
    if (!lean) return info;
    lean->m_levelString = ZipUtils::compressString(out, false, 0);
    lean->m_levelName = std::string(level->m_levelName) + " (search)";
    lean->m_levelID = 0;
    lean->m_levelType = GJLevelType::Main;
    lean->m_audioTrack = level->m_audioTrack;
    lean->m_songID = level->m_songID;
    lean->m_songIDs = level->m_songIDs;
    lean->m_sfxIDs = level->m_sfxIDs;
    lean->m_levelVersion = level->m_levelVersion;
    lean->m_gameVersion = level->m_gameVersion;
    lean->m_twoPlayerMode = level->m_twoPlayerMode;
    lean->m_levelLength = level->m_levelLength;

    info.level = lean;
    info.objectsBefore = total;
    info.objectsAfter = kept;
    log::info("[lean] {} objects -> {} ({} kept kinds, {} referenced groups, {} KB -> {} KB)", total, kept,
        keepTypes.size(), refGroups.size(), plain.size() / 1024, out.size() / 1024);
    return info;
}

}
