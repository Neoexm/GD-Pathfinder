#pragma once

#include <chrono>
#include <map>
#include <string>

namespace gdpf::profile {

struct Entry {
    uint64_t calls = 0;
    double seconds = 0.0;
};

std::map<std::string, Entry>& entries();
bool enabled();
void setEnabled(bool on);
void reset();
std::string report();

struct Scope {
    char const* name;
    std::chrono::steady_clock::time_point start;
    explicit Scope(char const* n) : name(n), start(std::chrono::steady_clock::now()) {}
    ~Scope() {
        if (!enabled()) return;
        auto& e = entries()[name];
        e.calls++;
        e.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
};

}
