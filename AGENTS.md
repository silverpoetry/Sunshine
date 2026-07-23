On Windows we use msys2 and ucrt64 to compile.
Use the MSYS2/UCRT64 environment under `E:\Develop\MSYS2\msys64`.

Preferred command shape:

`E:\Develop\MSYS2\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /d/Projects/sunshine && cmake -S . -B build-release-e-msys2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF && cmake --build build-release-e-msys2 --config Release -j 12"`

Use `build-release-e-msys2` for the local optimized Release build. Keep docs disabled unless explicitly needed, because this machine does not keep Doxygen/Graphviz in the normal build environment.

The test executable is named `test_sunshine` and will be located inside the `tests` directory within
the build directory.

The project uses gtest as a test framework.

Always follow the style guidelines defined in .clang-format for c/c++ code.

When adding localization do not update any language other than `en`. This also means to exclude en-US or other variants.
