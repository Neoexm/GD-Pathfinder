#pragma once

#include <cstdint>
#include <vector>

#include "Corridor.hpp"

// An EXACT plan for the ball, as opposed to BallPlan's quantised DP.
//
// The ball is the one mode where a one-unit y cell is fatal. Death Corridor's spike weave
// gives the ball a 26-unit band to live in for 500 ticks with nothing but gravity rings to
// turn around on, and a plan that perturbs y by half a cell at every stride boundary calls
// the only line that works "doomed" - which is exactly what happened: 94.63%, over and over,
// with the rules one to five ticks late at every decision because the rank could not tell a
// tick apart.
//
// So this plans in the engine's own arithmetic. The transition was fitted against 789 ticks
// of [etrace] across two speeds and misses zero of them:
//
//   base impulse        surface: 0.240 * m_yStart      blue ring: 0.224 * m_yStart
//   gravity             0.1294 vy per tick, sign = +1 when m_isUpsideDown, rounded to 3dp
//   position            y += vy * 0.225, vy clamped to +-15
//   m_yStart            speed dependent: 11.23 at 3x, 10.62 at 0.5x - NOT size dependent
//
//   the buffer only survives while the button is HELD: releaseButton clears it, so an
//   arming press must be held until it resolves, and a held button fires at the first
//   contact of anything.
//   a press EDGE resolves BEFORE the gravity step: it flips gravity, sets vy to the base
//   impulse, and then takes the new frame's gravity step on top (so vy = r3(base + g_new)).
//   a press that could not resolve leaves a buffer that survives until it does. the buffer
//   resolves AFTER the gravity step, so it sets vy to the base impulse with no further
//   gravity - and, for a ring, with no further move either.
//   "already on a surface" is read from the PRE-move position, so a buffered surface press
//   fires on the tick AFTER the landing, never on the landing tick itself. a ring fires on
//   the tick of FIRST CONTACT, which is one tick earlier than any press-while-touching.
//
// The search is a beam over exact states with parent tracking. A beam of 60 finds the whole
// 1293-tick Death Corridor ball section from its entry state; the frontier never collapses.
namespace gdpf {
namespace ballexact {

struct Params {
    double yStart   = 11.23;
    double perTick  = 1.949215;
    double dir      = 1.0;
    double hw       = 9.0;
    double hh       = 9.0;
    double grav     = 0.1294;
    double vyToY    = 0.225;
    double vyMax    = 15.0;
    double surfBase = 0.240;
    // How far in front of a speed portal the game applies it. Measured: the 0.5x portal at
    // x 41445 changes m_playerSpeed on the frame the ball is at 41419.449, so 25.55 units
    // early. (The 3x portal at 41775 fires about 16 units earlier still, which no box test
    // explains; it lands in a hazard-free stretch here, and a plan that mismatches the
    // engine is rebuilt from the engine's own state, so the slack is absorbed.)
    double portalLead = 26.0;
    // per Corridor orb kind. Only kinds that reverse gravity are modelled; anything else is
    // left unfirable, which makes the plan pessimistic about that orb rather than wrong.
    double orbBase[8] = {0.224, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
};

struct Plan {
    int                  tick0 = -1;
    double               x0 = 0.0;
    std::vector<uint8_t> act;      // button state to hold at tick0 + i
    std::vector<float>   y;        // y at the START of tick0 + i
    std::vector<float>   vy;
    std::vector<uint8_t> g;
    std::vector<uint8_t> held;     // button state at the start of tick0 + i
    std::vector<uint8_t> buf;      // m_jumpBuffered at the start of tick0 + i
    bool                 ok = false;
    int                  depth = 0;
    bool                 reachedEnd = false;
    double               endX = 0.0;

    bool covers(int tick) const {
        return tick0 >= 0 && tick >= tick0 && (size_t) (tick - tick0) < act.size();
    }
    int at(int tick) const { return act[(size_t) (tick - tick0)]; }
    // true when this plan was built for, or has been followed to, exactly this state
    bool matches(int tick, double yy, double vvy, int gg, int hheld, int bbuf, double eps) const;
};

struct Start {
    double x = 0.0, y = 0.0, vy = 0.0;
    int    g = 0, held = 0, buf = 0;
    int    tick = 0;
};

// Build a plan from `st`. `stopX` ends the plan early (a speed or gamemode portal the
// transition above does not model); 0 means no limit. Returns a plan whose `ok` says whether
// the beam survived the whole horizon.
void setTrace(int v);

Plan build(Corridor const& cor, Start const& st, Params const& prm,
           int horizon, int beam, double stopX);

}   // namespace ballexact
}   // namespace gdpf
