#include "tas/replay.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <type_traits>

namespace tas {

namespace {

// ---- file layout ---------------------------------------------------------------------------------------------------------
//   "MERP"                         magic
//   u16 version, u16 flags (0)
//   chunks: u32 tag, u32 size, `size` bytes            unknown tags are skipped
//   "END " chunk: u64 FNV-1a of every byte before its tag
// All numbers little endian (x86). Strings are u32 length + UTF-8 bytes.
constexpr char kMagic[4] = {'M', 'E', 'R', 'P'};
constexpr std::uint16_t kVersion = 1;

constexpr std::uint32_t Tag(const char (&s)[5])
{
    return static_cast<std::uint32_t>(s[0]) | static_cast<std::uint32_t>(s[1]) << 8 |
           static_cast<std::uint32_t>(s[2]) << 16 | static_cast<std::uint32_t>(s[3]) << 24;
}
constexpr std::uint32_t kHead = Tag("HEAD");   // run settings and counts
constexpr std::uint32_t kMeta = Tag("META");   // strings: start command/level, author, description, tool
constexpr std::uint32_t kName = Tag("NAME");   // key name table
constexpr std::uint32_t kTime = Tag("TIME");   // f32 dt per frame. Only written when the frames don't all use 1/fixedHz
constexpr std::uint32_t kEvnt = Tag("EVNT");   // sparse inputs: u32 frame + Event, in frame order
constexpr std::uint32_t kSync = Tag("SYNC");   // Sync per frame
constexpr std::uint32_t kExtn = Tag("EXTN");   // key/value strings, free for anyone
constexpr std::uint32_t kEnd  = Tag("END ");

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t Fnv(const void* data, std::size_t size, std::uint64_t h = kFnvOffset)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * kFnvPrime;
    return h;
}

class Writer {
public:
    template <class T>
    void Put(const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        buf_.append(reinterpret_cast<const char*>(&v), sizeof v);
    }
    void Str(const std::string& s)
    {
        Put(static_cast<std::uint32_t>(s.size()));
        buf_.append(s);
    }
    void Raw(const void* p, std::size_t n) { buf_.append(static_cast<const char*>(p), n); }

    // Starts a chunk; the size is filled in by End().
    void Begin(std::uint32_t tag)
    {
        Put(tag);
        sizeAt_ = buf_.size();
        Put(std::uint32_t{0});
    }
    void End()
    {
        const auto size = static_cast<std::uint32_t>(buf_.size() - sizeAt_ - 4);
        std::memcpy(&buf_[sizeAt_], &size, 4);
    }

    std::string& Buffer() { return buf_; }

private:
    std::string buf_;
    std::size_t sizeAt_ = 0;
};

class Reader {
public:
    Reader(const char* data, std::size_t size) : p_(data), end_(data + size) {}

    template <class T>
    bool Get(T& v)
    {
        if (Left() < sizeof v) return false;
        std::memcpy(&v, p_, sizeof v);
        p_ += sizeof v;
        return true;
    }
    bool Str(std::string& s)
    {
        std::uint32_t n;
        if (!Get(n) || Left() < n) return false;
        s.assign(p_, n);
        p_ += n;
        return true;
    }
    bool Skip(std::size_t n)
    {
        if (Left() < n) return false;
        p_ += n;
        return true;
    }
    const char* Here() const { return p_; }
    std::size_t Left() const { return static_cast<std::size_t>(end_ - p_); }

private:
    const char* p_;
    const char* end_;
};

void Fail(std::string* error, std::string msg)
{
    if (error) *error = std::move(msg);
}

float NominalDt(float hz) { return hz > 0.f ? static_cast<float>(1.0 / hz) : 0.f; }

} // namespace

bool Sync::operator==(const Sync& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }

void Replay::Clear() { *this = Replay{}; }

double Replay::Duration() const
{
    double t = 0.0;
    for (const Frame& f : frames) t += f.dt;
    return t;
}

std::uint16_t Replay::NameIndex(const std::string& name)
{
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<std::uint16_t>(i);
    names.push_back(name);
    return static_cast<std::uint16_t>(names.size() - 1);
}

