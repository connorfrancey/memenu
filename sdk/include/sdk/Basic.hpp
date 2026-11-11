#pragma once
// Mirage SDK: the engine's basic types (arrays, strings, names, interfaces, delegates) and what the generated code needs
// to call the game's functions. Hand-written (tools/sdkgen/static/Basic.hpp), copied by mirage_sdkgen.

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <set>
#include <string>
#include <utility>

namespace Classes
{
class UObject;

template<typename Fn>
inline Fn GetVFunction(const void *instance, std::size_t index)
{
	auto vtable = *reinterpret_cast<void* const* const*>(instance);
	return reinterpret_cast<Fn>(vtable[index]);
}

template<class T>
struct TArray
{
	friend struct FString;

	template <class U>
	friend struct TTransArray;

public:
	inline TArray()
	{
		Data = nullptr;
		Count = Max = 0;
	};

	inline T *Buffer() const
	{
		return Data;
	};

	inline size_t Num() const
	{
		return Count;
	};

	inline T& operator[](size_t i)
	{
		return Data[i];
	};

	inline const T& operator[](size_t i) const
	{
		return Data[i];
	};

	inline bool IsValidIndex(size_t i) const
	{
		return i < Num();
	}

	inline T& GetByIndex(size_t i)
	{
		return Data[i];
	}

	inline const T& GetByIndex(size_t i) const
	{
		return Data[i];
	}

private:
	T* Data;
	int32_t Count;
	int32_t Max;
};

template<class T>
struct TTransArray : private TArray<T>
{
	inline TTransArray() 
	{
	}

	UObject* Owner;
};

struct FString : private TArray<wchar_t>
{
	inline FString()
	{
	}

	FString(const wchar_t* other)
	{
		Max = Count = *other ? std::wcslen(other) + 1 : 0;

		if (Count)
		{
			Data = const_cast<wchar_t*>(other);
		}
	};

	inline bool IsValid() const
	{
		return Data != nullptr;
	}

	inline const wchar_t* c_str() const
	{
		return Data;
	}

	// ASCII narrowed with a loop, like FNameEntry::GetName (no std::locale per call)
	std::string ToString() const
	{
		auto length = std::wcslen(Data);

		std::string str(length, '\0');

		for (std::size_t i = 0; i < length; ++i)
		{
			const wchar_t c = Data[i];
			str[i] = c < 0x80 ? static_cast<char>(c) : '?';
		}

		return str;
	}
};

template<class TEnum>
class TEnumAsByte
{
public:
	inline TEnumAsByte()
	{
	}

	inline TEnumAsByte(TEnum _value)
		: value(static_cast<uint8_t>(_value))
	{
	}

	explicit inline TEnumAsByte(int32_t _value)
		: value(static_cast<uint8_t>(_value))
	{
	}

	explicit inline TEnumAsByte(uint8_t _value)
		: value(_value)
	{
	}

	inline operator TEnum() const
	{
		return (TEnum)value;
	}

	inline TEnum GetValue() const
	{
		return (TEnum)value;
	}

private:
	uint8_t value;
};

class FNameEntry
{
public:
	uint32_t Index;
	char UnknownData00[0x0C];
	wchar_t WideName[1024];

	inline const int32_t GetIndex() const
	{
		return Index;
	}

	inline const wchar_t* GetWideName() const
	{
		return WideName;
	}

	// Engine names are ASCII: narrowed with a loop. The std::locale this used to build on every call made each name
	// read cost microseconds, and the menu reads thousands (classes, keys, actors).
	std::string GetName() const
	{
		auto length = std::wcslen(WideName);

		std::string str(length, '\0');

		for (std::size_t i = 0; i < length; ++i)
		{
			const wchar_t c = WideName[i];
			str[i] = c < 0x80 ? static_cast<char>(c) : '?';
		}

		return str;
	}

	// The name equals `ascii`, compared in place (no string is built).
	bool Equals(const char* ascii) const
	{
		std::size_t k = 0;
		while (ascii[k] != '\0' && WideName[k] == static_cast<wchar_t>(static_cast<unsigned char>(ascii[k]))) ++k;
		return ascii[k] == '\0' && WideName[k] == L'\0';
	}
};

struct FName
{
	int32_t Index;
	int32_t Number;

	inline FName()
		: Index(0),
		  Number(0)
	{
	};

	inline FName(int32_t i)
		: Index(i),
		  Number(0)
	{
	};

	// Looks the name up in the engine's name table. Names found before are remembered (a name keeps its index for the
	// whole session); the table is walked comparing in place, without building a string per entry.
	FName(const char* nameToFind)
		: Index(0),
		  Number(0)
	{
		struct Cache
		{
			std::set<std::pair<std::string, int32_t>> found;
		};
		static Cache cache;

		const auto at = cache.found.lower_bound({nameToFind, INT32_MIN});
		if (at != cache.found.end() && at->first == nameToFind)
		{
			auto entry = GetGlobalNames()[at->second];
			if (entry != nullptr && entry->Equals(nameToFind))
			{
				Index = at->second;
				return;
			}
		}

		for (auto i = 0u; i < GetGlobalNames().Num(); ++i)
		{
			auto entry = GetGlobalNames()[i];
			if (entry != nullptr && entry->Equals(nameToFind))
			{
				cache.found.insert({nameToFind, static_cast<int32_t>(i)});

				Index = i;

				return;
			}
		}
	};

	static TArray<FNameEntry*>* GNames;
	static inline TArray<FNameEntry*>& GetGlobalNames()
	{
		return *GNames;
	};

	inline std::string GetName() const
	{
		auto name = GetGlobalNames()[Index]->GetName();

		if (Number > 0)
		{
			name += "_" + std::to_string(Number - 1);
		}

		return name;
	};

	inline bool operator==(const FName& other) const
	{
		return Index == other.Index && Number == other.Number;
	};
};

class FScriptInterface
{
private:
	UObject* ObjectPointer;
	void* InterfacePointer;

public:
	inline UObject* GetObject() const
	{
		return ObjectPointer;
	}

	inline UObject*& GetObjectRef()
	{
		return ObjectPointer;
	}

	inline void* GetInterface() const
	{
		return ObjectPointer != nullptr ? InterfacePointer : nullptr;
	}
};

template<class InterfaceType>
class TScriptInterface : public FScriptInterface
{
public:
	inline InterfaceType* operator->() const
	{
		return (InterfaceType*)GetInterface();
	}

	inline InterfaceType& operator*() const
	{
		return *((InterfaceType*)GetInterface());
	}

	inline operator bool() const
	{
		return GetInterface() != nullptr;
	}
};

struct FScriptDelegate
{
	class UObject* Object;
	FName FunctionName;
};

// A TMap (MapProperty): opaque, 60 bytes
struct FScriptMap
{
	unsigned char Data[0x3C];
};

class UFunction;

namespace detail
{
	// The function at `path` ("Engine.Actor.SetLocation"), nullptr if the game has not loaded it (yet). Looked up in a
	// table built from the engine's object list, rebuilt when the list has grown.
	UFunction* FindFunction(const char* path);

	// Runs `fn` on `object` with `params` (laid out as the engine laid out the function's parameters; out parameters
	// and the return value come back in them). ProcessEvent refuses functions with a native index, so the index is
	// cleared for the call. False when `fn` or `object` is null. Game thread only.
	bool Call(class UObject* object, UFunction* fn, void* params);

	// The generated layout checks that the compiler can't do (bitfields: each bool's bit in its dword). Returns the
	// number of failures; the first is described in `first`.
	int CheckLayouts(std::string* first = nullptr);
}
}

