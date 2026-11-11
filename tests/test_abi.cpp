// The Mirage mod ABI (api/mirage/api.h) is frozen: these are the sizes and offsets of ABI v1 on the game's 32-bit
// target. A failure here means a change that breaks mods already built: append to MirageApi instead (and add the new
// fields' offsets below), never insert, remove, reorder or retype.
#include "mirage/api.h"

#include <gtest/gtest.h>

#include <cstddef>

static_assert(sizeof(void*) == 4, "the ABI is frozen for the game's 32-bit target");

// ---- ABI v1 ------------------------------------------------------------------------------------------------------------
static_assert(MIRAGE_ABI_VERSION >= 1);
static_assert(MIRAGE_LOG_DEBUG == 0 && MIRAGE_LOG_INFO == 1 && MIRAGE_LOG_WARN == 2 && MIRAGE_LOG_ERROR == 3);
static_assert(MIRAGE_MODULE_ACTION == 0x1u && MIRAGE_MODULE_ENABLED_BY_DEFAULT == 0x2u);

static_assert(sizeof(MirageModuleDesc) == 52);
static_assert(offsetof(MirageModuleDesc, size) == 0);
static_assert(offsetof(MirageModuleDesc, name) == 4);
static_assert(offsetof(MirageModuleDesc, tab) == 8);
static_assert(offsetof(MirageModuleDesc, description) == 12);
static_assert(offsetof(MirageModuleDesc, order) == 16);
static_assert(offsetof(MirageModuleDesc, key) == 20);
static_assert(offsetof(MirageModuleDesc, flags) == 24);
static_assert(offsetof(MirageModuleDesc, user) == 28);
static_assert(offsetof(MirageModuleDesc, on_enable) == 32);
static_assert(offsetof(MirageModuleDesc, on_disable) == 36);
static_assert(offsetof(MirageModuleDesc, on_tick) == 40);
static_assert(offsetof(MirageModuleDesc, on_render) == 44);
static_assert(offsetof(MirageModuleDesc, on_trigger) == 48);

#define MIRAGE_AT(field, offset) static_assert(offsetof(MirageApi, field) == (offset), #field " moved")
MIRAGE_AT(size, 0);
MIRAGE_AT(abi_version, 4);
MIRAGE_AT(runtime_version, 8);
MIRAGE_AT(log, 12);
MIRAGE_AT(mod_id, 16);
MIRAGE_AT(mod_version, 20);
MIRAGE_AT(mod_dir, 24);
MIRAGE_AT(module_register, 28);
MIRAGE_AT(module_enabled, 32);
MIRAGE_AT(module_set_enabled, 36);
MIRAGE_AT(setting_checkbox, 40);
MIRAGE_AT(setting_slider, 44);
MIRAGE_AT(setting_slider_int, 48);
MIRAGE_AT(setting_text, 52);
MIRAGE_AT(setting_key, 56);
MIRAGE_AT(setting_get_int, 60);
MIRAGE_AT(setting_get_float, 64);
MIRAGE_AT(setting_set_int, 68);
MIRAGE_AT(setting_set_float, 72);
MIRAGE_AT(setting_get_text, 76);
MIRAGE_AT(setting_set_text, 80);
MIRAGE_AT(key_down, 84);
MIRAGE_AT(config_get, 88);
MIRAGE_AT(config_set, 92);
MIRAGE_AT(screen_size, 96);
MIRAGE_AT(draw_text, 100);
MIRAGE_AT(draw_line, 104);
MIRAGE_AT(draw_rect, 108);
MIRAGE_AT(draw_rect_filled, 112);
MIRAGE_AT(text_size, 116);
MIRAGE_AT(setting_find, 120);
MIRAGE_AT(setting_type, 124);
MIRAGE_AT(event_subscribe, 128);
MIRAGE_AT(event_unsubscribe, 132);
MIRAGE_AT(hook_function, 136);
MIRAGE_AT(hook_remove, 140);
MIRAGE_AT(call_object, 144);
MIRAGE_AT(call_function, 148);
MIRAGE_AT(call_skip, 152);
MIRAGE_AT(call_get, 156);
MIRAGE_AT(call_set, 160);
MIRAGE_AT(call_begin, 164);
MIRAGE_AT(call_invoke, 168);
MIRAGE_AT(call_end, 172);
MIRAGE_AT(player_pawn, 176);
MIRAGE_AT(player_controller, 180);
MIRAGE_AT(world_info, 184);
MIRAGE_AT(object_find, 188);
MIRAGE_AT(object_get, 192);
MIRAGE_AT(object_set, 196);
MIRAGE_AT(object_name, 200);
MIRAGE_AT(object_class_name, 204);
MIRAGE_AT(object_is_a, 208);
MIRAGE_AT(detour_create, 212);
MIRAGE_AT(detour_remove, 216);
MIRAGE_AT(find_pattern, 220);
// Size of ABI v1's table: a later version only makes it larger
static_assert(sizeof(MirageApi) >= 224);
static_assert(sizeof(MirageValue) == 272 && offsetof(MirageValue, text) == 16);
static_assert(sizeof(MirageEvent) == 24 && offsetof(MirageEvent, number) == 16 && offsetof(MirageEvent, text) == 20);
static_assert(MIRAGE_HOOK_BEFORE == 0 && MIRAGE_HOOK_AFTER == 1 && MIRAGE_HOOK_REPLACE == 2);
static_assert(MIRAGE_EVENT_TICK == 0 && MIRAGE_EVENT_LEVEL_LOAD == 1 && MIRAGE_EVENT_DEATH == 2 &&
              MIRAGE_EVENT_PLAYER_STATE == 3 && MIRAGE_EVENT_KEY == 4 && MIRAGE_EVENT_STATE_RESTORED == 5);
static_assert(MIRAGE_VALUE_NONE == 0 && MIRAGE_VALUE_TEXT == 5 && MIRAGE_VALUE_STRUCT == 6);
static_assert(MIRAGE_SETTING_BOOL == 0 && MIRAGE_SETTING_INT == 1 && MIRAGE_SETTING_FLOAT == 2 &&
              MIRAGE_SETTING_STRING == 3 && MIRAGE_SETTING_CHOICE == 4 && MIRAGE_SETTING_KEY == 5);

TEST(Abi, NewerRuntimeTableCoversOlderMods)
{
	// A runtime whose table stops before a field doesn't have it, as a mod built against a newer header sees it
	MirageApi api{};
	api.size = offsetof(MirageApi, text_size);
	api.text_size = nullptr;
	EXPECT_FALSE(MIRAGE_API_HAS(&api, text_size));
	api.size = sizeof(MirageApi);
	api.text_size = [](const char*, float*, float*) {};
	EXPECT_TRUE(MIRAGE_API_HAS(&api, text_size));
}
