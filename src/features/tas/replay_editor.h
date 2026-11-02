#pragma once
#include "tas/replay.h"
#include "tas/tas.h"

#include <cstddef>
#include <string>
#include <vector>

// The TAS replay editor: frame by frame. A transport bar moves the game itself (back to the start, one frame back,
// pause, one frame on, to the end, or to any frame: going back plays the macro again from its start up to there),
// the frame list shows every frame's inputs, and the input panel shows and changes what is held on the selected frame:
// pause, set the keys of the frame that plays next, step. At the end of a playback "Add frame" grows the macro by a frame
// and plays it, so a macro can be built one frame at a time.
//
// DrawInputDisplay() is the on-screen input display: the keys held on the frame being played or recorded.
namespace tasui {

struct EditorKeys {                 // Hotkey names for the tooltips
    std::string pause, advance;
};

class ReplayEditor {
public:
    void Draw(bool& open, const tas::Status& st, const EditorKeys& keys);
    static void DrawInputDisplay(const tas::Status& st);

private:
    void DrawTransport(tas::Replay& r, const tas::Status& st, const EditorKeys& keys);
    void DrawFrameList(tas::Replay& r, const tas::Status& st);
    void DrawInputs(tas::Replay& r, const tas::Status& st);
    void DrawRaw(tas::Replay& r, std::size_t frame);

    long long selected_ = -1;
    bool follow_ = true;
    std::size_t lastFollowed_ = static_cast<std::size_t>(-1);
    int goto_ = 0;
    int frameCount_ = 1;            // frames "Add" / "Delete" add or remove
    char newKey_[48] = "";          // a key for the input panel
    char newName_[48] = "";         // raw inputs: the name of a new event
    std::vector<std::string> extraKeys_;
};

} // namespace tasui
