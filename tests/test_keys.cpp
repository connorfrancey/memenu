// Key bindings (core/keys.h): combos with modifiers, which press fires which binding, names, and old configs.
#include "core/bindings.h"
#include "core/keys.h"

#include <Windows.h>
#include <gtest/gtest.h>

using namespace keys;

TEST(Keys, PlainKeysAreTheirVirtualKeyCode)
{
    // Configs from before combos hold plain virtual-key codes: they mean the same key with no modifier
    EXPECT_EQ(Vk(VK_F5), VK_F5);
    EXPECT_EQ(Mods(VK_F5), 0);
    EXPECT_EQ(Specificity(VK_F5), 0);
    const int combo = VK_F5 | kCtrl | kShift;
    EXPECT_EQ(Vk(combo), VK_F5);
    EXPECT_EQ(Mods(combo), kCtrl | kShift);
    EXPECT_EQ(Specificity(combo), 2);
}

TEST(Keys, WhichPressFiresWhichBinding)
{
    // A plain key fires whatever modifiers are held (F5 while holding Shift to slide)
    EXPECT_TRUE(Fires(VK_F5, VK_F5, 0));
    EXPECT_TRUE(Fires(VK_F5, VK_F5, kShift));
    EXPECT_FALSE(Fires(VK_F5, VK_F6, 0));
    EXPECT_FALSE(Fires(0, 0, 0));                         // Unbound never fires

    // A combo needs its modifiers, and doesn't mind extra ones
    EXPECT_FALSE(Fires(VK_F5 | kCtrl, VK_F5, 0));
    EXPECT_FALSE(Fires(VK_F5 | kCtrl, VK_F5, kShift));
    EXPECT_TRUE(Fires(VK_F5 | kCtrl, VK_F5, kCtrl));
    EXPECT_TRUE(Fires(VK_F5 | kCtrl, VK_F5, kCtrl | kAlt));
    EXPECT_FALSE(Fires(VK_F5 | kCtrl | kShift, VK_F5, kCtrl));

    // When F5 and Ctrl+F5 both fire, the dispatcher keeps the more specific one
    EXPECT_GT(Specificity(VK_F5 | kCtrl), Specificity(VK_F5));
}

TEST(Keys, Names)
{
    EXPECT_EQ(Name(0), "None");
    EXPECT_EQ(Name(VK_F5), "F5");
    EXPECT_EQ(Name(VK_F5 | kCtrl | kShift), "Ctrl+Shift+F5");
    EXPECT_EQ(Name('Q' | kAlt), "Alt+Q");
    EXPECT_EQ(Name(VK_XBUTTON1), "Mouse 4");
    EXPECT_EQ(Name(VK_MBUTTON | kCtrl), "Ctrl+Middle Mouse");
    EXPECT_TRUE(IsModifier(VK_LSHIFT));
    EXPECT_TRUE(IsModifier(VK_MENU));
    EXPECT_FALSE(IsModifier(VK_F5));
}

TEST(Keys, ClashesAreTheSamePress)
{
    EXPECT_TRUE(bindings::Clash(VK_F5, VK_F5));
    EXPECT_FALSE(bindings::Clash(VK_F5, VK_F5 | kCtrl));     // Different presses: Ctrl+F5 wins over F5
    EXPECT_FALSE(bindings::Clash(0, 0));                     // Unbound slots don't clash
}
