#include "tas/input_monitor.h"

#include "tas/engine.h"

#include "sdk/SDK.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <mutex>

namespace tas::input {
namespace {

std::mutex g_mutex;                                     // Guards everything but g_names
std::unordered_map<std::string, Key> g_keys;
struct Move { double at; float x, y; };
std::deque<Move> g_moves;                               // The last second of mouse movement
std::deque<double> g_presses;                           // The last second of presses
std::unordered_map<long long, std::string> g_names;     // Game thread: FName -> lower case name
bool g_installed = false;

const std::string& NameOf(int index, int number)
{
    const long long key = (static_cast<long long>(index) << 32) | static_cast<unsigned>(number);
    auto it = g_names.find(key);
    if (it != g_names.end()) return it->second;
    Classes::FName n(index);
    n.Number = number;
    std::string s = n.GetName();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return g_names.emplace(key, std::move(s)).first->second;
}

void Monitor(const engine::RawInput& in)
{
    const std::string& name = NameOf(in.nameIndex, in.nameNumber);
    const double now = Now();
    std::lock_guard lock(g_mutex);
    if (in.axis) {
        if (name == "mousex") g_moves.push_back({now, in.value, 0.f});
        else if (name == "mousey") g_moves.push_back({now, 0.f, in.value});
        while (!g_moves.empty() && g_moves.front().at < now - 1.0) g_moves.pop_front();
        return;
    }
    Key& k = g_keys[name];
    switch (in.event) {
        case 0:     // Pressed
        case 3:     // DoubleClick: the engine's second press of a mouse button
            if (!k.held) {
                k.held = true;
                k.down = now;
                ++k.presses;
                g_presses.push_back(now);
            }
            break;
        case 1:     // Released
            k.held = false;
            k.up = now;
            break;
        default:    // Repeat: still held
            break;
    }
    while (!g_presses.empty() && g_presses.front() < now - 1.0) g_presses.pop_front();
}

} // namespace

bool Install()
{
    if (g_installed) return true;
    if (!engine::Available()) return false;
    engine::SetInputMonitor(&Monitor);
    g_installed = true;
    return true;
}

bool Installed() { return g_installed; }

double Now() { return engine::RealSeconds(); }

Snapshot Get(double window)
{
    Snapshot s;
    s.now = Now();
    std::lock_guard lock(g_mutex);
    s.keys = g_keys;
    for (const Move& m : g_moves)
        if (m.at >= s.now - window) {
            s.mouseX += m.x;
            s.mouseY += m.y;
        }
    for (double t : g_presses)
        if (t >= s.now - 1.0) ++s.kps;
    return s;
}

void ResetCounts()
{
    std::lock_guard lock(g_mutex);
    for (auto& [name, k] : g_keys) k.presses = 0;
}

} // namespace tas::input
