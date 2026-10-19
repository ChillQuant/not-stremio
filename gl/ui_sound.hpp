// The interface's sounds: soft sounds for moving around, going back, switching page and so on.
// They come from ps5-homebrew-ui's sound bank (the "glass" set, in <app>/sounds/glass) and play
// through its mixer on the console's audio port.
//
// Sound design (all quiet; the ui bus sits at 0.7):
//   focus  = soft tick (focus_02.wav, 50ms), very quiet, pitch rising slightly with move speed
//   select = warm "thock": the glass set records no select file, so the bank's
//            synthesized fallback plays (triangle 740->880 Hz, 80ms), gain-shaped here
//   back   = descending blip (back_01.wav, 200ms)
//   error  = muted dull tap (error_01.wav, 280ms), min 300ms between taps
// A cue asked for while the player has the port is dropped: UI sounds never
// play during video playback.
//
// The console gives the app one audio port at a time: while a video plays, the player has it.
// The player asks for it when it starts (AudioPort::player_wants) and this lets go of it, then
// takes it back when the player is done.

#pragma once

#include "audio/cues.hpp"

#include <memory>
#include <string>

namespace sx
{

class UiSound
{
  public:
    UiSound();
    ~UiSound();

    // Loads the sounds in <dir>/glass and opens the audio port. False: no sound (not fatal).
    bool start(const std::string &dir);
    // Plays a cue now. Cues asked for while the player has the port are dropped.
    void play(hui::audio::Cue cue);
    // Plays a cue as soon as the port is back (the player has just finished).
    void play_when_back(hui::audio::Cue cue);
    void set_enabled(bool on)
    {
        enabled_ = on;
    }
    // Once a frame: hands the port to the player and takes it back.
    void update(float dt);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool enabled_ = true;
};

} // namespace sx
