# AppleWin patch notes

The source archive had an exported Git patch with four malformed encoding-only hunks touching the first lines of Visual Studio XML files. These hunks were removed without changing the remaining substantive diff.

This patch targets upstream AppleWin commit `4a43bcdb8e1cb908662f4c270ca84f309a97802f`. It has been checked for patch formatting, **not** independently applied or built against the upstream tree. The new source files in `src/` must be installed under AppleWin `source/` before compiling.

Example for a clean checkout:

```powershell
git checkout 4a43bcdb8e1cb908662f4c270ca84f309a97802f
git apply --check .\AppleWin-integration.patch
git apply .\AppleWin-integration.patch
Copy-Item .\src\*.h .\AppleWin\source\
Copy-Item .\src\*.cpp .\AppleWin\source\
```

The example is schematic: adjust working directory and paths before use. Prefer reviewing and applying the patch in a clean *separate* checkout.
