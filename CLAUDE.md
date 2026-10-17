# Kestrel16

## Formatting

After finishing any edit to or addition of C++ code (`.cpp` or `.h`), run clang-format on every file you touched:

```
clang-format -i <file> ...
```

The style is in `.clang-format` at the project root. clang-format is installed at
`C:\Program Files\LLVM\bin\clang-format.exe`; use that full path if `clang-format` isn't on PATH.
