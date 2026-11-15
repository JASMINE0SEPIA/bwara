# Build

On Windows x64, install Bitwig Studio 6.1.7.24824, Visual Studio 2022 C++ tools, CMake 3.22+, Git, Python 3 and JDK 25. Run from the project root.

```powershell
.\scripts\setup.ps1
cmake -S . -B build/bwara -G "Visual Studio 17 2022" -A x64
cmake --build build/bwara --config Release --target bwara-host bwara-engine bwara-engine-loader
python scripts/package.py
```

The mod ZIP is in `dist/`. The build adapts a copy of ARA TestHost for live updates and rejects missing patch anchors.

For PCM and atomic revision checks, build `bwara-core-test`, then run `ctest --test-dir build/bwara -C Release --output-on-failure`. These checks do not replace Bitwig playback, export, save/reopen and close-dialog testing.
