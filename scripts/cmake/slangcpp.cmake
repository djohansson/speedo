# slang's c++ target includes its prelude by its own install's absolute path: by its name instead, which the include path
# finds (the vcpkg tree's include), so that a generated source is the same wherever the checkout is, and FASTBuild's cache
# keys with it (see CMakeLists.txt, the assettest kernels). cmake -DFILE=<generated .cpp> -P slangcpp.cmake
file(READ "${FILE}" text)
string(REGEX REPLACE "#include \"[^\"\n]*/slang-cpp-prelude\\.h\"" "#include \"slang-cpp-prelude.h\"" text "${text}")
file(WRITE "${FILE}" "${text}")
