# Claude CLI adapter tests

These tests compile the production adapter without capture, graphics, or audio
dependencies. They run a temporary fake CLI; they never use a real Claude login
or contact an AI provider. Python 3, CMake, and a C++20 compiler are required.

```bash
git submodule update --init third-party/googletest
cmake -S tests/standalone/ai_claude_cli -B build/claude-cli
cmake --build build/claude-cli
ctest --test-dir build/claude-cli --output-on-failure
```

The same cases are included in the full native stream suite. For sanitizer
coverage, configure a separate build with
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"`.
