#include "core/module.h"
#include "theme.h"

namespace {

class Themes final : public ActionModule {
public:
    Themes() : ActionModule("Themes", "Misc", "Left click: reload theme files from disk", 30) {}

    void OnTrigger() override
    {
        theme::Rescan();
        theme::Apply(theme::Current());
    }

private:
    ctl::DynamicCombo theme_{this, "Theme",
        [] { return theme::Names(); },
        [] { return theme::CurrentIndex(); },
        [](int i) {
            const auto& names = theme::Names();
            if (i >= 0 && i < static_cast<int>(names.size())) theme::Apply(names[static_cast<std::size_t>(i)]);
        },
        "Theme files live in the themes/ folder next to the DLL"};
    ctl::Button openFolder{this, "Open themes folder", [] { theme::OpenFolder(); }};
};
MEM_MODULE(Themes)

} // namespace
