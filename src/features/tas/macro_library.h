#pragma once
#include <imgui.h>
#include <imfilebrowser.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The TAS module's macro files: the name of the macro in memory, Save / Load into the replays folder, the list of saved
// macros and autosaves, the file browsers for anywhere else, and the "not saved yet" question before something replaces
// the macro in memory. Render thread only (it's all menu).
namespace tasui {

class MacroLibrary {
public:
    // The name box: what Save writes to (replays/<name>.merp). Empty for a new macro.
    void DrawNameBox(float width);
    const char* Name() const { return name_; }

    // Saves to replays/<name>.merp. A macro without a name gets one from the date and time.
    void Save();
    void Load(const std::filesystem::path& path);
    void OpenBrowser(bool save);        // Open... / Save as... anywhere
    // Runs `action` now, or once the player said so when the macro in memory isn't saved (`what` replaces it:
    // "Recording", "Loading", ...).
    void Guard(std::string what, std::function<void()> action);
    bool Unsaved() const { return unsaved_; }    // Refreshed twice a second

    // The saved macros (newest first, with a search box), Open / Save as / Delete / New, and the autosaves.
    void DrawList(float width);
    // The file browsers and questions. Every frame while the menu is open.
    void DrawPopups();

private:
    struct File {
        std::filesystem::path path;
        std::string label;              // file name: "run.merp"
        std::string name;               // without the extension
        std::uintmax_t size = 0;
        std::filesystem::file_time_type time;
    };

    void Refresh();
    static std::vector<File> Scan(const std::filesystem::path& folder);
    std::filesystem::path NamedPath() const;     // The file the name means (picked in the list, .merp, .json)
    void SaveTo(const std::filesystem::path& path);
    void SetName(const std::filesystem::path& path);
    ImGui::FileBrowser& Browser(bool save);

    char name_[96] = "";
    char search_[64] = "";
    std::filesystem::path selected_;    // Picked in the list, or the last loaded / saved
    std::vector<File> files_, autosaves_;
    double refreshedAt_ = -1.0;         // < 0: look at everything again now
    double filesAt_ = 0.0;
    bool unsaved_ = false;
    bool confirmDelete_ = false;
    std::optional<ImGui::FileBrowser> openBrowser_, saveBrowser_;
    std::filesystem::path overwrite_;   // "Replace file?" asks about this one
    std::string guardWhat_;             // "Not saved" asks before this
    std::function<void()> guardAction_;
    bool openGuard_ = false;
};

} // namespace tasui
