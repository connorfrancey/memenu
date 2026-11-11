	// ---- Hand-written (tools/sdkgen/static/UObject.inl) --------------------------------------------------------------
	static TArray<UObject*>* GObjects;

	static inline TArray<UObject*>& GetGlobalObjects()
	{
		return *GObjects;
	}

	inline std::string GetName() const
	{
		return Name.GetName();
	}

	// The name without a trailing "_<number>"
	inline std::string GetObjectName() const
	{
		std::string name = Name.GetName();
		const std::size_t pos = name.find_last_of('_');
		return pos != std::string::npos ? name.substr(0, pos) : name;
	}

	// "Class Package.Outer.Name"
	std::string GetFullName() const;
	// "Package.Outer.Name"
	std::string GetPathName() const;

	// Index of `shortName` in the global name table, -1 if it isn't there. Compares the wide strings in place.
	static int FindNameIndex(const std::string& shortName)
	{
		auto& names = FName::GetGlobalNames();
		for (std::size_t i = 0; i < names.Num(); ++i) {
			const auto entry = names[i];
			if (entry == nullptr) continue;
			const wchar_t* w = entry->GetWideName();
			std::size_t k = 0;
			while (k < shortName.size() && w[k] == static_cast<wchar_t>(static_cast<unsigned char>(shortName[k]))) ++k;
			if (k == shortName.size() && w[k] == L'\0') return static_cast<int>(i);
		}
		return -1;
	}

	// The object whose full name is `name` ("Class Package.Outer.Name"), nullptr if there is none. Only objects whose
	// own name is the last part of `name` get their full name built.
	template <typename T>
	static T* FindObject(const std::string& name)
	{
		const auto cut = name.find_last_of(". ");
		const int nameIndex = FindNameIndex(cut == std::string::npos ? name : name.substr(cut + 1));
		if (nameIndex >= 0) {
			for (std::size_t i = 0; i < GetGlobalObjects().Num(); ++i) {
				auto object = GetGlobalObjects().GetByIndex(i);
				if (object != nullptr && object->Name.Index == nameIndex && object->GetFullName() == name)
					return static_cast<T*>(object);
			}
		}
		// Names with a number suffix ("Foo_2") are stored as "Foo" + number: compare full names
		for (std::size_t i = 0; i < GetGlobalObjects().Num(); ++i) {
			auto object = GetGlobalObjects().GetByIndex(i);
			if (object != nullptr && object->GetFullName() == name) return static_cast<T*>(object);
		}
		return nullptr;
	}

	static class UClass* FindClass(const std::string& name)
	{
		return FindObject<class UClass>(name);
	}

	template <typename T>
	static T* GetObjectCasted(std::size_t index)
	{
		return static_cast<T*>(GetGlobalObjects().GetByIndex(index));
	}

	// This object is an instance of `cmp` or of a class derived from it
	bool IsA(const class UClass* cmp) const;

	inline void ProcessEvent(class UFunction* function, void* parms)
	{
		void* ret = nullptr;
		GetVFunction<void(__thiscall*)(UObject*, UFunction*, void*, void*)>(this, 61)(this, function, parms, &ret);
	}
	// ---- Generated ----------------------------------------------------------------------------------------------------
