# Included by CMake (via CMAKE_USER_MAKE_RULES_OVERRIDE) before it sets its default compile rules.
# FASTBuild preprocesses sources with -frewrite-includes, which emits GNU line markers; projects that add
# -Wpedantic -Werror then fail with -Wgnu-line-marker. Target flags come after CMAKE_<LANG>_FLAGS and a later
# -Wpedantic re-enables the warning, so the suppression has to go at the very end of the command line.
set(CMAKE_C_COMPILE_OBJECT "<CMAKE_C_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -Wno-gnu-line-marker -o <OBJECT> -c <SOURCE>")
set(CMAKE_CXX_COMPILE_OBJECT "<CMAKE_CXX_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -Wno-gnu-line-marker -o <OBJECT> -c <SOURCE>")
