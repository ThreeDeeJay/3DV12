# 3DV12 — Direct3D 12 Stereo Shader Patcher

A drop-in **d3d12.dll proxy** that automatically patches shaders in Direct3D 12 games for correct stereoscopic 3D output via **NVIDIA 3D Vision** (NVAPI).  Inspired by [3DMigoto](https://github.com/bo3b/3Dmigoto), rebuilt from scratch for D3D12 with native SM5 bytecode injection.

## How it works

Place `d3d12.dll` next to the game executable.  The proxy intercepts every call to:

| Hook | What happens |
|---|---|
| `D3D12CreateDevice` | Returns a `WrappedDevice`; initialises NVAPI stereo |
| `CreateGraphicsPipelineState` | VS/PS bytecode is analysed and patched in-memory |
| `CreateComputePipelineState` | CS bytecode is patched for deferred/AO/shadow passes |
| `CreateRootSignature` | A stereo CBV parameter (`b13, space13`) is appended |
| `CreateCommandList` | Returns a `WrappedCommandList` |
| Draw / Dispatch | Stereo params are bound as a constant buffer before every call |

### Shader patching

Shaders are classified and patched at PSO creation time — no runtime HLSL compilation needed.

**Vertex shaders** receive a position correction block before `ret`:
```hlsl
// injected SM5 tokens (equivalent HLSL shown for clarity)
float offset = (pos.w - cb_stereo[0].convergence) * cb_stereo[0].separation;
pos.x += offset * cb_stereo[0].eyeSign;   // eyeSign = -0.5 (L) or +0.5 (R)
```

**Deferred / post-process pixel and compute shaders** (shadow maps, ambient occlusion, screen-space reflections, lighting accumulation) receive an eye-index load so they can reconstruct correct per-eye view rays and depths.

Classification uses reflection data (SRV/UAV/CB counts, SV_Depth writes) — the same heuristic 3D Vision Automatic uses, extended for compute.  DXIL (SM6+) shaders are detected and skipped gracefully (a DXC-based engine for SM6 is planned).

### Stereo constant buffer layout (`b13, space13`)

| Component | Value |
|---|---|
| `.x` | `separation` — eye separation fraction (NVAPI 0–100 → 0.0–1.0, scaled by INI) |
| `.y` | `convergence` — convergence depth (NVAPI units, scaled by INI) |
| `.z` | `eyeSign` — `-0.5` left eye, `+0.5` right eye |
| `.w` | reserved / padding |

## Requirements

- Windows 10/11, 64-bit or 32-bit
- NVIDIA GPU with 3D Vision driver support
- Direct3D 12 game using SM4/5 shaders (SM6/DXIL: pass-through, not patched)
- NVAPI (`nvapi64.dll` / `nvapi.dll`) present — installed automatically with NVIDIA drivers

## Building

### Prerequisites

- CMake ≥ 3.22
- MSVC (Visual Studio 2019 or 2022, with "Desktop development with C++" workload)
- Windows SDK ≥ 10.0.19041

### x64 (Release)

```powershell
cmake -B build-x64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-x64 --config Release --parallel
```

Output: `build-x64/Release/d3d12.dll`

### x86 (32-bit, Release)

```powershell
cmake -B build-x86 -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86 --config Release --parallel
```

### Optional: NVAPI SDK

If you have the NVIDIA NVAPI SDK, pass its root directory to get type-safe headers instead of the built-in forward declarations:

```powershell
cmake -B build-x64 -A x64 -DNVAPI_SDK_DIR=C:/nvapi
```

### GitHub Actions

Push to `main`, `master`, or `dev`; a workflow builds x64 + x86 × Release + Debug and uploads the DLLs as artifacts.  The `.github/` folder is explicitly preserved by the checkout step.

## Installation

1. Build `d3d12.dll` (x64 for 64-bit games, x86 for 32-bit games).
2. Copy `d3d12.dll` and `3DV12.ini.default` → rename `3DV12.ini.default` to `3DV12.ini` → place both next to the game `.exe`.
3. Enable **NVIDIA 3D Vision** in the driver control panel.
4. Launch the game.

Logs are written to `3DV12.log` by default (configurable in the INI).

## Configuration (`3DV12.ini`)

```ini
[General]
LogFile  = 3DV12.log
LogLevel = Info          ; Off | Error | Warn | Info | Debug | Trace

[Stereo]
Enabled             = true
SeparationScale     = 1.0   ; Multiplied on top of NVAPI separation
ConvergenceScale    = 1.0
AutoDetect          = true  ; Heuristic deferred-pass detection
PatchVertexShaders  = true
PatchPixelShaders   = true
PatchComputeShaders = true
StereoConstantBuffer = 13   ; cbuffer register (b<N>, space13)
StereoRootParamIndex = -1   ; -1 = auto-append to root signature

[Debug]
DumpShaders   = false        ; Write pre/post bytecode to DumpDir
DumpDir       = ShaderDump
LogShaderHash = true
```

## Project structure

```
3DV12/
├── src/
│   ├── version.h               # Version integer (bump for every release)
│   ├── version.rc.in           # PE version resource template
│   ├── log.h / log.cpp         # Thread-safe levelled logger
│   ├── config.h / config.cpp   # INI parser, global Config struct
│   ├── dxbc_parser.h / .cpp    # DXBC r/w, SM5 token encoder, MD5
│   ├── shader_patcher.h / .cpp # Classifier + PatchBlob orchestration
│   ├── stereo_engine.h / .cpp  # NVAPI runtime loader, StereoParams
│   ├── wrapped_device.h / .cpp # ID3D12Device5 proxy
│   ├── wrapped_command_list.h / .cpp  # ID3D12GraphicsCommandList4 proxy
│   ├── dllmain.cpp             # DLL entry, D3D12CreateDevice hook
│   └── d3d12.def               # Export table
├── .github/workflows/build.yml
├── CMakeLists.txt
├── 3DV12.ini.default
├── LICENSE                     # GPLv3
└── README.md
```

## Acknowledgements

- [3DMigoto](https://github.com/bo3b/3Dmigoto) — reference for DXBC container layout and stereo injection strategy
- [sView / StDXNVSurface](https://github.com/gkv311/sview) — NVAPI stereo handle patterns
- Microsoft — `d3d10tokenizedprogramformat.hpp` (Windows SDK)

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
