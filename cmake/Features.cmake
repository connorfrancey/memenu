# Features: which modules go into medge_menu.dll
#
# Every module under src/features/ has its own option, MEMENU_FEATURE_<NAME>, grouped by category
# (MEMENU_FEATURES_<CATEGORY>) under one master switch (MEMENU_FEATURES). A feature is built when all three are ON;
# turning a category or the master switch off forces its features off without losing their own setting.
#
#   cmake ... -D MEMENU_FEATURES=OFF                 just the menu GUI: no modules, no game integration
#   cmake ... -D MEMENU_FEATURES_DEBUG=OFF           everything except the Debug tab
#   cmake ... -D MEMENU_FEATURE_GOD_MODE=OFF         everything except God Mode
#
# Features marked GAME need the game integration (src/game, src/render, the engine tick hook and the Mirror's Edge
# SDK). When none of the enabled features needs it, that layer is left out too (MEMENU_GAME, set below), and the DLL
# is only the D3D9 hook, the menu, themes and the config file.
#
# Staged build (MEMENU_MINIMAL, `memenu minimal --stage N`): the feature options are ignored and the build is
# everything up to stage MEMENU_MINIMAL_STAGE, in the order below: least likely to break the game first. Stages 0-2
# are the bare hook (src/minimal/ instead of the menu), 3 is the real menu with no features, and every feature's
# STAGE says when it comes back. The last stage is the whole trainer, every feature on (TAS included).
#
# Sets, for CMakeLists.txt and cmake/Libraries.cmake:
#   MEMENU_FEATURE_SOURCES   .cpp files of the enabled features (and the game layer when it is needed)
#   MEMENU_ENABLED_FEATURES  their names
#   MEMENU_GAME              ON when an enabled feature needs the game layer (compile definition MEMENU_GAME=0/1)
#   MEMENU_BARE              ON for the staged build's stages 0-2: hooks.cpp + src/minimal/, no menu
#   MEMENU_STAGE             the staged build's stage (-1 when not staged)

include(CMakeDependentOption)

# -------------------------------------------------------------------------------------------------
# Stages of the staged build, in order. memenu_stage(<n> "<what it adds>"): `memenu minimal` shows these.
set(MEMENU_STAGE_LOGGING 1)
set(MEMENU_STAGE_THEMES 2)
set(MEMENU_STAGE_CORE 3)
set(MEMENU_STAGES "")
function(memenu_stage n description)
  list(APPEND MEMENU_STAGES ${n})
  set(MEMENU_STAGES "${MEMENU_STAGES}" PARENT_SCOPE)
endfunction()

memenu_stage(0  "D3D9 hook + ImGui demo window (INSERT)")
memenu_stage(1  "logging: spdlog, memenu.log")
memenu_stage(2  "themes: json, imjson, the theme files (on the demo window)")
memenu_stage(3  "the real menu: module system, config, keybinds, menu GUI; no features")
memenu_stage(4  "menu-only features that never touch the game, and the not-implemented stubs")
memenu_stage(5  "HUD overlays drawn every frame")
memenu_stage(6  "game integration: SDK, pattern scans, engine tick hook, StretchRect + input blocking; Game Status (read-only)")
memenu_stage(7  "reads the player every frame: Speedometer")
memenu_stage(8  "level actions: loading levels and streaming sublevels")
memenu_stage(9  "writes to the player and the world: God Mode, Game Speed, Save State")
memenu_stage(10 "takes over movement: Flight, No Clip and the trainer tools on top of them")
memenu_stage(11 "Collision View: debug renderer, world geometry, drawing inside the scene")
memenu_stage(12 "TAS / replay: virtual clock, input playback. Everything is on")
list(GET MEMENU_STAGES -1 MEMENU_LAST_STAGE)

if(MEMENU_MINIMAL)
  set(MEMENU_STAGE ${MEMENU_MINIMAL_STAGE})
  if(NOT MEMENU_STAGE MATCHES "^[0-9]+$" OR MEMENU_STAGE GREATER MEMENU_LAST_STAGE)
    message(FATAL_ERROR "MEMENU_MINIMAL_STAGE must be 0-${MEMENU_LAST_STAGE} (got '${MEMENU_STAGE}')")
  endif()
  if(MEMENU_STAGE LESS MEMENU_STAGE_CORE)
    set(MEMENU_BARE ON)
  else()
    set(MEMENU_BARE OFF)
  endif()
