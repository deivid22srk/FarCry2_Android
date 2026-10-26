# The game: host runtime + recompiled code from runtime/ and gen/.
# gen/ has ~82k lifted functions in 384 files; -O1 keeps the per-file compile
# around 20-30 s on a runner while staying close to -O2 for this straight-line
# code (the runtime itself is built at -O2).
get_filename_component(PORT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
file(GLOB GEN_SRC ${PORT}/gen/*.c)
file(GLOB RT_SRC ${PORT}/runtime/*.c)
file(GLOB RT_CXX ${PORT}/runtime/*.cpp)

add_library(lbgen OBJECT ${GEN_SRC})
set_property(TARGET lbgen PROPERTY JOB_POOL_COMPILE gen)
target_include_directories(lbgen PRIVATE ${PORT}/runtime ${PORT}/gen)
target_compile_definitions(lbgen PRIVATE GAME_FC2)
target_compile_options(lbgen PRIVATE -std=gnu11 -O1 -mtune=cortex-a73 -g0 -fno-strict-aliasing -fwrapv -fsigned-char -w -fPIC)
add_library(main SHARED ${RT_SRC} ${RT_CXX} $<TARGET_OBJECTS:lbgen>)
target_include_directories(main PRIVATE ${PORT}/runtime ${PORT}/gen ../sdlinc
    ../dxvk/include/native/directx ../dxvk/include/native/windows ../dxvk/include/native)
target_compile_definitions(main PRIVATE DXVK_NATIVE GAME_FC2)
target_compile_options(main PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char -Wno-everything
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)
target_link_libraries(main PRIVATE SDL2 dxvk_d3d9 m z $<$<BOOL:${ANDROID}>:log>)
if(ANDROID)
    target_link_libraries(main PRIVATE adrenotools)
    target_include_directories(main PRIVATE ../adrenotools/include)
endif()
