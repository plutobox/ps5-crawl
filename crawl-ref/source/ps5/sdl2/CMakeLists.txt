# SDL2 for the PS5: the project's static build of SDL2 on the console's OpenGL.
# Derived from the PS5 OpenGL SDK's SDL2 integration (BlackBearReloaded,
# GPL-3.0-or-later); the window, context and render driver are altered for
# Apotris (see SDL_ps5gl.c and SDL_render_ps5gl.c).
cmake_minimum_required(VERSION 3.16)
project(SDL2PS5 C)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_PTHREADS ON CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
set(SDL_SOURCE_VERSION "2.30.12-g8c56053f13ca-apotris")
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
# SDL_RENDER stays on: the PS5 OpenGL render driver below is registered in it.
foreach(feature HAPTIC SENSOR OPENGL OPENGLES VULKAN X11
                WAYLAND KMSDRM OFFSCREEN JACK PULSEAUDIO DBUS IBUS FCITX LIBUDEV)
    set(SDL_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(PS5 ON CACHE BOOL "" FORCE)
# The native folder's libc does not export these header-declared symbols.
foreach(symbol WCSLCPY WCSLCAT WCSCASECMP WCSNCASECMP)
    set(HAVE_${symbol} 0 CACHE INTERNAL "native libc contract" FORCE)
endforeach()
add_subdirectory("${SDL_SOURCE}" sdl)

# Replace upstream's OSMesa/scanout video code with the EGL driver of the SDK.
get_target_property(sources SDL2-static SOURCES)
list(FILTER sources EXCLUDE REGEX "/video/ps5/")
set_property(TARGET SDL2-static PROPERTY SOURCES "${sources}")
target_sources(SDL2-static PRIVATE SDL_ps5gl.c SDL_render_ps5gl.c)
target_include_directories(SDL2-static PRIVATE "${SDL_SOURCE}/src" "${PS5_OPENGL_PREFIX}/include")
target_compile_definitions(SDL2-static PRIVATE
    SDL_VIDEO_DRIVER_PS5=1 SDL_VIDEO_OPENGL=1 SDL_VIDEO_RENDER_PS5GL=1)

set_property(TARGET SDL2-static PROPERTY INTERFACE_LINK_LIBRARIES
    "$<INSTALL_INTERFACE:PS5OpenGLCore33::OpenGL>;ScePad;SceUserService;SceSystemService;SceAudioOut")
target_compile_definitions(SDL2-static INTERFACE SDL_MAIN_HANDLED=1)
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/sdl/SDL2Config.cmake"
    "\ninclude(CMakeFindDependencyMacro)\nfind_dependency(PS5OpenGLCore33 CONFIG)\n")
configure_file(sdl2.pc.in "${CMAKE_CURRENT_BINARY_DIR}/sdl/sdl2.pc" @ONLY)
