#include "runtime/call.h"

#include "game/object_ref.h"
#include "game/uobject.h"

#include "sdk/SDK.hpp"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace mirage::call
{
	namespace
	{
		using layout::Kind;

		constexpr std::uint64_t kParm = 0x80;		// CPF_Parm

		// UFunction::iNative (a WORD after FunctionFlags at 0x90). Found by looking for the native indexes two functions
		// are known to have (Object.uc / Actor.uc: native(281) IsInState, native(267) SetLocation), not assumed.
		int g_nativeAt = -1;
		bool g_checked = false;
		std::string g_why;

		std::uint16_t Word(const void* p, int at)
		{
			std::uint16_t v;
			std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof v);
			return v;
		}

		Classes::UFunction* FindIn(Classes::UStruct* cls, std::string_view name)
		{
			static Classes::UClass* const functionClass = Classes::UFunction::StaticClass();
			for (Classes::UStruct* s = cls; s; s = game::SuperOf(s)) {
				int guard = 0;
				for (Classes::UField* f = game::ChildrenOf(s); f && guard < 100000; f = game::NextOf(f), ++guard)
					if (game::IsA(f, functionClass) && f->GetName() == name) return static_cast<Classes::UFunction*>(f);
			}
			return nullptr;
		}

		Classes::UFunction* Named(const char* path)
		{
			return Classes::UObject::FindObject<Classes::UFunction>(path);
		}
	}

	bool LayoutChecked(std::string* why)
	{
		if (!g_checked) {
			g_checked = true;
			Classes::UFunction* isInState = Named("Function Core.Object.IsInState");
			Classes::UFunction* setLocation = Named("Function Engine.Actor.SetLocation");
			if (!isInState || !setLocation) {
				g_why = "IsInState or SetLocation not found";
			} else {
				for (int at = 0x90; at < 0xC0 && g_nativeAt < 0; at += 2)
					if (Word(isInState, at) == 281 && Word(setLocation, at) == 267) g_nativeAt = at;
				if (g_nativeAt < 0) g_why = "the native index of IsInState (281) and SetLocation (267) was not found";
			}
			if (g_nativeAt >= 0) spdlog::info("Mirage: UFunction::iNative at {:#x}", g_nativeAt);
			else spdlog::error("Mirage: calls to native functions are off: {}", g_why);
		}
		if (why) *why = g_why;
		return g_nativeAt >= 0;
	}

	int NativeIndex(Classes::UFunction* fn)
	{
		if (!fn || !LayoutChecked()) return -1;
		return Word(fn, g_nativeAt);
	}

	Classes::UFunction* FindFunction(Classes::UObject* object, std::string_view name)
	{
		if (!object || !object->Class) return nullptr;
		static std::unordered_map<std::string, Classes::UFunction*> cache;	// "<class address>.<name>": classes live on
		const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(object->Class)) + "." + std::string(name);
		if (const auto it = cache.find(key); it != cache.end()) return it->second;
		Classes::UFunction* fn = FindIn(object->Class, name);
		if (fn) cache.emplace(key, fn);		// Misses aren't kept: a class may gain functions as packages load
		return fn;
	}

	// ---- Params -------------------------------------------------------------------------------------------------------
	Params::Params(Classes::UObject* object, std::string_view function) : Params(FindFunction(object, function))
	{
		if (!fn_) error_ = "no function " + std::string(function) + " in " + (object && object->Class ? object->Class->GetName() : "null");
	}

	Params::Params(Classes::UFunction* fn) : fn_(fn)
	{
		if (!fn_) {
			error_ = "no function";
			return;
		}
		// A function's parameters never change: read once per function (game thread only, like every call here)
		static std::unordered_map<Classes::UFunction*, std::vector<layout::Property>> cache;
		auto it = cache.find(fn_);
		if (it == cache.end()) {
			std::vector<layout::Property> params;
			for (layout::Property& p : layout::LiveProperties(fn_))
				if (p.flags & kParm) params.push_back(std::move(p));
			it = cache.emplace(fn_, std::move(params)).first;
		}
		params_ = it->second;
		// The whole frame (parameters and locals): ProcessEvent copies ParmsSize bytes from it, never more
		buffer_.assign(std::max<std::size_t>(game::PropertySizeOf(fn_), 4), 0);
	}

	const layout::Property* Params::Find(std::string_view name, std::initializer_list<Kind> kinds) const
	{
		const auto it = std::find_if(params_.begin(), params_.end(), [&](const layout::Property& p) { return p.name == name; });
		if (it == params_.end()) {
			error_ = "no parameter " + std::string(name);
			return nullptr;
		}
		if (std::find(kinds.begin(), kinds.end(), it->kind) == kinds.end()) {
			error_ = std::string(name) + " is a " + layout::KindName(it->kind) + " parameter";
			return nullptr;
		}
		if (it->offset + it->elementSize * it->arrayDim > buffer_.size()) {
			error_ = std::string(name) + " lies outside the parameters";
			return nullptr;
		}
		return &*it;
	}

	const layout::Property* Params::Find(std::string_view name, std::initializer_list<Kind> kinds, bool)
	{
		return static_cast<const Params*>(this)->Find(name, kinds);
	}

	bool Params::Set(std::string_view name, bool value)
	{
		const auto* p = Find(name, {Kind::Bool}, true);
		if (!p) return false;
		std::uint32_t dword;
		std::memcpy(&dword, buffer_.data() + p->offset, 4);
		dword = value ? (dword | p->boolMask) : (dword & ~p->boolMask);
		std::memcpy(buffer_.data() + p->offset, &dword, 4);
		return true;
	}

	bool Params::Set(std::string_view name, std::int32_t value)
	{
		const auto* p = Find(name, {Kind::Int}, true);
		if (p) std::memcpy(buffer_.data() + p->offset, &value, 4);
		return p != nullptr;
	}

	bool Params::Set(std::string_view name, float value)
	{
		const auto* p = Find(name, {Kind::Float}, true);
		if (p) std::memcpy(buffer_.data() + p->offset, &value, 4);
		return p != nullptr;
	}

	bool Params::Set(std::string_view name, std::uint8_t value)
	{
		const auto* p = Find(name, {Kind::Byte}, true);
		if (p) buffer_[p->offset] = value;
		return p != nullptr;
	}

	bool Params::Set(std::string_view name, Classes::UObject* value)
	{
		const auto* p = Find(name, {Kind::Object, Kind::Class, Kind::Component}, true);
		if (p) std::memcpy(buffer_.data() + p->offset, &value, sizeof value);
		return p != nullptr;
	}

	bool Params::SetName(std::string_view name, const char* value)
	{
		const auto* p = Find(name, {Kind::Name}, true);
		if (!p) return false;
		const Classes::FName n = game::Name(value);
		static_assert(sizeof n == 8, "FName is an index and a number");
		std::memcpy(buffer_.data() + p->offset, &n, sizeof n);
		return true;
	}

	bool Params::SetRaw(std::string_view name, const void* data, std::uint32_t size)
	{
		const auto* p = Find(name, {Kind::Struct, Kind::Str, Kind::Int, Kind::Float, Kind::Byte, Kind::Name, Kind::Object}, true);
		if (!p) return false;
		if (size > p->elementSize * p->arrayDim) {
			error_ = std::string(name) + " holds " + std::to_string(p->elementSize * p->arrayDim) + " bytes, not " +
					 std::to_string(size);
			return false;
		}
		std::memcpy(buffer_.data() + p->offset, data, size);
		return true;
	}

	bool Params::GetBool(std::string_view name) const
	{
		const auto* p = Find(name, {Kind::Bool});
		if (!p) return false;
		std::uint32_t dword;
		std::memcpy(&dword, buffer_.data() + p->offset, 4);
		return (dword & p->boolMask) != 0;
	}

	std::int32_t Params::GetInt(std::string_view name) const
	{
		std::int32_t v = 0;
		if (const auto* p = Find(name, {Kind::Int})) std::memcpy(&v, buffer_.data() + p->offset, 4);
		return v;
	}

	float Params::GetFloat(std::string_view name) const
	{
		float v = 0.f;
		if (const auto* p = Find(name, {Kind::Float})) std::memcpy(&v, buffer_.data() + p->offset, 4);
		return v;
	}

	std::uint8_t Params::GetByte(std::string_view name) const
	{
		const auto* p = Find(name, {Kind::Byte});
		return p ? buffer_[p->offset] : 0;
	}

	Classes::UObject* Params::GetObject(std::string_view name) const
	{
		Classes::UObject* v = nullptr;
		if (const auto* p = Find(name, {Kind::Object, Kind::Class, Kind::Component}))
			std::memcpy(&v, buffer_.data() + p->offset, sizeof v);
		return v;
	}

	bool Params::GetRaw(std::string_view name, void* out, std::uint32_t size) const
	{
		const auto* p = Find(name, {Kind::Struct, Kind::Str, Kind::Int, Kind::Float, Kind::Byte, Kind::Name, Kind::Object});
		if (!p || size > p->elementSize * p->arrayDim) return false;
		std::memcpy(out, buffer_.data() + p->offset, size);
		return true;
	}

	// ---- Call ---------------------------------------------------------------------------------------------------------
	bool Call(Classes::UObject* object, Params& params, std::string* error)
	{
		const auto fail = [&](std::string why) {
			if (error) *error = std::move(why);
			return false;
		};
		if (!object) return fail("no object");
		if (!params.Valid()) return fail(params.Error());
		Classes::UFunction* fn = params.Function();

		// ProcessEvent refuses functions with a native index: cleared for the call, so it runs them like the rest (its own
		// frame, the function's parameter code reading the values from our buffer), then put back. Only the game thread
		// runs script, so nothing else sees it cleared.
		std::uint16_t saved = 0;
		std::uint8_t* slot = nullptr;
		if (LayoutChecked()) {
			slot = reinterpret_cast<std::uint8_t*>(fn) + g_nativeAt;
			std::memcpy(&saved, slot, sizeof saved);
		}
		if (saved) {
			const std::uint16_t zero = 0;
			std::memcpy(slot, &zero, sizeof zero);
		}
		object->ProcessEvent(fn, params.Data());
		if (saved) std::memcpy(slot, &saved, sizeof saved);
		return true;
	}
}
