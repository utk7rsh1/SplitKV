# SplitKV

A persistent key-value storage engine written in C++17.

I started this project to learn how database storage engines work at a low level. I kept reading about LSM trees and write amplification and wanted to understand the concepts by building something from scratch.

The design is based on the WiscKey paper, which separates keys from values so that compaction only needs to sort keys and not move large values around.

## Building

You need CMake 3.16+ and a C++17 compiler.

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

More documentation coming soon.
