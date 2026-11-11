/* Mirage mod API, ABI version 1 (Mirage step 4).
 *
 * The one header a native mod needs. Plain C, so a mod can be built with any compiler (another MSVC toolset, clang)
 * and still work: no STL, no exceptions and no C++ types cross the boundary, every struct starts with its size, and
 * every function pointer is __cdecl.
 *
 * A native mod is a 32-bit DLL (the game is 32-bit) next to its mod.toml (entry = "my_mod.dll") that exports
 *
 *     MIRAGE_EXPORT uint32_t mirage_mod_abi(void) { return MIRAGE_ABI_VERSION; }
 *     MIRAGE_EXPORT int32_t mirage_mod_load(const MirageApi* api, MirageMod* mod);    // 0 = loaded
 *     MIRAGE_EXPORT void mirage_mod_unload(void);                                      // optional
 *
 * mirage_mod_load registers the mod's modules and their settings through `api`, which stays valid until
 * mirage_mod_unload returns. The runtime refuses a mod whose mirage_mod_abi() is newer than its own ABI.
 *
 * Rules that keep old mods working (checked by tests/test_abi.cpp):
 *   - MirageApi only grows: functions are appended, never removed, reordered or changed. A mod built against a newer
 *     header checks `api->size` before using a function that may be missing (MIRAGE_API_HAS).
 *   - Structs the mod fills (MirageModuleDesc) start with `size`; the runtime reads only the fields that size covers.
 *   - Strings are UTF-8 and copied by the runtime: a mod may pass temporaries.
 *
 * Threads: on_tick runs on the game thread (the engine tick), on_render on the render thread, inside the menu's frame
 * (draw with api->draw_*). on_enable / on_disable run where the module is switched (usually the render thread).
 * Everything else may be called from any of these. */
#ifndef MIRAGE_API_H
#define MIRAGE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIRAGE_ABI_VERSION 1u

#if defined(_WIN32)
#define MIRAGE_CALL __cdecl
#define MIRAGE_EXPORT __declspec(dllexport)
#else
#define MIRAGE_CALL
#define MIRAGE_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define MIRAGE_EXTERN_C extern "C"
#else
#define MIRAGE_EXTERN_C
#endif

/* Handles: owned by the runtime, valid until the mod is unloaded */
typedef struct MirageMod MirageMod;            /* The mod itself (its id, folder, config) */
typedef struct MirageModule MirageModule;      /* A row in the menu */
typedef struct MirageSetting MirageSetting;    /* A control in a module's settings panel */

typedef enum MirageLogLevel {
    MIRAGE_LOG_DEBUG = 0,
    MIRAGE_LOG_INFO = 1,
    MIRAGE_LOG_WARN = 2,
    MIRAGE_LOG_ERROR = 3
} MirageLogLevel;

/* MirageModuleDesc.flags */
#define MIRAGE_MODULE_ACTION 0x1u                /* A button: on_trigger, no on/off state */
#define MIRAGE_MODULE_ENABLED_BY_DEFAULT 0x2u    /* On the first time the mod is loaded (the config remembers after) */

/* setting_type: what a setting holds */
typedef enum MirageSettingType {
    MIRAGE_SETTING_BOOL = 0,
    MIRAGE_SETTING_INT = 1,
    MIRAGE_SETTING_FLOAT = 2,
    MIRAGE_SETTING_STRING = 3,
    MIRAGE_SETTING_CHOICE = 4,
    MIRAGE_SETTING_KEY = 5,
    MIRAGE_SETTING_UNKNOWN = -1
} MirageSettingType;

typedef void(MIRAGE_CALL* MirageCallback)(void* user);

/* What a module is: filled by the mod, copied by module_register */
typedef struct MirageModuleDesc {
    uint32_t size;                  /* sizeof(MirageModuleDesc) */
    const char* name;               /* Unique in the menu ("Hello"); the config saves it under this name */
    const char* tab;                /* Menu column ("Mods" if NULL); created when no module had it yet */
    const char* description;        /* One line, shown on hover (may be NULL) */
    int32_t order;                  /* Position in the column, lower first (100 if 0) */
    int32_t key;                    /* Default toggle key (virtual-key code), 0 = none */
    uint32_t flags;                 /* MIRAGE_MODULE_* */
    void* user;                     /* Passed back to every callback */
    MirageCallback on_enable;       /* Each may be NULL */
    MirageCallback on_disable;      /* Also on unload: undo what the module changed */
    MirageCallback on_tick;         /* Game thread, every tick while on */
    MirageCallback on_render;       /* Render thread, every frame while on */
    MirageCallback on_trigger;      /* Action modules: clicked or key pressed */
} MirageModuleDesc;

