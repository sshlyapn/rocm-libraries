# Universal GEMM minimal sample (CK Tile)

This directory provides a **small, self-contained** example of using CK Tile **UniversalGemm**
directly (kernel args + kernel launch) without including any headers from other examples/tests.

## Build

From the CK build directory (configured with `GPU_TARGETS`):

```bash
make -j"$(nproc)" tile_example_gemm_universal_minimal
```

The binary is generated at:

```bash
build/bin/tile_example_gemm_universal_minimal
```

## Run

```bash
./build/bin/tile_example_gemm_universal_minimal
```


