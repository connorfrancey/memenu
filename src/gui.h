#pragma once

namespace gui {
    inline bool visible = true;

    void ApplyStyle();           // call once after ImGui::CreateContext()
    void Render();               // call between ImGui::NewFrame() and ImGui::Render()
    bool CaptureKey(int vk);     // true if the key was consumed by "press a key to bind"

    // The self-test: this column (a module's tab) is drawn open even if the player collapsed it, until cleared ("");
    // then it is collapsed again if it was. Any thread.
    void OpenColumnForTest(const char* tab);
}
