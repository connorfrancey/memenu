// Self-test mod: while its module is on it hooks Engine.Actor.CheckMaxEffectDistance before the call at priority 20
// and turns CullDistance into CullDistance * 10 + 1 (hook-lua, at priority 10, does * 10 + 2: from 0, the order shows
// as 1 then 12). Every tick it reads the pawn's Location.Z by name.
#include <mirage/mirage.hpp>

namespace
{
	class HookNative final : public mirage::Module
	{
	public:
		HookNative() : Module("Hook (native)", "Mods", "Self-test: hooks CheckMaxEffectDistance (priority 20)") {}

		void OnEnable() override
		{
			hook_ = mirage::Hook("Engine.Actor.CheckMaxEffectDistance", MIRAGE_HOOK_BEFORE, 20, [](mirage::Call& c) {
				const float v = c.Get<float>("CullDistance") * 10.f + 1.f;
				c.Set("CullDistance", v);
				mirage::Set("saw", v);
			});
		}
		void OnDisable() override
		{
			if (hook_) hook_->Remove();
			hook_ = nullptr;
		}
		void OnTick() override
		{
			if (const mirage::Object pawn = mirage::Object::Pawn()) mirage::Set("pawn_z", pawn.Get<float>("Location.Z"));
		}

	private:
		mirage::Hooked* hook_ = nullptr;
	};
	MIRAGE_MODULE(HookNative)
}

MIRAGE_MOD()
