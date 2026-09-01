# Host validation toolchain

The reproducible host baseline is CMake 3.31.6, Ninja 1.11.1.4, GNU g++ 13.3.0, and C++17 on
Ubuntu 24.04 under WSL. Production targets use only the C++17 standard library, POSIX sockets in the
host adapter, and threads. GoogleTest/GoogleMock is a test-only dependency fixed to 1.14.0 with the
official source archive SHA-256
`8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7`.

Configure and build in a fresh directory outside the repository:

```sh
cmake -S native -B /tmp/fw03-build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFW_PLATFORM=host_posix -DBUILD_TESTING=ON
cmake --build /tmp/fw03-build --parallel
ctest --test-dir /tmp/fw03-build --output-on-failure \
  --output-junit /tmp/fw03-build/test-results.xml
```

For an offline build, unpack the verified official GoogleTest archive outside the repository and set
`FETCHCONTENT_SOURCE_DIR_GOOGLETEST` to that directory. No generated code or downloaded dependency is
stored in the child repository. `android_ndk` and `qnx` are named compatibility slots only; CMake
fails closed until a corresponding licensed SDK adapter exists and compiles.