const std::string& Replay::Name(std::uint16_t i) const
{
    static const std::string unknown = "?";
    return i < names.size() ? names[i] : unknown;
}

void Replay::BeginFrame(float dt)
{
    Frame f;
    f.dt = dt;
    f.first = static_cast<std::uint32_t>(events.size());
    frames.push_back(f);
}

void Replay::AddEvent(const Event& e)
{
    if (frames.empty()) return;
    events.push_back(e);
    ++frames.back().count;
}

void Replay::Truncate(std::size_t frame)
{
    if (frame >= frames.size()) return;
    events.resize(frames[frame].first);
    frames.resize(frame);
    if (sync.size() > frame) sync.resize(frame);
    InvalidateSyncFrom(frame);
}

void Replay::InsertFrames(std::size_t at, std::size_t count, float dt)
{
    at = std::min(at, frames.size());
    Frame f;
    f.dt = dt;
    f.first = static_cast<std::uint32_t>(at < frames.size() ? frames[at].first : events.size());
    frames.insert(frames.begin() + at, count, f);
    if (!sync.empty()) sync.insert(sync.begin() + std::min(at, sync.size()), count, Sync{});
    InvalidateSyncFrom(at);
}

void Replay::EraseFrames(std::size_t at, std::size_t count)
{
    if (at >= frames.size()) return;
    count = std::min(count, frames.size() - at);
    const std::uint32_t evBegin = frames[at].first;
    const std::uint32_t evEnd = at + count < frames.size() ? frames[at + count].first : static_cast<std::uint32_t>(events.size());
    events.erase(events.begin() + evBegin, events.begin() + evEnd);
    frames.erase(frames.begin() + at, frames.begin() + at + count);
    for (std::size_t i = at; i < frames.size(); ++i) frames[i].first -= evEnd - evBegin;
    if (sync.size() > at) sync.erase(sync.begin() + at, sync.begin() + std::min(sync.size(), at + count));
    InvalidateSyncFrom(at);
}

void Replay::InsertEvent(std::size_t frame, std::size_t indexInFrame, const Event& e)
{
    if (frame >= frames.size()) return;
    Frame& f = frames[frame];
    indexInFrame = std::min<std::size_t>(indexInFrame, f.count);
    events.insert(events.begin() + f.first + indexInFrame, e);
    ++f.count;
    for (std::size_t i = frame + 1; i < frames.size(); ++i) ++frames[i].first;
    InvalidateSyncFrom(frame + 1);
}

void Replay::EraseEvent(std::size_t frame, std::size_t indexInFrame)
{
    if (frame >= frames.size() || indexInFrame >= frames[frame].count) return;
    events.erase(events.begin() + frames[frame].first + indexInFrame);
    --frames[frame].count;
    for (std::size_t i = frame + 1; i < frames.size(); ++i) --frames[i].first;
    InvalidateSyncFrom(frame + 1);
}

void Replay::ReplaceEvent(std::size_t frame, std::size_t indexInFrame, const Event& e)
{
    if (frame >= frames.size() || indexInFrame >= frames[frame].count) return;
    events[frames[frame].first + indexInFrame] = e;
    InvalidateSyncFrom(frame + 1);
}

void Replay::SetFrameDt(std::size_t frame, float dt)
{
    if (frame >= frames.size() || frames[frame].dt == dt) return;
    frames[frame].dt = dt;
    InvalidateSyncFrom(frame + 1);
}

void Replay::InvalidateSyncFrom(std::size_t frame)
{
    syncValidUntil = std::min<std::uint32_t>(syncValidUntil, static_cast<std::uint32_t>(frame));
}

std::size_t Replay::RenameKey(const std::string& from, const std::string& to)
{
    std::uint16_t src = 0xFFFF;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == from) src = static_cast<std::uint16_t>(i);
    if (src == 0xFFFF || from == to) return 0;

    const std::uint16_t dst = NameIndex(to);
    std::size_t changed = 0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        for (std::uint32_t i = 0; i < frames[f].count; ++i) {
            Event& e = events[frames[f].first + i];
            if (e.name != src) continue;
            e.name = dst;
            ++changed;
            InvalidateSyncFrom(f + 1);
        }
    }
    return changed;
}

