#pragma once
#include "tas/history.h"
#include "tas/replay.h"
#include "tas/tas.h"
#include "tas/timeline.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The TAS timeline window: the macro as rows of key holds (bars) and axis movement (bars as tall as the movement), a
// camera row with keyframes, bookmarks on the ruler. Edits go through tas/timeline.h on the replay in memory, every one
// can be undone; while recording it is read only.
//
//     click                     select a hold or camera key (ctrl + click: add to / take from the selection)
//     shift + drag              select everything in a box
//     drag                      move the selection; drag a bar's end to resize that bar
//     double click              add a hold (key rows), a camera key (camera row) or a bookmark (ruler)
//     right click               what can be done there: delete, split, duplicate, ease, insert or delete frames...
//     click or drag the ruler   go to that frame (plays the macro up to there)
//     ctrl + wheel              zoom around the mouse; shift + wheel scrolls
//
//     ctrl+Z / ctrl+Y   undo / redo        ctrl+C / X / V   copy / cut / paste (at the mouse, or the session's frame)
//     ctrl+D            duplicate          Delete           delete the selection
//     left / right      nudge a frame (shift: 10)          ctrl+A  select all     Esc  select nothing
//     F                 fit the macro in the window        space   pause / resume the session
namespace tasui {

// A steady color per key name (the timeline's bars, the editor's and the input display's keys).
ImU32 KeyColor(const std::string& name, float alpha = 1.f);

// Undo / redo shared by the timeline and the replay editor. Use it on the render thread, with the replay locked
// (inside tas::WithReplay): Push before an edit, Watch once per frame.
tas::EditHistory& History();

class TimelineView {
public:
    void Draw(bool& open, const tas::Status& status);

private:
    using KeyHold = tas::timeline::KeyHold;
    using Hold = tas::timeline::Hold;

    struct Drag {
        enum class What { None, Seek, Move, Start, End, Box } what = What::None;
        KeyHold one;                              // Start / End: the bar being resized
        std::vector<Hold> others;                 // ...and the other bars of its row, as they were
        float startX = 0.f, startY = 0.f;
        int delta = 0;                            // Frames moved so far
        bool additive = false;                    // Box: add to the selection (ctrl)
    };
    struct Context {                              // What a right click was on
        enum class Kind { None, Hold, Camera, Mark, Row, Ruler } kind = Kind::None;
        KeyHold hold;
        std::uint16_t row = 0;                    // Row: the key
        bool axis = false;
        std::uint32_t frame = 0;                  // The frame under the mouse (camera key / bookmark: theirs)
    };

    void Shortcuts(tas::Replay& r, const tas::Status& st, bool readOnly, bool focused);
    void DrawToolbar(tas::Replay& r, const tas::Status& st, bool readOnly);
    void DrawCanvas(tas::Replay& r, const tas::Status& st, bool readOnly);
    void DrawContextMenu(tas::Replay& r, bool readOnly);
    void DrawDetails(tas::Replay& r, bool readOnly);

    // ---- the selection --------------------------------------------------------------------------------------------
    bool Selected(const KeyHold& h) const;
    bool SelectedCamera(std::uint32_t frame) const;
    bool Empty() const { return holds_.empty() && cams_.empty(); }
    void Select(const KeyHold& h, bool toggle);
    void SelectCamera(std::uint32_t frame, bool toggle);
    void SelectAll(const tas::Replay& r);
    void Prune(const tas::Replay& r);             // Drops what no longer exists (after an edit, an undo, a load)
    int GroupDelta(const tas::Replay& r, int delta) const;   // `delta` cut so the selection stays inside the replay

    // ---- edits (each one undoable) --------------------------------------------------------------------------------
    void MoveSelection(tas::Replay& r, int delta);
    void DeleteSelection(tas::Replay& r);
    void CopySelection(const tas::Replay& r);
    void PasteAt(tas::Replay& r, std::uint32_t frame);
    void Duplicate(tas::Replay& r);
    void Commit(tas::Replay& r);                  // The end of a drag
    Hold ResizePreview() const;
    std::uint32_t PasteFrame(const tas::Status& st) const;

    float pxPerFrame_ = 6.f;
    bool follow_ = true;
    bool fitNext_ = false;                        // Fit the macro in the window on the next draw
    int newHold_ = 10;                            // Frames a double click adds
    int frameCount_ = 1;                          // Frames the ruler menu inserts / deletes
    char newTrack_[48] = "";
    char markName_[64] = "";
    std::vector<std::string> extraTracks_;        // Added with no inputs yet
    std::vector<KeyHold> holds_;                  // The selection
    std::vector<std::uint32_t> cams_;
    tas::timeline::Clip clip_;                    // Copied
    Drag drag_;
    Context ctx_;
    bool hoveredCanvas_ = false;
    int mouseFrame_ = 0;
    float viewW_ = 0.f;
};

} // namespace tasui
