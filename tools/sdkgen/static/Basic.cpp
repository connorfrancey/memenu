// Mirage SDK: the hand-written parts of the generated SDK (tools/sdkgen/static/Basic.cpp, copied by mirage_sdkgen).

#include "sdk/SDK.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Classes
{
	TArray<FNameEntry*>* FName::GNames = nullptr;
	TArray<UObject*>* UObject::GObjects = nullptr;

	std::string UObject::GetFullName() const
	{
		std::string name;
		if (Class != nullptr) {
			std::string outer;
			for (auto p = Outer; p; p = p->Outer) outer = p->GetName() + "." + outer;
			name = Class->GetName();
			name += " ";
			name += outer;
			name += GetName();
		}
		return name;
	}

	std::string UObject::GetPathName() const
	{
		std::string name = GetName();
		for (auto p = Outer; p; p = p->Outer) name = p->GetName() + "." + name;
		return name;
	}

	bool UObject::IsA(const UClass* cmp) const
	{
		int depth = 0;
		for (const UStruct* c = Class; c && depth < 64; c = c->SuperField, ++depth)
			if (c == cmp) return true;
		return false;
	}

	namespace detail
	{
		UFunction* FindFunction(const char* path)
		{
			static std::mutex lock;
			static std::unordered_map<std::string, UFunction*> table;
			static std::size_t scanned = 0;		// Objects in the list when the table was built

			std::lock_guard<std::mutex> guard(lock);
			if (auto it = table.find(path); it != table.end()) return it->second;
			if (!UObject::GObjects) return nullptr;
			const auto& objects = UObject::GetGlobalObjects();
			if (objects.Num() == scanned) return nullptr;		// Nothing new since the last time
			UClass* const functionClass = UFunction::StaticClass();
			if (!functionClass) return nullptr;
			table.clear();
			for (std::size_t i = 0; i < objects.Num(); ++i) {
				UObject* o = objects.GetByIndex(i);
				if (o && o->Class == functionClass) table.emplace(o->GetPathName(), static_cast<UFunction*>(o));
			}
			scanned = objects.Num();
			const auto it = table.find(path);
			return it == table.end() ? nullptr : it->second;
		}

		bool Call(UObject* object, UFunction* fn, void* params)
		{
			if (!object || !fn) return false;
			// ProcessEvent refuses functions with a native index (it would answer with an untouched buffer): cleared
			// for the call, so the engine runs them like script functions, then put back. Only the game thread runs
			// script, so nothing else sees it cleared.
			const std::uint16_t native = fn->iNative;
			fn->iNative = 0;
			object->ProcessEvent(fn, params);
			fn->iNative = native;
			return true;
		}
	}
}
