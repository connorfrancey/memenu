#include "features/tas/macro_library.h"

#include "core/notify.h"
#include "tas/tas.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace fs = std::filesystem;

namespace tasui {
namespace {

bool IsMacroFile(const fs::path& path)
{
    const auto ext = path.extension();
    return ext == tas::kFileExtension || ext == ".json";
}

bool ContainsNoCase(const std::string& text, const char* part)
{
    if (!part || !*part) return true;
    const std::string p = part;
    return std::search(text.begin(), text.end(), p.begin(), p.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != text.end();
}

constexpr ImGuiFileBrowserFlags kBrowserFlags = ImGuiFileBrowserFlags_CloseOnEsc | ImGuiFileBrowserFlags_ConfirmOnEnter |
                                                ImGuiFileBrowserFlags_SkipItemsCausingError |
                                                ImGuiFileBrowserFlags_EditPathString;

} // namespace

// ---- name, save, load ------------------------------------------------------------------------------------------------
void MacroLibrary::DrawNameBox(float width)
{
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint("##file", "macro name (or leave empty)", name_, sizeof name_);
    ImGui::SetItemTooltip("Save writes replays\\%s%s next to the menu DLL", name_[0] ? name_ : "<date and time>",
                          tas::kFileExtension);
}

void MacroLibrary::SetName(const fs::path& path)
{
    std::snprintf(name_, sizeof name_, "%s", path.stem().string().c_str());
    selected_ = path;
}

void MacroLibrary::Save()
{
    if (!name_[0]) {
        // A name from the date and time: saving is one click
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(name_, sizeof name_, "macro %Y-%m-%d %H-%M-%S", &tm);
    }
    SaveTo(tas::Folder() / (std::string(name_) + tas::kFileExtension));
}

void MacroLibrary::SaveTo(const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    tas::WithReplay([&](tas::Replay& r) {
        if (r.description.empty()) r.description = path.stem().string();
    });
    std::string error;
    if (tas::SaveFile(path, &error)) {
        notify::Push("Saved " + path.filename().string());
        SetName(path);
        unsaved_ = false;
    } else {
        notify::Push("TAS: " + error);
    }
    refreshedAt_ = -1.0;
}

void MacroLibrary::Load(const fs::path& path)
{
    std::string error;
    if (tas::LoadFile(path, &error)) {
        notify::Push("Loaded " + path.filename().string());
        SetName(path);
        unsaved_ = false;
    } else {
        notify::Push("TAS: " + error);
    }
}

fs::path MacroLibrary::NamedPath() const
{
    if (!selected_.empty() && selected_.stem().string() == name_) return selected_;
    std::error_code ec;
    const auto merp = tas::Folder() / (std::string(name_) + tas::kFileExtension);
    if (fs::exists(merp, ec)) return merp;
    const auto json = tas::Folder() / (std::string(name_) + ".json");
    return fs::exists(json, ec) ? json : merp;
}

void MacroLibrary::Guard(std::string what, std::function<void()> action)
{
    if (!tas::Unsaved()) {
        action();
        return;
    }
    guardWhat_ = std::move(what);
    guardAction_ = std::move(action);
    openGuard_ = true;
}

// ---- the folder ---------------------------------------------------------------------------------------------------
std::vector<MacroLibrary::File> MacroLibrary::Scan(const fs::path& folder)
{
    std::vector<File> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec) || !IsMacroFile(e.path())) continue;
        File f;
        f.path = e.path();
        f.label = e.path().filename().string();
        f.name = e.path().stem().string();
        f.size = e.file_size(ec);
        f.time = e.last_write_time(ec);
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const File& a, const File& b) { return a.time > b.time; });   // Newest first
    return out;
}

// The folders are small: looking again every couple of seconds keeps the lists current without a Refresh button.
void MacroLibrary::Refresh()
{
    const double now = ImGui::GetTime();
    if (refreshedAt_ < 0.0 || now - filesAt_ > 2.0) {
        files_ = Scan(tas::Folder());
        autosaves_ = Scan(tas::AutosaveFolder());
        filesAt_ = now;
    }
    if (refreshedAt_ < 0.0 || now - refreshedAt_ > 0.5) {
        unsaved_ = tas::Unsaved();
        refreshedAt_ = now;
    }
}

