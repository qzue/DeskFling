add_library(dpp_core
    scene/coordinates.cpp physics/fixed_step_clock.cpp physics/physics_world.cpp
    input/input.cpp render/draw_list.cpp modes/gravity_mode.cpp modes/billiards_mode.cpp
    modes/slingshot_mode.cpp modes/air_hockey_mode.cpp
    modes/game_mode.cpp scene/playground.cpp session/session.cpp)
target_include_directories(dpp_core PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(dpp_core PUBLIC box2d dpp_project_options)
