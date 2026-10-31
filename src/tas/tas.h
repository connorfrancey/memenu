#pragma once
#include "tas/replay.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

// Macro / TAS / replay sessions (see engine.h for how the game is driven).
//
// A session runs the game on its own clock: every frame advances the game by exactly the replay's time step (or, for a
// real-time recording, by what that frame really took, which is stored). The game thread's clocks, rand() and level
// streaming are made deterministic, and all input goes through the replay:
//
//   Recording  real input is held back to the start of the next frame, then stored and handed to the engine there.
//              Playback hands it over at the same point, so a recording and its playback see identical input.
//   Playing    real input is dropped, the replay's input is injected. The player's position, velocity and move are
//              compared bit for bit with the recording on every frame: the first frame that differs is reported.
//
// Start kinds: Now (a macro from the current state) or a level load (console command or level-select checkpoint), which
// is what makes a run reproducible and verifiable: the first frame is the first one after the level has loaded.
//
// All functions may be called from any thread. Commands run on the game thread at the start of the next frame.
namespace tas {

enum class Mode { Idle, Recording, Playing };
enum class AtEnd { Stop = 0, Pause = 1, Record = 2 };   // what playback does after the last frame

// What a recording does when the player dies (like Geometry Dash bots: practice mode keeps only the good attempt).
//   Nothing   keep recording (the death and respawn become part of the run)
//   Restart   throw the attempt away and record again from the start level
//   Rewind    "practice mode": cut the recording `rewindSeconds` before the death, replay up to there (fast, checked
//             frame by frame) and pause; resume to keep recording from that point. Counts as a rerecord.
// Restart and Rewind need a level start: a macro that starts "now" can't go back, it keeps recording.
enum class OnDeath { Nothing = 0, Restart = 1, Rewind = 2 };

struct Options {
    float recordHz = 60.f;            // time step for new recordings; 0 = real time
    float realtimeCap = 62.f;         // real-time recordings: frame rate cap (0 = none). The game's own cap is 62
    float speed = 1.f;                // pacing of fixed-step frames (playback and recording): 1 = real time, 0 = max
    bool pauseOnDesync = true;
    bool blockStreaming = true;       // load streamed sublevels in the frame they are requested (deterministic)
    bool seekWithoutRendering = false;
    AtEnd atEnd = AtEnd::Pause;
    OnDeath onDeath = OnDeath::Rewind;
    float rewindSeconds = 2.f;        // how far Rewind (on death, or its key) goes back
    int keyPause = 0, keyAdvance = 0, keyTakeOver = 0, keyStop = 0, keyRewind = 0;   // virtual-key codes, polled while a session runs
};

struct Status {
    bool available = false;           // engine hooks found
    Mode mode = Mode::Idle;
    bool armed = false;               // waiting for the level to load
    bool paused = false;
    bool seeking = false;
    std::size_t frame = 0;            // next frame to run
    std::size_t frames = 0;           // frames in the replay
    double time = 0.0;                // game seconds into the run
    long long desyncFrame = -1;       // first frame whose state didn't match, -1 = none
    std::size_t syncChecked = 0;      // frames compared and matching this playback
    bool verified = false;            // the last playback matched the recording on every recorded frame
    std::string message;              // last thing that happened
};

bool Init();                          // after game::Init()
bool Available();

void SetOptions(const Options& options);
Status GetStatus();

// ---- session commands --------------------------------------------------------------------------------------------------
// Starts a new recording (the current replay is replaced).
void Record(StartKind start, const std::string& command, const std::string& map, const std::string& checkpoint);
void Play();                          // the current replay from its start
void Stop();
void TogglePause();
void Advance(int frames = 1);         // run this many frames, then pause
void TakeOver();                      // while playing: drop the rest and continue recording from this frame
void Goto(std::size_t frame);         // play (from the start if needed, as fast as possible) up to `frame`, then pause
void Rewind();                        // while recording: go back Options::rewindSeconds and pause there (practice mode)

// ---- the replay ----------------------------------------------------------------------------------------------------------
// Runs `fn` with the replay locked. Keep it short: the game thread waits for it at the start of each frame.
void WithReplay(const std::function<void(Replay&)>& fn);
// .merp, or JSON (ExportJson's format) when the path ends in .json. Saving to a .json path exports JSON.
bool LoadFile(const std::filesystem::path& path, std::string* error);
bool SaveFile(const std::filesystem::path& path, std::string* error);
void Clear();                         // stop, and start over with an empty replay
std::filesystem::path Folder();       // replays/ next to the DLL

} // namespace tas
