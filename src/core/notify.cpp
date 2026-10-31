#include "notify.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr float kShowSeconds = 2.5f;     // Fully visible
    constexpr float kFadeSeconds = 0.5f;     // Then fades out
    constexpr std::size_t kMaxShown = 6;

    struct Message {
        std::string text;
        Clock::time_point since;
    };

    std::mutex g_mutex;
    std::deque<Message> g_messages;
}

void notify::Push(std::string text)
{
    std::lock_guard lock(g_mutex);
    // The same message again (holding a key, toggling twice) replaces the old one instead of stacking.
    g_messages.erase(std::remove_if(g_messages.begin(), g_messages.end(), [&](const Message& m) { return m.text == text; }),
                     g_messages.end());
    g_messages.push_back({std::move(text), Clock::now()});
    while (g_messages.size() > kMaxShown) g_messages.pop_front();
}

void notify::Draw()
{
    std::deque<Message> messages;
    {
        std::lock_guard lock(g_mutex);
        const auto now = Clock::now();
        while (!g_messages.empty() &&
               std::chrono::duration<float>(now - g_messages.front().since).count() > kShowSeconds + kFadeSeconds)
            g_messages.pop_front();
        messages = g_messages;
    }
    if (messages.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float lineH = ImGui::GetFontSize() + 8.f;
    const auto now = Clock::now();

    float y = display.y * 0.75f;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const float age = std::chrono::duration<float>(now - it->since).count();
        const float alpha = age < kShowSeconds ? 1.f : std::clamp(1.f - (age - kShowSeconds) / kFadeSeconds, 0.f, 1.f);
        const ImVec2 size = ImGui::CalcTextSize(it->text.c_str());
        const ImVec2 pos{(display.x - size.x) * 0.5f, y};
        const auto a = static_cast<int>(alpha * 255.f);

        dl->AddRectFilled({pos.x - 8.f, pos.y - 4.f}, {pos.x + size.x + 8.f, pos.y + size.y + 4.f},
                          IM_COL32(15, 15, 18, a * 200 / 255), 4.f);
        dl->AddText(pos, IM_COL32(235, 235, 240, a), it->text.c_str());
        y -= lineH;
    }
}
