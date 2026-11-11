// Hello: a native Mirage mod. Built by its own CMake project (CMakeLists.txt next to this file) against the mod API
// headers only (api/mirage), with any compiler that targets 32-bit Windows.
//
// One module, "Hello (native)" in the Mods column: while it is on it draws a greeting, counts the game's ticks and
// remembers how many times it was switched on in its own config file (config.toml in the mod's folder).

#include <mirage/mirage.hpp>

#include <string>

namespace
{
	class Hello final : public mirage::Module
	{
	public:
		Hello() : Module("Hello (native)", "Mods", "A native example mod: draws a greeting while on", 10) {}

		void OnEnable() override
		{
			const int times = std::stoi(mirage::Config("times_enabled", "0")) + 1;
			mirage::SetConfig("times_enabled", std::to_string(times));
			status.Set("on (" + std::to_string(times) + " times so far)");
			mirage::Info("enabled, " + std::to_string(times) + " times so far");
		}

		void OnDisable() override { status.Set("off"); }

		void OnTick() override { ticks.Set(ticks.Get() + 1); }

		void OnRender() override
		{
			float w = 0, h = 0;
			mirage::ScreenSize(w, h);
			const std::string text = greeting.Get() + " (" + std::to_string(ticks.Get()) + " ticks)";
			const float x = w * 0.5f, y = h * position.Get();
			mirage::DrawRectFilled(x - 6.f, y - 4.f, x + 8.f * static_cast<float>(text.size()), y + 18.f,
								   mirage::Rgba(0, 0, 0, 140));
			mirage::DrawText(x, y, mirage::Rgba(120, 220, 255), text);
		}

	private:
		mirage::Text greeting{this, "Greeting", "Hello from a native mod", "What it draws"};
		mirage::Slider position{this, "Height", 0.2f, 0.f, 0.95f, "%.2f", "Where on the screen, from the top"};
		mirage::SliderInt ticks{this, "Ticks", 0, 0, 1 << 30, "Game ticks counted while on"};
		mirage::Text status{this, "Status", "off", "Set by the mod"};
	};
	MIRAGE_MODULE(Hello)
}

MIRAGE_MOD()