/* ---- Game objects, events and function hooks (step 6) -------------------------------------------------------------- */
typedef struct MirageObject MirageObject;           /* A game object (an Actor, a Controller, ...); not owned: it lives
                                                       as long as the game keeps it, so look it up again rather than keep
                                                       it across ticks */
typedef struct MirageCall MirageCall;               /* A call of a game function: one a hook sees, or one the mod makes */
typedef struct MirageHook MirageHook;               /* A function hook */
typedef struct MirageSubscription MirageSubscription;   /* An event subscription */
typedef struct MirageDetour MirageDetour;           /* A detour of native code */

/* A value read from or written to a game object or a call, by dotted path ("Location.Z", "NewLocation", "ReturnValue") */
typedef enum MirageValueType {
    MIRAGE_VALUE_NONE = 0,      /* Nothing there (the path didn't resolve) */
    MIRAGE_VALUE_BOOL = 1,      /* i: 0/1 */
    MIRAGE_VALUE_INT = 2,       /* i (bytes and enums too) */
    MIRAGE_VALUE_FLOAT = 3,     /* f */
    MIRAGE_VALUE_OBJECT = 4,    /* o (NULL: none) */
    MIRAGE_VALUE_TEXT = 5,      /* text: a name or a string (written: names only) */
    MIRAGE_VALUE_STRUCT = 6     /* A struct: reach its members with a longer path ("Location" -> "Location.X") */
} MirageValueType;
typedef struct MirageValue {
    int32_t type;               /* MIRAGE_VALUE_* */
    int32_t i;
    float f;
    MirageObject* o;
    char text[256];             /* UTF-8, cut at 255 bytes */
} MirageValue;

/* hook_function: when the hook runs */
#define MIRAGE_HOOK_BEFORE 0    /* Before the function: may change its parameters, or call_skip() it */
#define MIRAGE_HOOK_AFTER 1     /* After it: its out parameters and "ReturnValue" are set */
#define MIRAGE_HOOK_REPLACE 2   /* Instead of it: set "ReturnValue" (the highest priority replace runs, alone) */
typedef void(MIRAGE_CALL* MirageHookFn)(void* user, MirageCall* call);

/* event_subscribe: what happened (MirageEvent.type) and what comes with it */
#define MIRAGE_EVENT_TICK 0           /* Game thread, every engine tick while in a level: number = seconds since the last */
#define MIRAGE_EVENT_LEVEL_LOAD 1     /* Game thread: value 0 a load starts, 1 it finished; text = the map ("edge_p") */
#define MIRAGE_EVENT_DEATH 2          /* Game thread: value 0 the player died (before the game handles it), 1 after */
#define MIRAGE_EVENT_PLAYER_STATE 3   /* Game thread: the player's move state changed: value = new EMovement, value2 = old */
#define MIRAGE_EVENT_KEY 4            /* A key went down (the game window's thread): value = virtual-key code */
#define MIRAGE_EVENT_STATE_RESTORED 5 /* Game thread: the TAS put back a memory snapshot. The game's memory went back in
                                         time (a mod's own memory didn't): every game object kept from before may be
                                         another or gone, so look them up again */
typedef struct MirageEvent {
    uint32_t size;              /* sizeof(MirageEvent) */
    int32_t type;               /* MIRAGE_EVENT_* */
    int32_t value, value2;
    float number;
    const char* text;           /* Valid during the callback; "" if none */
} MirageEvent;
typedef void(MIRAGE_CALL* MirageEventFn)(void* user, const MirageEvent* event);

/* ---- Script patching (step 8) ---------------------------------------------------------------------------------------
 * A loaded UnrealScript function's bytecode, opened as a tree of tokens (script_open), edited (script_set, script_add,
 * script_set_children) and put in place of the function's own code (script_apply): the game's VM runs it, no native
 * detour. Jump targets and skip sizes are recomputed from the tree. script_restore, or the mod's unload, puts the
 * original back. Game thread. script_dump lists the tokens (with their indices) to find what to change. */
