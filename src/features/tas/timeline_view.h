#pragma once
#include "tas/replay.h"
#include "tas/tas.h"
#include "tas/timeline.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The TAS timeline window: the macro as rows of key holds (bars) and axis movement, a camera row with keyframes, and a
// ruler to seek with. Edits go through tas/timeline.h on the replay in memory; while recording it is read only.
//
//     double click a row        add a hold (keys) or a camera key (camera row)
//     drag a bar / its ends     move it / change when it starts or ends
//     drag a camera key         move it to another frame
//     right click               delete the bar or key under the mouse
//     click or drag the ruler   go to that frame (plays the macro up to there)
//     ctrl + wheel              zoom around the mouse; shift + wheel scrolls
namespace tasui {

// A steady color per key name (the timeline's bars, the editor's and the input display's keys).
ImU32 KeyColor(const std::string& name, float alpha = 1.f);

class TimelineView {
public:
    void Draw(bool& open, const tas::Status& status);

private:
    struct Selection {
        enum class Kind { None, Hold, Camera } kind = Kind::None;
        std::uint16_t name = 0;                 // Hold: the key
        tas::timeline::Hold hold;
        std::uint32_t frame = 0;                // Camera: the key's frame
    };
    struct Drag {
        enum class What { None, Seek, Move, Start, End, Camera } what = What::None;
        std::uint16_t name = 0;
        tas::timeline::Hold orig;
        std::vector<tas::timeline::Hold> others;  // the track's other holds, as they were
        std::uint32_t camFrame = 0;
        float startX = 0.f;
        int delta = 0;                            // frames moved so far
    };

    void DrawToolbar(tas::Replay& r, const tas::Status& st, bool readOnly);
    void DrawCanvas(tas::Replay& r, const tas::Status& st, bool readOnly);
    void DrawDetails(tas::Replay& r, bool readOnly);
    void Commit(tas::Replay& r);
    tas::timeline::Hold Preview() const;          // the dragged hold where it would land

    float pxPerFrame_ = 6.f;
    bool follow_ = true;
    int newHold_ = 10;                            // frames a double click adds
    char newTrack_[48] = "";
    std::vector<std::string> extraTracks_;        // added with no inputs yet
    Selection sel_;
    Drag drag_;
    std::size_t frames_ = 0;
};

} // namespace tasui