void MacroLibrary::DrawList(float width)
{
    Refresh();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float quarter = (width - 3 * spacing) / 4.f;
    const float rowH = ImGui::GetTextLineHeightWithSpacing();

    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint("##search", "search", search_, sizeof search_);
    if (ImGui::BeginListBox("##macros", {width, rowH * 5.f + 4.f})) {
        int shown = 0;
        for (const auto& f : files_) {
            if (!ContainsNoCase(f.label, search_)) continue;
            ++shown;
            if (ImGui::Selectable(f.label.c_str(), f.path == selected_, ImGuiSelectableFlags_AllowDoubleClick)) {
                SetName(f.path);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    const fs::path path = f.path;
                    Guard("Loading " + f.label, [this, path] { Load(path); });
                }
            }
            ImGui::SetItemTooltip("Double click to load (%.1f KB)", static_cast<double>(f.size) / 1024.0);
        }
        if (shown == 0) ImGui::TextDisabled(files_.empty() ? "No saved macros yet" : "No match");
        ImGui::EndListBox();
    }

    if (ImGui::Button("Open...", {quarter, 0})) OpenBrowser(false);
    ImGui::SetItemTooltip("Open a .merp or .json macro from anywhere");
    ImGui::SameLine();
    if (ImGui::Button("Save as...", {quarter, 0})) OpenBrowser(true);
    ImGui::SetItemTooltip("Save anywhere: .merp, or .json for readable, editable text");
    ImGui::SameLine();
    ImGui::BeginDisabled(!name_[0]);
    if (ImGui::Button(confirmDelete_ ? "Sure?" : "Delete", {quarter, 0})) {
        if (!confirmDelete_) {
            confirmDelete_ = true;
        } else {
            confirmDelete_ = false;
            const auto path = NamedPath();
            std::error_code ec;
            if (fs::remove(path, ec)) notify::Push("Deleted " + path.filename().string());
            else notify::Push("TAS: couldn't delete " + path.filename().string());
            if (path == selected_) selected_.clear();
            refreshedAt_ = -1.0;
        }
    }
    if (!ImGui::IsItemHovered()) confirmDelete_ = false;
    ImGui::SetItemTooltip("Delete the file named in the box (the macro in memory stays)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("New", {quarter, 0}))
        Guard("Starting a new macro", [this] {
            tas::Clear();
            name_[0] = 0;
            selected_.clear();
        });
    ImGui::SetItemTooltip("Stop and empty the macro in memory (saved files are kept)");

    // Every recording is kept here too: nothing recorded is ever lost
    if (!autosaves_.empty()) {
        char header[48];
        std::snprintf(header, sizeof header, "Autosaves (%zu)###autosaves", autosaves_.size());
        if (ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (const auto& f : autosaves_) {
                if (ImGui::Selectable(f.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    const fs::path path = f.path;
                    Guard("Loading an autosave", [this, path] {
                        Load(path);
                        name_[0] = 0;          // Saving it puts it with the others, under a name of its own
                        selected_.clear();
                    });
                }
                ImGui::SetItemTooltip("Double click to load. The newest %d recordings are kept here", tas::kAutosaves);
            }
            ImGui::TreePop();
        }
    }
}

// ---- browsers and questions ------------------------------------------------------------------------------------------
// Created the first time they are opened: the constructor reads the disk (folders, drive letters).
ImGui::FileBrowser& MacroLibrary::Browser(bool save)
{
    auto& browser = save ? saveBrowser_ : openBrowser_;
    if (!browser) {
        std::error_code ec;
        fs::create_directories(tas::Folder(), ec);
        browser.emplace(save ? kBrowserFlags | ImGuiFileBrowserFlags_EnterNewFilename | ImGuiFileBrowserFlags_CreateNewDir
                             : kBrowserFlags,
                        tas::Folder());
        browser->SetWindowSize(720, 460);
    }
    return *browser;
}

void MacroLibrary::OpenBrowser(bool save)
{
    ImGui::FileBrowser& b = Browser(save);
    b.SetTypeFilters({".merp", ".json"});
    if (save) {
        b.SetTitle("Save macro as");
        b.SetInputName(std::string(name_[0] ? name_ : "macro") + tas::kFileExtension);
    } else {
        b.SetTitle("Open macro");
    }
    b.Open();
}

void MacroLibrary::DrawPopups()
{
    Refresh();
    if (openBrowser_) {
        openBrowser_->Display();
        if (openBrowser_->HasSelected()) {
            const fs::path path = openBrowser_->GetSelected();
            openBrowser_->ClearSelected();
            Guard("Loading " + path.filename().string(), [this, path] { Load(path); });
        }
    }
    if (saveBrowser_) {
        saveBrowser_->Display();
        if (saveBrowser_->HasSelected()) {
            fs::path path = saveBrowser_->GetSelected();
            saveBrowser_->ClearSelected();
            if (!IsMacroFile(path)) path += tas::kFileExtension;     // .json only when asked for by name
            std::error_code ec;
            if (fs::exists(path, ec)) {
                overwrite_ = path;
                ImGui::OpenPopup("Replace file?##tas");
            } else {
                SaveTo(path);
            }
        }
    }

    if (ImGui::BeginPopupModal("Replace file?##tas", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s already exists. Replace it?", overwrite_.filename().string().c_str());
        ImGui::TextDisabled("%s", overwrite_.parent_path().string().c_str());
        if (ImGui::Button("Replace", {120.f, 0})) {
            SaveTo(overwrite_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120.f, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openGuard_) {
        ImGui::OpenPopup("Not saved##tas");
        openGuard_ = false;
    }
    if (ImGui::BeginPopupModal("Not saved##tas", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The macro in memory has changes that aren't saved.");
        ImGui::TextDisabled("%s replaces it. (Recordings are also kept in Autosaves.)", guardWhat_.c_str());
        const auto run = [this] {
            auto action = std::move(guardAction_);
            guardAction_ = nullptr;
            ImGui::CloseCurrentPopup();
            if (action) action();
        };
        if (ImGui::Button("Save it first", {130.f, 0})) {
            Save();
            run();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't save", {130.f, 0})) run();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.f, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            guardAction_ = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace tasui