typedef struct MirageScript MirageScript;          /* A function's bytecode, open for editing (owned by the mod) */
typedef struct MirageToken {
    uint32_t size;              /* sizeof(MirageToken): set it before script_get / script_set / script_add */
    int32_t kind;               /* The token's kind: script_kind("FloatConst"), script_kind_name(kind) */
    int32_t native_index;       /* NativeFunction: the native's index (171: float * float, 174: float + float) */
    int32_t int_value;          /* IntConst */
    float floats[3];            /* FloatConst: floats[0]; VectorConst */
    int32_t ints[3];            /* RotationConst */
    MirageObject* object;       /* The object operand: a variable's property, a function, a class (NULL: none) */
    MirageObject* object2;      /* A second one (StructMember's struct, Context's property) */
    char name[64];              /* The name operand (VirtualFunction's function, NameConst); written: an existing name */
    uint16_t word, word2;       /* Skips and jump targets as decoded (script_apply recomputes them) */
    uint8_t byte_value, byte_value2, reserved[2];   /* ByteConst / IntConstByte, cast type, flags */
    int32_t target;             /* A jump added by the mod: the token it goes to (-1: as decoded, -2: the script's end) */
    uint32_t memory_offset;     /* Where it was in the code when opened (read only; 0 for an added token) */
    char text[256];             /* StringConst / UnicodeStringConst, UTF-8 */
} MirageToken;