else()
  set(MEMENU_STAGE -1)
  set(MEMENU_BARE OFF)
endif()

# -------------------------------------------------------------------------------------------------
# Options (not used by the staged build)
if(NOT MEMENU_MINIMAL)
  option(MEMENU_FEATURES "Build the menu's features (OFF: only the menu GUI, no in-game functionality)" ON)
  set(MEMENU_FEATURE_CATEGORIES player trainer tas world visual debug levels misc)
  foreach(category IN LISTS MEMENU_FEATURE_CATEGORIES)
    string(TOUPPER "${category}" CAT)
    cmake_dependent_option(MEMENU_FEATURES_${CAT} "Build the ${category} features" ON "MEMENU_FEATURES" OFF)
  endforeach()
endif()

set(MEMENU_FEATURE_SOURCES "")
set(MEMENU_ALL_FEATURE_SOURCES "")   # Every file any feature lists, enabled or not (CMakeLists.txt checks against it)
set(MEMENU_GAME OFF)
set(MEMENU_ENABLED_FEATURES "")
set(MEMENU_DISABLED_FEATURES "")

# memenu_feature(<name> <category> [GAME] [OFF] STAGE <n> FILES <files relative to src/>...)
#   GAME   the feature needs the game integration
#   OFF    the feature defaults to off (normal build)
#   STAGE  when the staged build adds it back (see memenu_stage above): rank new features by how likely they are
#          to break the game
function(memenu_feature name category)
  cmake_parse_arguments(PARSE_ARGV 2 F "GAME;OFF" "STAGE" "FILES")
  string(TOUPPER "${name}" NAME)
  string(TOUPPER "${category}" CAT)
  if(NOT DEFINED F_STAGE OR NOT F_STAGE IN_LIST MEMENU_STAGES OR F_STAGE LESS MEMENU_STAGE_CORE)
    message(FATAL_ERROR "memenu_feature(${name}): needs STAGE <n>, one of the memenu_stage() numbers from "
                        "${MEMENU_STAGE_CORE} up (cmake/Features.cmake)")
  endif()

  if(MEMENU_MINIMAL)
    if(F_STAGE LESS_EQUAL MEMENU_STAGE)
      set(enabled ON)
    else()
      set(enabled OFF)
    endif()
  else()
    set(default ON)
    if(F_OFF)
      set(default OFF)
    endif()
    cmake_dependent_option(MEMENU_FEATURE_${NAME} "Build the ${name} feature (${category})" ${default}
      "MEMENU_FEATURES;MEMENU_FEATURES_${CAT}" OFF)
    set(enabled ${MEMENU_FEATURE_${NAME}})
  endif()

  foreach(file IN LISTS F_FILES)
    list(APPEND MEMENU_ALL_FEATURE_SOURCES "${PROJECT_SOURCE_DIR}/src/${file}")
  endforeach()
  set(MEMENU_ALL_FEATURE_SOURCES "${MEMENU_ALL_FEATURE_SOURCES}" PARENT_SCOPE)

  if(enabled)
    foreach(file IN LISTS F_FILES)
      list(APPEND MEMENU_FEATURE_SOURCES "${PROJECT_SOURCE_DIR}/src/${file}")
    endforeach()
    list(REMOVE_DUPLICATES MEMENU_FEATURE_SOURCES)   # Helpers shared by several features (fly_module.cpp)
    list(APPEND MEMENU_ENABLED_FEATURES ${name})
    if(F_GAME)
      set(MEMENU_GAME ON PARENT_SCOPE)
    endif()
  else()
    list(APPEND MEMENU_DISABLED_FEATURES ${name})
  endif()
  set(MEMENU_FEATURE_SOURCES "${MEMENU_FEATURE_SOURCES}" PARENT_SCOPE)
  set(MEMENU_ENABLED_FEATURES "${MEMENU_ENABLED_FEATURES}" PARENT_SCOPE)
  set(MEMENU_DISABLED_FEATURES "${MEMENU_DISABLED_FEATURES}" PARENT_SCOPE)
