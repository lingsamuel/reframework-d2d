# 

## HDR

```sh
cmake -B build/tests -DREFD2D_BUILD_TESTS=ON
cmake --build build/tests --config Debug
ctest --test-dir build/tests -C Debug --output-on-failure
```
