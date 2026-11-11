#pragma once
// Script patching (Mirage, step 8): a loaded UnrealScript function's bytecode, decoded into tokens, edited, assembled
// again (UnrealLib's assemble_script: jumps and skips follow the edits) and swapped into the function. No native
// detour: the game's own VM runs the new code.
//
// The bytecode is the in-memory form: object operands are the objects' addresses, name operands the engine's global
// FNames (in Mirror's Edge both have their file sizes, 4 and 8 bytes, so the decoder reads them as it reads a package).
//
// The original is kept: Restore puts it back. A patched buffer is never freed (a call running in it when it is
// replaced keeps running in it) and lives in the process heap, outside this DLL's. Game thread only.

#include <UnrealLib/Bytecode/Bytecode.hpp>

#include <string>
#include <vector>

namespace Classes
{
	class UStruct;
}

namespace mirage::scripts
{
	// The function (or state) by path ("TdGame.TdMove_Jump.StartMove"), null if there is none with script
	Classes::UStruct* Find(const std::string& path);

	// Its bytecode as it is now (patched or not), decoded. False and `error` if it doesn't decode.
	bool Read(Classes::UStruct* function, UnrealLib::Script& out, std::string* error);

	// `script` assembled and put in place of the function's bytecode; `owner` names who did it (a mod's id). False
	// and `error` if it can't be assembled; nothing changes then.
	bool Apply(Classes::UStruct* function, const UnrealLib::Script& script, const std::string& owner, std::string* error);

	// The original bytecode back. Restore(owner) undoes all of one owner's patches; RestoreAll every patch (unload).
	bool Restore(Classes::UStruct* function);
	void RestoreOwner(const std::string& owner);
	void RestoreAll();
	bool Patched(Classes::UStruct* function);

	// A readable listing of `script`: one line per token, indented by depth, with its memory offset and operands
	std::string Dump(const UnrealLib::Script& script);

	// The token map of the game's bytecode
	const UnrealLib::TokenMap& Map();
}
