#pragma once
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

class Module;

// The controls a module can have. A module declares them as members, with the arguments of the ImGui widget they wrap
// and an optional tooltip last:
//
//     ctl::Slider speed{this, "Speed", 900.f, 100.f, 10000.f, "%.0f"};
//     ctl::Checkbox followView{this, "Move along view", true, "Forward goes where you look"};
//
// and reads them where it needs them (`speed.Get()`, or just `speed`). A control registers itself with its module,
// draws itself, saves and loads itself, and can be driven with keys:
//
//     Checkbox          toggle
//     Slider/SliderInt  increase / decrease by a step
//     Combo             next / previous option
//     ColorPick         next / previous hue
//     Button            press
//
// Right-click a control in the menu to give it keys. (KeyHold is itself a key and Label/Separator/Custom have no value.)
// To show a control only sometimes, call VisibleIf() in the module's constructor.
namespace ctl {
	class Setting {
	public:
		virtual ~Setting() = default;
		Setting(const Setting &) = delete;
		Setting &operator=(const Setting &) = delete;

		const std::string &Name() const { return name_; }
		Module &Owner() const { return *owner_; }

		bool Visible() const { return !visibleIf_ || visibleIf_(); }
		void VisibleIf(std::function<bool()> f) { visibleIf_ = std::move(f); }

		// Key bindings (virtual-key codes, 0 = none). What they do is up to the control, see BindLabel().
		int key = 0;
		int keyAlt = 0;

		// GUI: the control, its tooltip (with the keys it has) and the right-click key binding popup.
		void Show(float width);

		// What `slot` 0 (key) and 1 (keyAlt) do for this control, or nullptr if it has no such binding.
		virtual const char *BindLabel(int slot) const { (void)slot; return nullptr; }
		bool Bindable() const { return BindLabel(0) || BindLabel(1); }
		// A bound key was pressed. Shows a short on-screen message with the new value.
		void Press(int slot);

		// Controls with a value are saved in the config
		virtual bool Persistent() const { return false; }
		virtual void Save(nlohmann::ordered_json &out) const { (void)out; }
		virtual void Load(const nlohmann::ordered_json &in) { (void)in; }

	protected:
		struct Binds {          // Labels; missing ones are nullptr
			const char* key;
			const char* keyAlt;
		};
		Setting(Module *owner, std::string name, std::string tooltip = {}, Binds binds = {});

		virtual void Draw(float width) = 0;
		// Performs the bound action and returns the text to show ("ON", "1200", ...). Empty: nothing to show.
		virtual std::string Fire(int slot) { (void) slot; return {}; }
		
	private:
		Module *owner_;
		std::string name_, tooltip_;
		Binds binds_;
		std::function<bool()> visibleIf_;
	};

	// A control that holds a T, which is what gets saved. Set() keeps it valid (in range, a real option, ...).
	template <class T>
	class Value : public Setting {
	public:
		const T& Get() const { return value_; }
		operator const T&() const { return value_; }
		void Set(T v) { value_ = Clamp(std::move(v)); }

		bool Persistent() const override { return true; }
		void Save(nlohmann::ordered_json& out) const override;
		void Load(const nlohmann::ordered_json& in) override;

	protected:
		Value(Module* owner, std::string name, T value, std::string tooltip, Binds binds = {})
			: Setting(owner, std::move(name), std::move(tooltip), binds), value_(std::move(value)) {}

		virtual T Clamp(T v) const { return v; }    // Not called by this constructor: derived ones clamp their default

		T value_;
	};

	// Save/Load live in setting.cpp, for these types only.
	extern template class Value<bool>;
	extern template class Value<int>;
	extern template class Value<float>;
	extern template class Value<std::array<float, 4>>;

	// ---- Value Controls --------------------------------------------------------------------------------------------------
	class Checkbox final : public Value<bool> {
	public:
		Checkbox(Module* owner, std::string name, bool value = false, std::string tooltip = {});

	protected:
		void Draw(float width) override;
		std::string Fire(int slot) override;
	};

	// `format` is for the value only, e.g. "%.1fx" or "%.0f deg". `step` is for the keys (0: 1/20 of the range).
	template <class T>
	class BasicSlider final : public Value<T> {
	public:
		BasicSlider(Module* owner, std::string name, T value, T min, T max,
					std::string format = std::is_integral_v<T> ? "%d" : "%.2f", T step = 0, std::string tooltip = {});

	protected:
		T Clamp(T v) const override;
		void Draw(float width) override;
		std::string Fire(int slot) override;

	private:
		T min_, max_, step_;
		std::string format_;
	};
	extern template class BasicSlider<float>;
	extern template class BasicSlider<int>;

	using Slider = BasicSlider<float>;
	using SliderInt = BasicSlider<int>;

	// The value is the index of the selected item.
	class Combo final : public Value<int> {
	public:
		Combo(Module* owner, std::string name, std::vector<std::string> items, int value = 0, std::string tooltip = {});

		const std::string& Selected() const;

	protected:
		int Clamp(int index) const override;
		void Draw(float width) override;
		std::string Fire(int slot) override;

	private:
		std::vector<std::string> items_;
	};

	// A combo whose items and selection live somewhere else (e.g. the list of theme files). Never saved by the menu.
	class DynamicCombo final : public Setting {
	public:
		DynamicCombo(Module* owner, std::string name, std::function<std::vector<std::string>()> items,
					 std::function<int()> get, std::function<void(int)> set, std::string tooltip = {});

	protected:
		void Draw(float width) override;
		std::string Fire(int slot) override;

	private:
		std::function<std::vector<std::string>()> items_;
		std::function<int()> get_;
		std::function<void(int)> set_;
	};

	// RGBA, 0..1.
	class ColorPick final : public Value<std::array<float, 4>> {
	public:
		ColorPick(Module* owner, std::string name, std::array<float, 4> value = {1.f, 1.f, 1.f, 1.f}, std::string tooltip = {});

		unsigned int Packed() const;                           // IM_COL32 (ABGR)

	protected:
		std::array<float, 4> Clamp(std::array<float, 4> v) const override;
		void Draw(float width) override;
		std::string Fire(int slot) override;
	};

	// ---- Input ------------------------------------------------------------------------------------------------------
	// A key the module polls (hold-to-boost, up/down while flying). Bound with the button itself. The value is the
	// virtual-key code.
	class KeyHold final : public Value<int> {
	public:
		KeyHold(Module* owner, std::string name, int vk = 0, std::string tooltip = {});

		bool Down() const;                    // Currently held?

	protected:
		void Draw(float width) override;
	};

	class Button final : public Setting {
	public:
		Button(Module* owner, std::string name, std::function<void()> onClick, std::string tooltip = {});

	protected:
		void Draw(float width) override;
		std::string Fire(int slot) override;

	private:
		std::function<void()> onClick_;
	};

	// ---- Layout / Free-form -----------------------------------------------------------------------------------------
	class Label final : public Setting {
	public:
		Label(Module* owner, std::string text);

	protected:
		void Draw(float width) override;
	};

	class Separator final : public Setting {
	public:
		explicit Separator(Module* owner, std::string title = {});    // Empty: a plain line

	protected:
		void Draw(float width) override;
	};

	// Whatever `draw` renders inside the module's panel, every frame. `width` is the usable width.
	class Custom final : public Setting {
	public:
		Custom(Module* owner, std::function<void(float width)> draw);

	protected:
		void Draw(float width) override;

	private:
		std::function<void(float)> draw_;
	};
}
