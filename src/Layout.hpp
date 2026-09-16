#pragma once

#include <Geode/Geode.hpp>
#include <cstring>
#include <string>

namespace gdpf::layout {

struct Member { char const* name; size_t offset; };

std::string describe(char const* cls, size_t offset);
size_t sizeOf(char const* cls);

}
