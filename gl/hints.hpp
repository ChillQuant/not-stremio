// Contextual button hints bar: one thin, dim line near the bottom of the
// screen, e.g. "R3 Stats · L3 Restart · Options More" during playback and
// "Options: Mark watched" on the grids. The text follows the screen; the bar
// fades out after 5 s without input and comes back on any button press.
//
// The state machine is driven by Hints::update (once per frame from the
// polish pump in Session::update); Hints::draw renders it and is called by
// the shell after the toast. Never throws; update is safe to call before the
// fonts exist (draw then simply skips).

#pragma once

#include <string>

namespace hui::gfx
{
class DrawList;
}

namespace sx
{
struct Env;

namespace Hints
{

enum class Context
{
    grid,     // Board / Discover / Library
    detail,   // a title's page
    search,   // search results
    playback, // the video player
};

// Once per frame. context_text is the full hints line for the current screen
// (nullptr/empty: the built-in line for the context). idle_seconds is the
// time since the last input; a reset of that clock counts as a button press.
void update(float dt, Context context, const char *context_text, double idle_seconds);

// Draws the bar; a no-op while faded out or with no text.
void draw(hui::gfx::DrawList &list, const Env &env);

// Current state, for tests and debug.
const std::string &text();
float alpha(); // 0..1

} // namespace Hints
} // namespace sx
