// The installer's work on a game folder (installer/src/install.*), on a fake one in the temp folder: a MirrorsEdge.exe
// that is just a file, and stand-in DLLs embedded like the real ones (payload.rc). Never touches Windows' list of
// installed apps (SetAppRegistration(false)) or a real game folder.
#include "install.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

bool GameRunning()
{
    return install::Inspect(fs::temp_directory_path()).gameRunning;     // Any running MirrorsEdge.exe
}

std::string Read(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// <temp>/memenu-installer-test-<n>/ with a game root and Binaries\MirrorsEdge.exe, removed afterwards.
class FakeGame : public ::testing::Test {
protected:
    void SetUp() override
    {
        install::SetAppRegistration(false);
        root_ = fs::temp_directory_path() / ("memenu-installer-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root_);
        bin_ = root_ / "Binaries";
        Write(bin_ / "MirrorsEdge.exe", "not really the game");
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path root_, bin_;
};

} // namespace

TEST_F(FakeGame, FindsTheBinariesFolder)
{
    EXPECT_EQ(install::NormalizeGameDir(bin_ / "MirrorsEdge.exe"), bin_);     // The exe
    EXPECT_EQ(install::NormalizeGameDir(bin_), bin_);                          // Its folder
    EXPECT_EQ(install::NormalizeGameDir(root_), bin_);                         // The game's root folder
    EXPECT_FALSE(install::NormalizeGameDir(root_ / "nothing here").has_value());
    EXPECT_FALSE(install::NormalizeGameDir(fs::temp_directory_path()).has_value());
}

TEST_F(FakeGame, InstallsInspectsAndUninstalls)
{
    if (GameRunning()) GTEST_SKIP() << "Mirror's Edge is running: the installer refuses then";
    auto st = install::Inspect(bin_);
    EXPECT_TRUE(st.gameFound);
    EXPECT_FALSE(st.installed);

    std::wstring message;
    ASSERT_TRUE(install::Install(bin_, message)) << std::string(message.begin(), message.end());
    EXPECT_NE(message.find(L"Installed"), std::wstring::npos);
    EXPECT_EQ(Read(bin_ / "dinput8.dll"), Read(MEMENU_TEST_DATA "/fake_loader.bin"));
    EXPECT_EQ(Read(bin_ / "memenu" / "medge_menu.dll"), Read(MEMENU_TEST_DATA "/fake_menu.bin"));
    EXPECT_FALSE(fs::exists(bin_ / "memenu" / "uninstall.exe"));               // Not registered: no uninstaller copy
    for (const auto& e : fs::directory_iterator(bin_))                          // No half-written leftovers
        EXPECT_NE(e.path().extension(), ".new") << e.path();

    st = install::Inspect(bin_);
    EXPECT_TRUE(st.installed);
    EXPECT_EQ(st.installedVersion, "9.9.9");                                    // Read from the loader's marker
    EXPECT_FALSE(st.foreignLoader);
    EXPECT_FALSE(st.hasSettings);

    ASSERT_TRUE(install::Install(bin_, message));                              // Again: an update
    EXPECT_NE(message.find(L"Updated"), std::wstring::npos);

    ASSERT_TRUE(install::Uninstall(bin_, true, message));
    EXPECT_FALSE(fs::exists(bin_ / "dinput8.dll"));
    EXPECT_FALSE(fs::exists(bin_ / "memenu"));
    EXPECT_TRUE(fs::exists(bin_ / "MirrorsEdge.exe"));                          // The game is left alone
    EXPECT_FALSE(install::Inspect(bin_).installed);
}

TEST_F(FakeGame, KeepsSettingsWhenAsked)
{
    if (GameRunning()) GTEST_SKIP() << "Mirror's Edge is running: the installer refuses then";
    std::wstring message;
    ASSERT_TRUE(install::Install(bin_, message));
    Write(bin_ / "memenu" / "MedgeMenu.json", "{\"theme\": \"mine\"}");
    Write(bin_ / "memenu" / "replays" / "run.merp", "a run");
    EXPECT_TRUE(install::Inspect(bin_).hasSettings);

    ASSERT_TRUE(install::Uninstall(bin_, false, message));
    EXPECT_FALSE(fs::exists(bin_ / "dinput8.dll"));
    EXPECT_FALSE(fs::exists(bin_ / "memenu" / "medge_menu.dll"));
    EXPECT_EQ(Read(bin_ / "memenu" / "MedgeMenu.json"), "{\"theme\": \"mine\"}");
    EXPECT_TRUE(fs::exists(bin_ / "memenu" / "replays" / "run.merp"));
}

TEST_F(FakeGame, PutsAnotherModsLoaderBack)
{
    if (GameRunning()) GTEST_SKIP() << "Mirror's Edge is running: the installer refuses then";
    Write(bin_ / "dinput8.dll", "someone else's mod");
    EXPECT_TRUE(install::Inspect(bin_).foreignLoader);

    std::wstring message;
    ASSERT_TRUE(install::Install(bin_, message));
    EXPECT_NE(message.find(L"another mod"), std::wstring::npos);
    EXPECT_EQ(Read(bin_ / "dinput8.dll.memenu-backup"), "someone else's mod");
    EXPECT_TRUE(install::Inspect(bin_).installed);

    ASSERT_TRUE(install::Uninstall(bin_, true, message));
    EXPECT_EQ(Read(bin_ / "dinput8.dll"), "someone else's mod");                // Back where it was
    EXPECT_FALSE(fs::exists(bin_ / "dinput8.dll.memenu-backup"));
}

TEST_F(FakeGame, RefusesWithoutAGame)
{
    std::wstring message;
    EXPECT_FALSE(install::Install(root_ / "elsewhere", message));
    EXPECT_NE(message.find(L"MirrorsEdge.exe"), std::wstring::npos);
    EXPECT_FALSE(fs::exists(root_ / "elsewhere" / "dinput8.dll"));
}
