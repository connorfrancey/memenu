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
