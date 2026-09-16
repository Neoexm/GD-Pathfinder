#pragma once

#include <Geode/Geode.hpp>
#include <memory>
#include <string>

#include "Session.hpp"

namespace gdpf {

class Experiment {
public:
    virtual ~Experiment() = default;
    virtual void start(PlayLayer* pl) = 0;
    virtual bool slice(PlayLayer* pl, double budgetSeconds) = 0;
    virtual std::string summary() const = 0;
    bool passed() const { return m_passed; }
protected:
    bool m_passed = false;
};

std::unique_ptr<Experiment> makeSnapshotTest(InputList inputs, int interval);

std::unique_ptr<Experiment> makeSpeedTest(InputList inputs);

std::unique_ptr<Experiment> makeRestoreDiff(InputList inputs, int snapTick, float diffX, int runTicks, int diffTick);

std::unique_ptr<Experiment> makeResetDrift(InputList inputs, int cycles, float diffX, int runTicks, int churn, int traceTicks = 0);

std::unique_ptr<Experiment> makeObjectDump(std::string levelKey);

}
