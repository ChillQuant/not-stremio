// The polish layer's per-frame pump: haptics timers, the haptics on/off pref,
// and the hints-bar state machine. Called once per frame from Session::update
// (a single line). Never draws, never throws, safe before init.

#pragma once

#include "haptics.hpp"
#include "hints.hpp"

namespace sx
{

inline Hints::Context hints_context_for(bool watching, bool detail_open, bool search_open)
{
    if (watching)
        return Hints::Context::playback;
    if (detail_open)
        return Hints::Context::detail;
    if (search_open)
        return Hints::Context::search;
    return Hints::Context::grid;
}

inline void polish_frame(float dt, bool haptics_enabled, Hints::Context hints_context, double idle_seconds)
{
    Haptics::instance().set_enabled(haptics_enabled);
    Haptics::instance().tick(dt);
    Hints::update(dt, hints_context, nullptr, idle_seconds);
}

} // namespace sx
