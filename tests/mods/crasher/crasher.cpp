// Self-test mod: switching its module on writes through a null pointer. The runtime must catch it, name this mod and
// the place in this DLL, stop the mod (the rest go on) and not load it again next time (crashes.toml).
#include <mirage/mirage.hpp>

namespace
{
	class Crasher final : public mirage::Module
	{
	public:
		Crasher() : Module("Crasher", "Mods", "Crashes on purpose when switched on (self-test)") {}
		void OnEnable() override
		{
			volatile int* nothing = nullptr;
			*nothing = 42;
		}
	};
	MIRAGE_MODULE(Crasher)
}

MIRAGE_MOD()
