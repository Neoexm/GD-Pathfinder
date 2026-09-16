#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace gdpf {

struct InputEvent {
    int tick = 0;
    int button = 1;
    bool down = false;
    bool player2 = false;
    // A reference recording carries the x it happened at. Replaying by x instead of by
    // tick removes every alignment and drift error between the recorder and this engine.
    float atX = -1.f;
};

using InputList = std::vector<InputEvent>;

enum class Mode {
    Idle,
    Solving,
    Playing,
    Verifying,
    Experiment,
};

enum class Request {
    None,
    Solve,
    PlayInputs,
    VerifyInputs,
    RunExperiment,
    Resume,
};

struct RunResult {
    bool completed = false;
    bool died = false;
    int endTick = 0;
    float furthestX = 0.f;
    float furthestPercent = 0.f;
    uint64_t trajectoryHash = 0;
    int deathObjectID = 0;
};

inline char const* modeName(Mode m) {
    switch (m) {
        case Mode::Idle: return "idle";
        case Mode::Solving: return "solving";
        case Mode::Playing: return "playing";
        case Mode::Verifying: return "verifying";
        case Mode::Experiment: return "experiment";
    }
    return "?";
}

}
