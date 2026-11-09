#pragma once
#include "tas/replay.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Undo / redo for the TAS editors (timeline and replay editor): the replay as it was before each edit. Snapshots of the
// whole replay keep it simple and exact (the sync data, camera keys and bookmarks come back too); a byte budget keeps a
// long macro from piling up memory: the oldest steps go first.
//
// The editors call Push() right before changing the replay. When the replay changes some other way (a recording, a
// load, a playback that rerecords), the steps no longer apply to it: Watch() notices and forgets them.
namespace tas {

class EditHistory {
public:
    explicit EditHistory(std::size_t budgetBytes = 64u << 20, std::size_t maxSteps = 200);

    // Before an edit: remembers `r` as it is now, under `what` ("Move W", "Delete 3 holds"). Forgets what was undone.
    void Push(const Replay& r, std::string what);
    // `r` goes back to before the last edit (and that edit can be redone). False if there is nothing to undo.
    bool Undo(Replay& r);
    bool Redo(Replay& r);
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    const std::string& UndoName() const;     // Empty when there is nothing
    const std::string& RedoName() const;
    std::size_t Steps() const { return undo_.size(); }
    void Clear();

    // Call with the replay each frame an editor shows it: forgets every step when the replay was changed by something
    // other than an edit pushed here (or an undo / redo).
    void Watch(const Replay& r);

    static std::size_t Bytes(const Replay& r);

private:
    struct Step {
        Replay replay;
        std::string what;
        std::size_t bytes = 0;
    };
    struct Signature {
        std::size_t frames = 0, events = 0, camera = 0, markers = 0, names = 0;
        std::uint64_t created = 0;
        std::uint32_t rerecords = 0;
        StartKind start = StartKind::Now;
        bool operator==(const Signature& o) const
        {
            return frames == o.frames && events == o.events && camera == o.camera && markers == o.markers &&
                   names == o.names && created == o.created && rerecords == o.rerecords && start == o.start;
        }
        bool operator!=(const Signature& o) const { return !(*this == o); }
    };
    static Signature Of(const Replay& r);
    void Trim();

    std::size_t budget_, maxSteps_;
    std::vector<Step> undo_, redo_;
    std::size_t bytes_ = 0;
    Signature known_{};
    bool expectChange_ = false;     // An edit was pushed: the next change Watch sees is ours
};

} // namespace tas
