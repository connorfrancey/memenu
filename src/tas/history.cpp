#include "tas/history.h"

#include <utility>

namespace tas {

EditHistory::EditHistory(std::size_t budgetBytes, std::size_t maxSteps) : budget_(budgetBytes), maxSteps_(maxSteps) {}

std::size_t EditHistory::Bytes(const Replay& r)
{
    std::size_t n = sizeof(Replay) + r.frames.size() * sizeof(Frame) + r.events.size() * sizeof(Event) +
                    r.sync.size() * sizeof(Sync) + r.camera.size() * sizeof(CameraKey);
    for (const auto& s : r.names) n += s.size() + 32;
    for (const auto& m : r.markers) n += m.name.size() + 40;
    for (const auto& [k, v] : r.extensions) n += k.size() + v.size() + 64;
    return n;
}

EditHistory::Signature EditHistory::Of(const Replay& r)
{
    return {r.frames.size(), r.events.size(), r.camera.size(), r.markers.size(), r.names.size(), r.created,
            r.rerecords, r.start};
}

void EditHistory::Push(const Replay& r, std::string what)
{
    Step s{r, std::move(what), Bytes(r)};
    bytes_ += s.bytes;
    undo_.push_back(std::move(s));
    for (const Step& d : redo_) bytes_ -= d.bytes;
    redo_.clear();
    known_ = Of(r);
    expectChange_ = true;
    Trim();
}

// The oldest steps go until the rest fits (the newest one always stays).
void EditHistory::Trim()
{
    std::size_t drop = 0;
    while (undo_.size() - drop > 1 && (bytes_ > budget_ || undo_.size() - drop > maxSteps_)) bytes_ -= undo_[drop++].bytes;
    if (drop) undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(drop));
}

bool EditHistory::Undo(Replay& r)
{
    if (undo_.empty()) return false;
    Step back = std::move(undo_.back());
    undo_.pop_back();
    bytes_ -= back.bytes;
    Step now{std::move(r), back.what, 0};
    now.bytes = Bytes(now.replay);
    bytes_ += now.bytes;
    r = std::move(back.replay);
    redo_.push_back(std::move(now));
    known_ = Of(r);
    expectChange_ = false;
    return true;
}

bool EditHistory::Redo(Replay& r)
{
    if (redo_.empty()) return false;
    Step fwd = std::move(redo_.back());
    redo_.pop_back();
    bytes_ -= fwd.bytes;
    Step now{std::move(r), fwd.what, 0};
    now.bytes = Bytes(now.replay);
    bytes_ += now.bytes;
    r = std::move(fwd.replay);
    undo_.push_back(std::move(now));
    known_ = Of(r);
    expectChange_ = false;
    return true;
}

const std::string& EditHistory::UndoName() const
{
    static const std::string none;
    return undo_.empty() ? none : undo_.back().what;
}

const std::string& EditHistory::RedoName() const
{
    static const std::string none;
    return redo_.empty() ? none : redo_.back().what;
}

void EditHistory::Clear()
{
    undo_.clear();
    redo_.clear();
    bytes_ = 0;
    expectChange_ = false;
}

void EditHistory::Watch(const Replay& r)
{
    // An edit pushed since the last look is what changed it (an edit that keeps every count is fine too); anything
    // else means the replay was changed by something other than an edit: the steps don't fit it any more.
    const Signature now = Of(r);
    if (now != known_ && !expectChange_) Clear();
    expectChange_ = false;
    known_ = now;
}

} // namespace tas
