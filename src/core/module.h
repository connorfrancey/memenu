#pragma once
#include "scheduler.h"
#include "setting.h"

#include <atomic>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A feature of the menu: a row you switch on and off, with its own settings.
//
// Every module is a class in its own file that derives from Module (or ActionModule), declares its controls as members
// and overrides the hooks it needs. It is registered with MEM_MODULE and nothing else has to know about it, there is no
// central list. See features/ for examples.
//
//     class Crosshair final : public Module {
//     public:
//         Crosshair() : Module("Crosshair", "Visual", "...") {}
//         void OnRender() override { ... }
//     private:
//         ctl::SliderInt size{this, "Size", 8, 2, 40};
//     };
//     MEM_MODULE(Crosshair)
//
// `tab` is the menu column it is listed in (tabs are created on demand), `order` its position there: lower first, then
// by name. The rest (default key, on by default, ...) are the public fields below, set in the constructor.
class Module 
{
public:
	Module(std::string name, std::string tab, std::string description, int order = 100);
	virtual ~Module() = default;
	Module(const Module&) = delete;
	Module& operator=(const Module&) = delete;
	
	const std::string& Name() const { return name_; }
	const std::string& Tab() const { return tab_; }
	const std::string& Description() const { return description_; }
	int Order() const { return order_; }
	
	virtual bool IsAction() const { return false; }
	bool Enabled() const { return enabled_; }
	void SetEnabled(bool on);					// Calls OnEnable/OnDisable, and switches off the rest of its group
	void Toggle();								// Action modules run OnTrigger instead
	
	// ---- Hooks ------------------------------------------------------------------------------------------------------------
	virtual void OnEnable() {}
	virtual void OnDisable() {}					// Also on unload: undo every change you made. May be on any thread, hand work for the game to game::RunOnGameThread
	virtual void OnTick() {}					// Game thread, every engine tick while enabled
	virtual void OnRender() {}					// Render thread, inside the ImGui frame, while enabled (HUD, overlays)
	virtual void OnTrigger() {}					// Action modules: clicked, or its key was pressed
	
	// ---- Scheduling (core/scheduler.h) ------------------------------------------------------------------------------------
	// When OnTick runs: interval, priority, dependencies. Set it in the constructor. The default runs every tick.
	sched::Schedule schedule;
	// Asked (game thread) when the module is due: false = nothing to do, skip OnTick this time.
	virtual bool NeedsUpdate() { return true; }
	// Run OnTick at the next tick even if it isn't due. Any thread. Editing one of its controls does this.
	void MarkDirty() { dirty_.store(true, std::memory_order_relaxed); }
	bool TakeDirty() { return dirty_.exchange(false, std::memory_order_relaxed); }
	
	// ---- Controls and state ------------------------------------------------------------------------------------------------
	const std::vector<ctl::Setting*>& Settings() const { return settings_; }
	ctl::Setting* FindSetting(std::string_view name) const;
	
	int key = 0;					// Toggle key (virtual-key code), 0 = none
	bool enabledByDefault = false;	
	std::string group;				// Modules of one group switch each other off ("movement": No Clip / Flight)
	
	// Menu state
	bool expanded = false;			// Its settings panel is open
	float anim = 0.f, hover = 0.f;
	
	// Used by the controls to register themselves.
	void AddSetting(ctl::Setting* s) { settings_.push_back(s); }

private:
	std::string name_, tab_, description_;
	int order_;
	bool enabled_ = false;
	std::atomic<bool> dirty_{false};
	std::vector<ctl::Setting*> settings_;	// Owned by the members of the derived class	
};

// A row that is a button: clicking it (or its key) runs OnTrigger() and it has no on/off state.
class ActionModule : public Module 
{
public:
	using Module::Module;
	bool IsAction() const final { return true; }
	void OnTrigger() override = 0;	
};

namespace modules 
{
	struct Tab 
	{
		std::string name;
		int order = 100;
		float width = 200.f;
		std::vector<Module*> modules;		// Sorted
	};
	
	// ---- Registration (static, from each module's own file) ------------------------------------------------------------------------
	using Factory = std::unique_ptr<Module> (*)();
	void Register(Factory factory);
	
	template <class T>
	struct Registrar 
	{
		Registrar(std::string name, int order, float width);	
	};
	
	// ---- Lifetime ------------------------------------------------------------------------------------------------------------------
	
}