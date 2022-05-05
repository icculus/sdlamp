cmake_minimum_required(VERSION 3.0.0)

project(sdlamp)

find_package(SDL2 REQUIRED)

add_subdirectory(physfs)
add_subdirectory(SDL_sound)

add_executable(sdlamp
    sdlamp.c
    physfs/extras/physfsrwops.c
    physfs/extras/ignorecase.c
)

target_include_directories(sdlamp PRIVATE
    .
    physfs/src
    SDL_sound/src
    ${SDL2_INCLUDE_DIRS} ${SDL2_INCLUDE_DIR}
)

target_link_libraries(sdlamp physfs-static SDL2_sound-static ${SDL2_LIBRARIES} ${SDL2_LIBRARY})