endfunction()

# -------------------------------------------------------------------------------------------------
# Player
memenu_feature(flight_mode       player GAME STAGE 10 FILES features/player/flight_mode.cpp features/player/fly_module.cpp)
memenu_feature(no_clip           player GAME STAGE 10 FILES features/player/no_clip.cpp features/player/fly_module.cpp)
memenu_feature(god_mode          player GAME STAGE 9  FILES features/player/god_mode.cpp)
memenu_feature(teleport_to_spawn player GAME STAGE 9  FILES features/player/teleport_to_spawn.cpp)

# Trainer
memenu_feature(beamer            trainer GAME STAGE 10 FILES features/trainer/beamer.cpp)
memenu_feature(kickglitch        trainer GAME STAGE 10 FILES features/trainer/kickglitch.cpp)
memenu_feature(strang            trainer GAME STAGE 10 FILES features/trainer/strang.cpp)
memenu_feature(save_state        trainer GAME STAGE 9  FILES features/trainer/save_state.cpp)
memenu_feature(speedometer       trainer GAME STAGE 7  FILES features/trainer/speedometer.cpp)
memenu_feature(tool_overlay      trainer      STAGE 5  FILES features/trainer/tool_overlay.cpp)

# TAS / replay: temporarily off by default
memenu_feature(tas               tas GAME OFF STAGE 12 FILES features/tas/tas_module.cpp tas/engine.cpp tas/replay.cpp tas/tas.cpp)

# World
memenu_feature(game_speed        world GAME STAGE 9 FILES features/world/game_speed.cpp)
memenu_feature(freeze_timer      world GAME STAGE 9 FILES features/world/freeze_timer.cpp)

# Visual
memenu_feature(crosshair         visual STAGE 5 FILES features/visual/crosshair.cpp)
memenu_feature(fov               visual GAME STAGE 9 FILES features/visual/fov.cpp)
memenu_feature(show_fps          visual STAGE 5 FILES features/visual/show_fps.cpp)

# Debug
memenu_feature(collision_view    debug GAME STAGE 11 FILES features/debug/collision_view.cpp)
memenu_feature(log_viewer        debug      STAGE 4  FILES features/debug/log_viewer.cpp)
memenu_feature(performance       debug      STAGE 5  FILES features/debug/performance.cpp)

# Levels
memenu_feature(level_loader      levels GAME STAGE 8 FILES features/levels/level_loader.cpp)
memenu_feature(quick_actions     levels GAME STAGE 8 FILES features/levels/quick_actions.cpp)
memenu_feature(sublevels         levels GAME STAGE 8 FILES features/levels/sublevels.cpp)

# Misc
memenu_feature(config_actions    misc      STAGE 4 FILES features/misc/config_actions.cpp)
memenu_feature(game_status       misc GAME STAGE 6 FILES features/misc/game_status.cpp)
memenu_feature(themes            misc      STAGE 4 FILES features/misc/themes.cpp)
memenu_feature(watermark         misc      STAGE 5 FILES features/misc/watermark.cpp)

# -------------------------------------------------------------------------------------------------
# The game integration layer, only when something uses it
if(MEMENU_GAME)
  file(GLOB MEMENU_GAME_SOURCES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/game/*.cpp
    ${PROJECT_SOURCE_DIR}/src/render/*.cpp)
  list(APPEND MEMENU_FEATURE_SOURCES ${MEMENU_GAME_SOURCES})
endif()

list(LENGTH MEMENU_ENABLED_FEATURES _n_on)
list(LENGTH MEMENU_DISABLED_FEATURES _n_off)
if(MEMENU_MINIMAL)
  message(STATUS "memenu: staged build, stage ${MEMENU_STAGE} of ${MEMENU_LAST_STAGE}")
endif()
message(STATUS "memenu: ${_n_on} feature(s) enabled, ${_n_off} disabled, game integration ${MEMENU_GAME}")
if(MEMENU_DISABLED_FEATURES)
  string(REPLACE ";" ", " _off "${MEMENU_DISABLED_FEATURES}")
  message(STATUS "memenu: disabled: ${_off}")
endif()