/* The runtime's functions. Only grows; `size` says how much of it this runtime has. */
typedef struct MirageApi {
    uint32_t size;                  /* sizeof(MirageApi) of the runtime */
    uint32_t abi_version;           /* MIRAGE_ABI_VERSION of the runtime */
    const char* runtime_version;    /* "0.1.0" */

    /* ---- Logging: into the menu's log, prefixed with the mod's id ---------------------------------------------- */
    void(MIRAGE_CALL* log)(MirageMod* mod, int32_t level, const char* text);

    /* ---- The mod ------------------------------------------------------------------------------------------------ */
    const char*(MIRAGE_CALL* mod_id)(MirageMod* mod);            /* "hello" (from mod.toml) */
    const char*(MIRAGE_CALL* mod_version)(MirageMod* mod);       /* "1.0.0" */
    const char*(MIRAGE_CALL* mod_dir)(MirageMod* mod);           /* Its folder, UTF-8, no trailing slash */

    /* ---- Modules ------------------------------------------------------------------------------------------------ */
    /* NULL when the name is taken or the description is invalid (the reason is logged) */
    MirageModule*(MIRAGE_CALL* module_register)(MirageMod* mod, const MirageModuleDesc* desc);
    int32_t(MIRAGE_CALL* module_enabled)(MirageModule* module);
    void(MIRAGE_CALL* module_set_enabled)(MirageModule* module, int32_t on);

    /* ---- Settings: controls in the module's panel, saved with the menu's config ------------------------------- */
    /* Made in mod_load, before the config is read: the saved value replaces `value`. NULL if the name is taken. */
    MirageSetting*(MIRAGE_CALL* setting_checkbox)(MirageModule* module, const char* name, int32_t value,
                                                  const char* tooltip);
    MirageSetting*(MIRAGE_CALL* setting_slider)(MirageModule* module, const char* name, float value, float min,
                                                float max, const char* format, const char* tooltip);
    /* min and max are clamped to +-(INT32_MAX / 2), the most an int slider takes */
    MirageSetting*(MIRAGE_CALL* setting_slider_int)(MirageModule* module, const char* name, int32_t value, int32_t min,
                                                    int32_t max, const char* tooltip);
    MirageSetting*(MIRAGE_CALL* setting_text)(MirageModule* module, const char* name, const char* value,
                                              const char* tooltip);
    MirageSetting*(MIRAGE_CALL* setting_key)(MirageModule* module, const char* name, int32_t vk, const char* tooltip);

    /* Values. bool/int/float read any numeric setting (a checkbox is 0/1); a key setting's value is its key. */
    int32_t(MIRAGE_CALL* setting_get_int)(MirageSetting* setting);
    float(MIRAGE_CALL* setting_get_float)(MirageSetting* setting);
    void(MIRAGE_CALL* setting_set_int)(MirageSetting* setting, int32_t value);      /* Clamped to the range */
    void(MIRAGE_CALL* setting_set_float)(MirageSetting* setting, float value);
    /* Text: copies up to `capacity` bytes (with the terminator) and returns the full length */
    uint32_t(MIRAGE_CALL* setting_get_text)(MirageSetting* setting, char* buffer, uint32_t capacity);
    void(MIRAGE_CALL* setting_set_text)(MirageSetting* setting, const char* value);
    /* A key setting is held down right now (with the modifiers it was bound with) */
    int32_t(MIRAGE_CALL* key_down)(MirageSetting* setting);

    /* ---- Per-mod config: <mod folder>/config.toml, for what isn't a setting ----------------------------------- */
    /* Copies up to `capacity` bytes and returns the full length; `fallback` (may be NULL) when the key isn't set */
    uint32_t(MIRAGE_CALL* config_get)(MirageMod* mod, const char* key, const char* fallback, char* buffer,
                                      uint32_t capacity);
    void(MIRAGE_CALL* config_set)(MirageMod* mod, const char* key, const char* value);    /* Written right away */

    /* ---- Drawing: only in on_render, in screen pixels, colors 0xAABBGGRR (as ImGui's IM_COL32) ---------------- */
    void(MIRAGE_CALL* screen_size)(float* width, float* height);
    void(MIRAGE_CALL* draw_text)(float x, float y, uint32_t color, const char* text);
    void(MIRAGE_CALL* draw_line)(float x1, float y1, float x2, float y2, uint32_t color, float thickness);
    void(MIRAGE_CALL* draw_rect)(float x1, float y1, float x2, float y2, uint32_t color, float thickness);
    void(MIRAGE_CALL* draw_rect_filled)(float x1, float y1, float x2, float y2, uint32_t color);
    void(MIRAGE_CALL* text_size)(const char* text, float* width, float* height);

    /* ---- Settings declared in mod.toml ([settings.<key>]) ---------------------------------------------------------
     * The runtime makes them when their module registers (module_register returns with them in place). Their values
     * are read and written with the setting_get / setting_set functions above (a choice is its option's index as an
     * int, its option's text as text). */
    MirageSetting*(MIRAGE_CALL* setting_find)(MirageMod* mod, const char* key);    /* NULL if the mod has none such */
    int32_t(MIRAGE_CALL* setting_type)(MirageSetting* setting);                    /* MIRAGE_SETTING_* */

    /* ---- Events (step 6). Each callback runs on the thread named at its MIRAGE_EVENT_*. Removed at the mod's unload. */
    MirageSubscription*(MIRAGE_CALL* event_subscribe)(MirageMod* mod, int32_t type, MirageEventFn fn, void* user);
    void(MIRAGE_CALL* event_unsubscribe)(MirageSubscription* subscription);

    /* ---- Function hooks (step 6): the game's UnrealScript functions, by path ("Engine.Actor.SetLocation", a state's
     * "TdGame.TdPawn.Walking.EndState"). Game thread. Hooks of one function run by priority, higher first. A native
     * function (FUNC_Native) can be hooked before and after, without parameters; one called by native index from
     * script isn't seen at all. NULL when it can't be hooked (the log says why); a function not loaded yet (a level's)
     * is hooked when it is. Removed at the mod's unload. */
    MirageHook*(MIRAGE_CALL* hook_function)(MirageMod* mod, const char* function, int32_t when, int32_t priority,
                                            MirageHookFn fn, void* user);
    void(MIRAGE_CALL* hook_remove)(MirageHook* hook);

    /* ---- Calls: what a hook sees, or a call the mod makes (call_begin, set its parameters, call_invoke, read its
     * out parameters and "ReturnValue", call_end). Game thread. */
    MirageObject*(MIRAGE_CALL* call_object)(MirageCall* call);          /* The object it's called on */
    const char*(MIRAGE_CALL* call_function)(MirageCall* call);          /* "Engine.Actor.SetLocation" */
    void(MIRAGE_CALL* call_skip)(MirageCall* call);                     /* A before hook: the function doesn't run */
    int32_t(MIRAGE_CALL* call_get)(MirageCall* call, const char* path, MirageValue* out);       /* 0: no such parameter */
    int32_t(MIRAGE_CALL* call_set)(MirageCall* call, const char* path, const MirageValue* in);  /* 0: not set */
    MirageCall*(MIRAGE_CALL* call_begin)(MirageObject* object, const char* function);  /* Its class's (or a parent's) */
    int32_t(MIRAGE_CALL* call_invoke)(MirageCall* call);                /* 1: it ran */
    void(MIRAGE_CALL* call_end)(MirageCall* call);

    /* ---- Game objects (step 6). Game thread. NULL while loading or outside a level. */
    MirageObject*(MIRAGE_CALL* player_pawn)(void);
    MirageObject*(MIRAGE_CALL* player_controller)(void);
    MirageObject*(MIRAGE_CALL* world_info)(void);
    MirageObject*(MIRAGE_CALL* object_find)(const char* full_name);    /* "TdGame.Default__TdPawn", "Class Engine.Actor" */
    int32_t(MIRAGE_CALL* object_get)(MirageObject* object, const char* path, MirageValue* out);
    int32_t(MIRAGE_CALL* object_set)(MirageObject* object, const char* path, const MirageValue* in);
    uint32_t(MIRAGE_CALL* object_name)(MirageObject* object, char* buffer, uint32_t capacity);         /* "TdPlayerPawn_0" */
    uint32_t(MIRAGE_CALL* object_class_name)(MirageObject* object, char* buffer, uint32_t capacity);   /* "TdPlayerPawn" */
    int32_t(MIRAGE_CALL* object_is_a)(MirageObject* object, const char* class_name);   /* An instance of it or a subclass */

    /* ---- Native code (step 6): detours of the game's functions, chained by the runtime ----------------------------
     * Several mods may detour one function: they run by priority, higher first, each calling its *original (the next
     * detour, or the real function); the runtime keeps every *original right as detours come and go, so keep it in a
     * variable that lives as long as the mod. A function the menu hooks itself is refused. Removed at the mod's unload. */
    MirageDetour*(MIRAGE_CALL* detour_create)(MirageMod* mod, void* target, void* detour, int32_t priority, void** original);
    void(MIRAGE_CALL* detour_remove)(MirageDetour* detour);
    /* A byte pattern in the game's code ('x' in `mask` must match, '?' is any byte), NULL if not found */
    void*(MIRAGE_CALL* find_pattern)(const char* pattern, const char* mask);

    /* ---- Script patching (step 8; see MirageToken). Game thread. -------------------------------------------------
     * Token indices are 0-based; script_children / script_set_children with index -1 are the top-level statements. */
    MirageScript*(MIRAGE_CALL* script_open)(MirageMod* mod, const char* function);   /* NULL: none, or no script */
    void(MIRAGE_CALL* script_close)(MirageScript* script);
    int32_t(MIRAGE_CALL* script_count)(MirageScript* script);                        /* Tokens */
    int32_t(MIRAGE_CALL* script_get)(MirageScript* script, int32_t index, MirageToken* out);       /* 0: no such */
    int32_t(MIRAGE_CALL* script_set)(MirageScript* script, int32_t index, const MirageToken* in);  /* 0: not set */
    int32_t(MIRAGE_CALL* script_add)(MirageScript* script, const MirageToken* in);   /* Its index, -1 if refused */
    /* Copies up to `capacity` indices and returns how many there are (-1: no such token) */
    int32_t(MIRAGE_CALL* script_children)(MirageScript* script, int32_t index, int32_t* out, int32_t capacity);
    int32_t(MIRAGE_CALL* script_set_children)(MirageScript* script, int32_t index, const int32_t* children, int32_t count);
    int32_t(MIRAGE_CALL* script_apply)(MirageScript* script);        /* 1: in place (the log says why not) */
    int32_t(MIRAGE_CALL* script_restore)(MirageMod* mod, const char* function);      /* 1: it was patched */
    uint32_t(MIRAGE_CALL* script_dump)(MirageScript* script, char* buffer, uint32_t capacity);    /* Full length */
    int32_t(MIRAGE_CALL* script_kind)(const char* name);            /* -1: no such kind */
    const char*(MIRAGE_CALL* script_kind_name)(int32_t kind);        /* "Bad" if none */

    /* ---- The menu (step 9) ---------------------------------------------------------------------------------------- */
    void(MIRAGE_CALL* notify)(MirageMod* mod, const char* text);   /* A notification on screen for a few seconds. Any thread */
} MirageApi;

/* The runtime has `field` (a mod built against a newer header, running on an older runtime) */
#define MIRAGE_API_HAS(api, field) \
    ((api)->size >= offsetof(MirageApi, field) + sizeof((api)->field) && (api)->field != NULL)

/* What a native mod exports */
typedef uint32_t(MIRAGE_CALL* MirageModAbiFn)(void);
typedef int32_t(MIRAGE_CALL* MirageModLoadFn)(const MirageApi* api, MirageMod* mod);
typedef void(MIRAGE_CALL* MirageModUnloadFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* MIRAGE_API_H */
