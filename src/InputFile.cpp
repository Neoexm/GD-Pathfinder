#include "InputFile.hpp"

#include <Geode/utils/file.hpp>

using namespace geode::prelude;

namespace gdpf {

matjson::Value PathFile::toJson() const {
    auto json = matjson::Value::object();
    json.set("format", matjson::Value(format));
    json.set("level", matjson::Value(levelKey));
    json.set("name", matjson::Value(levelName));
    json.set("mod", matjson::Value(modVersion));
    json.set("ticks", matjson::Value(ticks));
    auto arr = matjson::Value::array();
    for (auto const& ev : inputs) {
        auto e = matjson::Value::array();
        e.push(matjson::Value(ev.tick));
        e.push(matjson::Value(ev.button));
        e.push(matjson::Value(ev.down));
        e.push(matjson::Value(ev.player2));
        if (ev.atX >= 0.f) e.push(matjson::Value((double) ev.atX));
        arr.push(std::move(e));
    }
    json.set("inputs", std::move(arr));
    return json;
}

Result<PathFile> PathFile::fromJson(matjson::Value const& json) {
    PathFile pf;
    if (!json.isObject()) return Err("path file is not an object");
    if (json.contains("format")) pf.format = (int) json["format"].asInt().unwrapOr(1);
    if (json.contains("level")) pf.levelKey = json["level"].asString().unwrapOr("");
    if (json.contains("name")) pf.levelName = json["name"].asString().unwrapOr("");
    if (json.contains("mod")) pf.modVersion = json["mod"].asString().unwrapOr("");
    if (json.contains("ticks")) pf.ticks = (int) json["ticks"].asInt().unwrapOr(0);
    if (!json.contains("inputs")) return Err("path file has no inputs");
    auto arrRes = json["inputs"].asArray();
    if (arrRes.isErr()) return Err("inputs is not an array");
    for (auto const& e : arrRes.unwrap()) {
        auto tuple = e.asArray();
        if (tuple.isErr() || tuple.unwrap().size() < 3) return Err("bad input event");
        auto const& t = tuple.unwrap();
        InputEvent ev;
        ev.tick = (int) t[0].asInt().unwrapOr(0);
        ev.button = (int) t[1].asInt().unwrapOr(1);
        ev.down = t[2].asBool().unwrapOr(false);
        ev.player2 = t.size() > 3 ? t[3].asBool().unwrapOr(false) : false;
        ev.atX = t.size() > 4 ? (float) t[4].asDouble().unwrapOr(-1.0) : -1.f;
        pf.inputs.push_back(ev);
    }
    return Ok(std::move(pf));
}

Result<> PathFile::save(std::filesystem::path const& path) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return file::writeStringSafe(path, toJson().dump(2));
}

Result<PathFile> PathFile::load(std::filesystem::path const& path) {
    auto str = file::readString(path);
    if (str.isErr()) return Err(str.unwrapErr());
    auto json = matjson::Value::parse(str.unwrap());
    if (json.isErr()) return Err("invalid json in path file");
    return fromJson(json.unwrap());
}

std::string describeInputs(InputList const& inputs, size_t maxEvents) {
    std::string out;
    size_t n = 0;
    for (auto const& ev : inputs) {
        if (n++ >= maxEvents) { out += fmt::format(" ...(+{})", inputs.size() - maxEvents); break; }
        if (!out.empty()) out += ' ';
        out += fmt::format("{}{}{}{}", ev.tick, ev.down ? "v" : "^", ev.button == 1 ? "" : ev.button == 2 ? "L" : "R", ev.player2 ? "'" : "");
    }
    return out;
}

}