std::uint64_t Replay::InputHash() const
{
    std::uint64_t h = kFnvOffset;
    h = Fnv(&start, sizeof start, h);
    h = Fnv(command.data(), command.size(), h);
    h = Fnv(map.data(), map.size(), h);
    h = Fnv(checkpoint.data(), checkpoint.size(), h);
    h = Fnv(&seed, sizeof seed, h);
    for (const Frame& f : frames) {
        h = Fnv(&f.dt, sizeof f.dt, h);
        for (std::uint32_t i = 0; i < f.count; ++i) {
            const Event& e = events[f.first + i];
            const std::string& n = Name(e.name);      // by name: the order of the name table doesn't matter
            h = Fnv(n.data(), n.size(), h);
            h = Fnv(&e.kind, sizeof e.kind, h);
            h = Fnv(&e.gamepad, sizeof e.gamepad, h);
            h = Fnv(&e.value, sizeof e.value, h);
            h = Fnv(&e.value2, sizeof e.value2, h);
        }
    }
    return h;
}

std::uint64_t Replay::SyncHash() const
{
    return sync.empty() ? 0 : Fnv(sync.data(), sync.size() * sizeof(Sync));
}

bool Replay::Save(const std::filesystem::path& path, std::string* error) const
{
    Writer w;
    w.Raw(kMagic, sizeof kMagic);
    w.Put(kVersion);
    w.Put(std::uint16_t{0});

    w.Begin(kHead);
    w.Put(start);
    w.Put(fixedHz);
    w.Put(seed);
    w.Put(gameStamp);
    w.Put(rerecords);
    w.Put(created);
    w.Put(static_cast<std::uint32_t>(frames.size()));
    w.Put(syncValidUntil);
    w.Put(Duration());
    w.End();

    w.Begin(kMeta);
    for (const std::string* s : {&command, &map, &checkpoint, &author, &description, &botName, &botVersion}) w.Str(*s);
    w.End();

    w.Begin(kName);
    w.Put(static_cast<std::uint32_t>(names.size()));
    for (const auto& n : names) w.Str(n);
    w.End();

    const float nominal = NominalDt(fixedHz);
    if (std::any_of(frames.begin(), frames.end(), [&](const Frame& f) { return f.dt != nominal; })) {
        w.Begin(kTime);
        for (const Frame& f : frames) w.Put(f.dt);
        w.End();
    }

    w.Begin(kEvnt);
    w.Put(static_cast<std::uint32_t>(events.size()));
    for (std::size_t f = 0; f < frames.size(); ++f) {
        for (std::uint32_t i = 0; i < frames[f].count; ++i) {
            w.Put(static_cast<std::uint32_t>(f));
            w.Put(events[frames[f].first + i]);
        }
    }
    w.End();

    if (!sync.empty()) {
        w.Begin(kSync);
        w.Raw(sync.data(), sync.size() * sizeof(Sync));
        w.End();
    }

    if (!extensions.empty()) {
        w.Begin(kExtn);
        w.Put(static_cast<std::uint32_t>(extensions.size()));
        for (const auto& [k, v] : extensions) { w.Str(k); w.Str(v); }
        w.End();
    }

    const std::uint64_t checksum = Fnv(w.Buffer().data(), w.Buffer().size());
    w.Begin(kEnd);
    w.Put(checksum);
    w.End();

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    // Write a temporary file and swap it in, so a crash mid-write never destroys the previous version.
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { Fail(error, "can't write " + tmp.string()); return false; }
        out.write(w.Buffer().data(), static_cast<std::streamsize>(w.Buffer().size()));
        if (!out) { Fail(error, "write failed: " + tmp.string()); return false; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { Fail(error, "can't replace " + path.string() + ": " + ec.message()); return false; }
    return true;
}

bool Replay::Load(const std::filesystem::path& path, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { Fail(error, "can't open " + path.string()); return false; }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    if (data.size() < 8 || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
        Fail(error, "not a .merp file");
        return false;
    }

    // The END chunk is always the last 16 bytes: check the whole file before parsing any of it.
    constexpr std::size_t kEndSize = 4 + 4 + 8;
    std::uint32_t endTag = 0, endSize = 0;
    std::uint64_t stored = 0;
    if (data.size() >= 8 + kEndSize) {
        const char* end = data.data() + data.size() - kEndSize;
        std::memcpy(&endTag, end, 4);
        std::memcpy(&endSize, end + 4, 4);
        std::memcpy(&stored, end + 8, 8);
    }
    if (endTag != kEnd || endSize != 8) { Fail(error, "the file is truncated"); return false; }
    if (Fnv(data.data(), data.size() - kEndSize) != stored) {
        Fail(error, "the file is damaged (checksum mismatch)");
        return false;
    }

    Reader r(data.data(), data.size());
    r.Skip(sizeof kMagic);
    std::uint16_t version = 0, flags = 0;
    r.Get(version);
    r.Get(flags);
    if (version > kVersion) {
        Fail(error, "made by a newer version (format " + std::to_string(version) + ")");
        return false;
    }

    Replay rep;
    std::uint32_t frameCount = 0;
    std::vector<float> dts;
    std::vector<std::pair<std::uint32_t, Event>> sparse;
    bool head = false, end = false;

    while (!end) {
        const std::size_t chunkStart = static_cast<std::size_t>(r.Here() - data.data());
        std::uint32_t tag = 0, size = 0;
        if (!r.Get(tag) || !r.Get(size) || r.Left() < size) { Fail(error, "the file is truncated"); return false; }
        Reader c(r.Here(), size);
        r.Skip(size);

        bool ok = true;
        if (tag == kHead) {
            double duration = 0.0;
            ok = c.Get(rep.start) && c.Get(rep.fixedHz) && c.Get(rep.seed) && c.Get(rep.gameStamp) &&
                 c.Get(rep.rerecords) && c.Get(rep.created) && c.Get(frameCount) && c.Get(rep.syncValidUntil) &&
                 c.Get(duration);
            head = ok;
        } else if (tag == kMeta) {
            for (std::string* s : {&rep.command, &rep.map, &rep.checkpoint, &rep.author, &rep.description,
                                   &rep.botName, &rep.botVersion})
                ok = ok && c.Str(*s);
        } else if (tag == kName) {
            std::uint32_t n = 0;
            ok = c.Get(n) && n <= 0xFFFF;
            for (std::uint32_t i = 0; ok && i < n; ++i) {
                std::string s;
                ok = c.Str(s);
                rep.names.push_back(std::move(s));
            }
        } else if (tag == kTime) {
            dts.resize(size / sizeof(float));
            ok = size % sizeof(float) == 0;
            if (ok && !dts.empty()) std::memcpy(dts.data(), c.Here(), size);
        } else if (tag == kEvnt) {
            std::uint32_t n = 0;
            ok = c.Get(n) && c.Left() / 16 >= n;
            sparse.reserve(n);
            for (std::uint32_t i = 0; ok && i < n; ++i) {
                std::uint32_t frame;
                Event e;
                ok = c.Get(frame) && c.Get(e);
                sparse.emplace_back(frame, e);
            }
        } else if (tag == kSync) {
            ok = size % sizeof(Sync) == 0;
            rep.sync.resize(size / sizeof(Sync));
            if (ok && !rep.sync.empty()) std::memcpy(rep.sync.data(), c.Here(), size);
        } else if (tag == kExtn) {
            std::uint32_t n = 0;
            ok = c.Get(n);
            for (std::uint32_t i = 0; ok && i < n; ++i) {
                std::string k, v;
                ok = c.Str(k) && c.Str(v);
                rep.extensions[k] = v;
            }
        } else if (tag == kEnd) {
            end = chunkStart + kEndSize == data.size();     // already verified above
            ok = end;
        }   // unknown chunk: skipped
        if (!ok) { Fail(error, "bad chunk in the file"); return false; }
    }
    if (!head) { Fail(error, "no header chunk"); return false; }

    // Rebuild the per-frame table from the sparse events.
    if (!dts.empty() && dts.size() != frameCount) { Fail(error, "time table doesn't match the frame count"); return false; }
    const float nominal = NominalDt(rep.fixedHz);
    rep.frames.resize(frameCount);
    for (std::uint32_t f = 0; f < frameCount; ++f) rep.frames[f].dt = dts.empty() ? nominal : dts[f];

    std::uint32_t last = 0;
    rep.events.reserve(sparse.size());
    for (const auto& [frame, e] : sparse) {
        if (frame >= frameCount || frame < last) { Fail(error, "events out of order"); return false; }
        if (e.name >= rep.names.size()) { Fail(error, "bad key name index"); return false; }
        last = frame;
        rep.events.push_back(e);
        ++rep.frames[frame].count;
    }
    std::uint32_t first = 0;
    for (Frame& f : rep.frames) { f.first = first; first += f.count; }
    if (!rep.sync.empty() && rep.sync.size() != frameCount) rep.sync.clear();
    rep.syncValidUntil = std::min<std::uint32_t>(rep.syncValidUntil, frameCount);

    *this = std::move(rep);
    return true;
}

bool Replay::ExportJson(const std::filesystem::path& path, std::string* error) const
{
    using json = nlohmann::ordered_json;
    char hash[2][20];
    std::snprintf(hash[0], sizeof hash[0], "%016llx", static_cast<unsigned long long>(InputHash()));
    std::snprintf(hash[1], sizeof hash[1], "%016llx", static_cast<unsigned long long>(SyncHash()));

    json j;
    j["format"] = "Mirror's Edge Replay";
    j["version"] = kVersion;
    j["author"] = author;
    j["description"] = description;
    j["duration"] = Duration();
    j["gameVersion"] = gameStamp;
    j["bot"] = {{"name", botName}, {"version", botVersion}};
    j["start"] = {{"kind", StartName(start)}, {"command", command}, {"map", map}, {"checkpoint", checkpoint}};
    j["framerate"] = fixedHz;
    j["seed"] = seed;
    j["rerecords"] = rerecords;
    j["created"] = created;
    j["frames"] = frames.size();
    j["inputHash"] = hash[0];
    j["syncHash"] = hash[1];
    if (!extensions.empty()) j["extensions"] = extensions;

    const float nominal = NominalDt(fixedHz);
    if (std::any_of(frames.begin(), frames.end(), [&](const Frame& f) { return f.dt != nominal; })) {
        json dt = json::array();
        for (const Frame& f : frames) dt.push_back(f.dt);
        j["frameTimes"] = std::move(dt);
    }

    json inputs = json::array();
    for (std::size_t f = 0; f < frames.size(); ++f) {
        for (std::uint32_t i = 0; i < frames[f].count; ++i) {
            const Event& e = events[frames[f].first + i];
            json in = {{"frame", f}, {"key", Name(e.name)}, {"event", KindName(e.kind)}, {"value", e.value}};
            if (e.kind == EventKind::Axis) in["deltaTime"] = e.value2;
            if (e.gamepad) in["gamepad"] = true;
            inputs.push_back(std::move(in));
        }
    }
    j["inputs"] = std::move(inputs);

    std::ofstream out(path, std::ios::trunc);
    if (!out) { Fail(error, "can't write " + path.string()); return false; }
    out << j.dump(1, '\t');
    return static_cast<bool>(out);
}

const char* KindName(EventKind kind)
{
    switch (kind) {
        case EventKind::Pressed:     return "down";
        case EventKind::Released:    return "up";
        case EventKind::Repeat:      return "repeat";
        case EventKind::DoubleClick: return "double";
        case EventKind::Axis:        return "axis";
    }
    return "?";
}

const char* StartName(StartKind kind)
{
    switch (kind) {
        case StartKind::Now:        return "now";
        case StartKind::Command:    return "command";
        case StartKind::Checkpoint: return "checkpoint";
    }
    return "?";
}

std::string Describe(const Replay& r, const Event& e)
{
    char buf[96];
    if (e.kind == EventKind::Axis)
        std::snprintf(buf, sizeof buf, "%s %+.3f", r.Name(e.name).c_str(), e.value);
    else
        std::snprintf(buf, sizeof buf, "%s %s", r.Name(e.name).c_str(), KindName(e.kind));
    return buf;
}

} // namespace tas
