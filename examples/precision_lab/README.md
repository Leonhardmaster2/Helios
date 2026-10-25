# Headless Diligent precision lab

This executable is the Vulkan migration proof for Helios's large-world
precision boundary. It creates no SDL object, swap chain, or native window and
never reads desktop, mouse, or keyboard state.

The contract under test is:

1. Store camera and object world positions as binary64 values.
2. Subtract the camera origin on the CPU in binary64.
3. Reject camera-relative components outside the lab's 16,384 m bound.
4. Upload only those bounded values as float32.
5. Apply an orthonormal view transform in a Diligent compute pipeline on the
   Vulkan backend.
6. Read 128 bytes back and compare every result to a binary64 CPU reference
   using a predeclared IEEE-754 error bound.

Four base/moved pairs cover near-planet, one-AU, outer-solar-system, and
one-light-year world scales. The JSON report also computes an intentionally
incorrect baseline (`float(objectWorld) - float(cameraWorld)`) to quantify why
absolute world positions must not be converted to float before rebasing.

The `--ctest` path is permanently bounded to one device creation, one compute
dispatch containing eight shader invocations, one 128-byte copy, and one
readback. The test is registered only for the explicit `gpu` CTest
opt-in and has a 30-second timeout. Register the physical-GPU checks with:

```powershell
cmake --preset windows-dev -DHELIOS_ENABLE_GPU_TESTS=ON
cmake --build --preset windows-dev
ctest --test-dir build/windows-dev -C RelWithDebInfo -L gpu --output-on-failure
```

`--no-validation` is available for drivers without the Vulkan validation
layer, but normal validation keeps it enabled.

It relies on the root project having already defined
`Diligent-GraphicsEngineVk-static`, `Diligent-Common`, `helios_shader_data`,
`helios_json`, and `helios_configure_target`. The shader is compiled and
embedded by the canonical root shader pipeline. It deliberately does not link
`helios_vk`, `helios_render`, or `helios_diligent_render`, so the process
contains only Diligent's Vulkan loader path.
