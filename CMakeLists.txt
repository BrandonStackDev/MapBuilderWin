cmake_minimum_required(VERSION 3.20)
project(MapCreate C)
set(CMAKE_C_STANDARD 99)

# Where you installed raylib with "cmake --install"
list(APPEND CMAKE_PREFIX_PATH "C:/raylib")
find_package(raylib CONFIG REQUIRED)

add_executable(create main.c)
add_executable(play preview.c)

foreach(tgt IN ITEMS create play)
  set_target_properties(${tgt} PROPERTIES LINK_FLAGS "/STACK:16777216")
  target_link_libraries(${tgt} PRIVATE raylib opengl32 gdi32 winmm user32 shell32)
  target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_include_directories(${tgt} PRIVATE "C:/raylib/include")
  # Set stack size to 16 MB (default is ~1 MB)
endforeach()
