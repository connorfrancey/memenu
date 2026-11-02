#include "core/module.h"
#include "game/levels.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "THE SHARD" -> "The Shard"
std::string TitleCase(const std::string& s)
{
    std::string out = s;
    bool start = true;
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        c = static_cast<char>(start ? std::toupper(u) : std::tolower(u));
        start = c == ' ' || c == '-';
    }
    return out;
}

// What the game's label "CHAPTER 9 - THE SHARD (Scraper_p)" says, for a short row: "9" and "The Shard".
struct Title {
    std::string chapter, name;
};

Title MapTitle(const levels::MapView& map)
{
    std::string label = map.label;
    if (const std::size_t paren = label.rfind(" ("); paren != std::string::npos) label.resize(paren);
    Title t;
    if (Lower(label).rfind("chapter ", 0) == 0) {
        const std::size_t dash = label.find(" - ");
        t.chapter = label.substr(8, dash == std::string::npos ? std::string::npos : dash - 8);
        t.name = dash == std::string::npos ? std::string() : label.substr(dash + 3);
    } else {
        t.name = label;
    }
    t.name = TitleCase(t.name.empty() ? map.file : t.name);
    return t;
}

// "CHECKPOINT A (Scraper_Start)" -> "A", or the game's friendly name when it has a real one.
std::string CheckpointTitle(const std::string& label)
{
    std::string s = label;
    if (const std::size_t paren = s.rfind(" ("); paren != std::string::npos) s.resize(paren);
    if (Lower(s).rfind("checkpoint ", 0) == 0) s = s.substr(11);
    return TitleCase(s);
}

bool Contains(const std::string& text, const std::string& lowerPart)
{
    return lowerPart.empty() || Lower(text).find(lowerPart) != std::string::npos;
}

// The levels in story order with their checkpoints: click a level to open it, then "Start" or a checkpoint to load it.
// The level you are in is marked and open. Maps and checkpoints come from the game's own level-select data; loads use
// the same StartGame() the game's menu does.
class LevelLoader final : public ActionModule {
public:
    LevelLoader() : ActionModule("Levels", "Levels", "Run: read the level list from the game again", 10)
    {
        expanded = true;
        consoleLoad.VisibleIf([this] { return optionsSection.Get(); });
        saveProgress.VisibleIf([this] { return optionsSection.Get(); });
    }

    void OnTrigger() override { levels::Refresh(); }

private:
    void DrawList(float width)
    {
        const auto method = consoleLoad ? levels::Method::Console : levels::Method::Game;
        const bool save = saveProgress;
        const auto maps = levels::Maps();
        if (maps.empty()) {
            ImGui::TextDisabled("No levels yet");
            return;
        }
        const std::string current = Lower(levels::CurrentMap());

        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##find", "find a level or checkpoint", find_, sizeof find_);
        const std::string find = Lower(find_);

        const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const ImVec4 here = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);

        // The game lists them last to first: drawn the other way round. Indices stay the game's.
        for (int i = static_cast<int>(maps.size()) - 1; i >= 0; --i) {
            const auto& map = maps[static_cast<std::size_t>(i)];
            if (Lower(map.file) == "tdmainmenu") continue;         // The Main Menu module does that
            const Title title = MapTitle(map);
            const bool isHere = !current.empty() && Lower(map.file) == current;

            // With a search: the level shows if it or one of its checkpoints matches, and opens to show them.
            const bool levelMatch = Contains(title.name, find) || Contains(map.file, find);
            bool anyCheckpoint = false;
            for (std::size_t j = 0; j < map.checkpoints.size() && !find.empty(); ++j)
                anyCheckpoint |= Contains(map.checkpoints[j], find);
            if (!levelMatch && !anyCheckpoint) continue;

            ImGui::PushID(i);
            if (!find.empty()) ImGui::SetNextItemOpen(true);
            else if (isHere && openedFor_ != current) { ImGui::SetNextItemOpen(true); openedFor_ = current; }
            const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
            if (isHere) ImGui::PushStyleColor(ImGuiCol_Text, here);
            const bool open = ImGui::TreeNodeEx("##map", flags, "%s", title.name.c_str());
            if (isHere) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s%s\n%zu checkpoints", map.label.c_str(), isHere ? "\nYou are here" : "",
                                  map.checkpoints.size());
            // Right end of the row: "here" for the level you are in, else its chapter number
            const char* tag = isHere ? "here" : title.chapter.c_str();
            if (tag[0]) {
                ImGui::SameLine(width - ImGui::CalcTextSize(tag).x - ImGui::GetStyle().FramePadding.x);
                ImGui::TextColored(isHere ? here : dim, "%s", tag);
            }
            if (open) {
                const float rowW = width - ImGui::GetStyle().IndentSpacing;   // The tree indents its children
                if ((levelMatch || find.empty()) && ImGui::Selectable("Start", false, 0, {rowW, 0}))
                    levels::Load(i, 0, method, save);
                for (int j = 0; j < static_cast<int>(map.checkpoints.size()); ++j) {
                    const std::string& label = map.checkpoints[static_cast<std::size_t>(j)];
                    if (!levelMatch && !Contains(label, find)) continue;
                    const std::string name = j < static_cast<int>(map.checkpointNames.size())
                                                 ? map.checkpointNames[static_cast<std::size_t>(j)] : std::string();
                    ImGui::PushID(j);
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable(("##cp" + std::to_string(j)).c_str(), false, 0, {rowW, 0}))
                        levels::Load(i, j + 1, method, save);
                    ImGui::SetItemTooltip("%s", label.c_str());
                    // The short title, and the game's name of the place dimmed after it
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const std::string shortTitle = CheckpointTitle(label);
                    dl->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), shortTitle.c_str());
                    if (!name.empty() && Lower(name) != Lower(shortTitle)) {
                        std::string place = name;
                        std::replace(place.begin(), place.end(), '_', ' ');
                        const float x = at.x + std::max(ImGui::CalcTextSize(shortTitle.c_str()).x + 10.f, 34.f);
                        dl->PushClipRect({x, at.y}, {at.x + rowW, at.y + ImGui::GetTextLineHeight()}, true);
                        dl->AddText({x, at.y}, ImGui::GetColorU32(dim), place.c_str());
                        dl->PopClipRect();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    char find_[48] = "";
    std::string openedFor_;     // The level opened for being the one you're in (the next one opens when you move)

    ctl::Custom list{this, [this](float w) { DrawList(w); }};
    ctl::Section optionsSection{this, "Load options", false};
    ctl::Checkbox consoleLoad{this, "Console load", false,
                              "Use the console command 'open <map>' instead of the game's StartGame. Ignores checkpoints"};
    ctl::Checkbox saveProgress{this, "Save progress", false, "Let loads made here overwrite your saved checkpoint progress"};
};
MEM_MODULE(LevelLoader)

} // namespace
