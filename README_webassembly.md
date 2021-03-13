# WebAssembly Support for Halide

This branch represents an effort to support WebAssembly (Wasm) code generation from Halide, using the Halide backend.

It is very much a work-in-progress at this point; be sure to read all of the current limitations. Some of the most important:

- SIMD can be enabled via Target::WasmSimd128, but won't work (yet), mainly because existing support in the LLVM backend and/or V8 isn't quite solid enough yet; we hope to be able to support this reliably soon.
- Multithreading is not yet ready to be supported, but we hope it will be soon. (One of the main gating factors is finding a test environment that supports wasm threads that will work well with Halide's test suite.)
- Halide's JIT for Wasm is useful only for internal testing purposes.

# Additional Tooling Requirements:
- In additional to the usual install of LLVM and Clang, you'll need wasm-ld (via LLVM/tools/lld). All should be v9.x+ (current trunk)
- V8 library, v7.5+
- d8 shell tool, v7.5+
- Emscripten, 1.38.28+

Note that for all of the above, earlier versions might work, but have not been tested. (We intend to provide more concrete baseline requirements as development continues.)

# AOT Limitations

Halide outputs a Wasm object (.o) or static library (.a) file, must like any other architecture; to use it, of course, you must link it to suitable calling code. Additionally, you must link to something that provides an implementation of `libc`; as a practical matter, this means using the Emscripten tool to do your linking, as it provides the most complete such implementation we're aware of.

- Halide ahead-of-time tests assume/require that you have Emscripten installed and available on your system.

- Halide doesn't support multithreading in Wasm at this point; we obviously would like to do so, but there are practical issues with doing so at the time of this writing:

  - At present, support for Threads in Wasm effectively requires compiling Emscripten with pthreads enabled and running in a complete browser environment; there are no command-line tools (e.g. Node, d8) that we're aware of which support threading in Wasm. The Halide test build and test facility doesn't have any existing way to do in-browser testing, so we are focusing on single-threaded output until we have a way to reliably test.


# JIT Limitations

It's important to reiterate that the JIT mode will never be appropriate for anything other than limited self tests, for a number of reasons:

- It requires linking both an instance of the V8 library and LLVM's wasm-ld tool into libHalide. (We would like to offer support for other Wasm engines in the future, e.g. SpiderMonkey, to provide more balanced testing, but there is no timetable for this.)
- Every JIT invocation requires redundant recompilation of the Halide runtime. (We hope to improve this once the LLVM Wasm backend can support dlopen() properly.)
- Wasm effectively runs in a private, 32-bit memory address space; while the host has access to that entire space, the reverse is not true, and thus any `define_extern` calls require copying all `halide_buffer_t` data across the Wasm<->host boundary in both directions.
- Host functions used via `define_extern` or `HalideExtern` cannot accept or return values that are pointer types or 64-bit integer types; this includes things like `const char *` and `user_context`. (`halide_buffer_t*` is explicitly supported as a special case, however.)
- Threading isn't supported at all (yet); all `parallel()` schedules will be run serially.
- The `.async()` directive isn't supported at all, not even in serial-emulation mode.
- You can't use `Param<void *>` (or any other arbitrary pointer type) with the Wasm jit.
- You can't use `Func.debug_to_file()`, `Func.set_custom_do_par_for()`, `Func.set_custom_do_task()`, or `Func.set_custom_allocator()`.
- The implementation of `malloc()` used by the JIT is incredibly basic and unsuitable for anything other than the most basic of tests.
- GPU usage (or any buffer usage that isn't 100% host-memory) isn't supported at all yet.

Note that while some of these limitations may be improved in the future, some are effectively intrinsic to the nature of this problem. Realistically, this JIT implementation is intended solely for running Halide self-tests (and even then, a number of them are fundamentally impractical to support in a hosted-Wasm environment and are blacklisted).

In sum: don't plan on using Halide JIT mode with Wasm unless you are working on the Halide library itself.

# To Use This Branch:

- Ensure WebAssembly is in LLVM_TARGETS_TO_BUILD:
```
-DLLVM_TARGETS_TO_BUILD="X86;ARM;NVPTX;AArch64;Mips;PowerPC;Hexagon;WebAssembly
```

## Enabling wasm JIT
If you want to run `test_correctness` and other interesting parts of the Halide test suite (and you almost certainly will), you'll need to install libV8 and ensure that LLVM is built with wasm-ld:

- Ensure that you have tools/lld in your LLVM build checkout:
```
svn co https://llvm.org/svn/llvm-project/lld/trunk /path/to/llvm-trunk/tools/lld
```

(You might have to do a clean build of LLVM for CMake to notice that you've added a tool.)

- Install libv8 and the d8 shell tool (instructions omitted), or build from source if you prefer (instructions omitted).

- Set V8_INCLUDE_PATH, V8_LIB_PATH to point to the paths for V8 shared libraries and include files. (If you build from source and are linking static libraries instead of dynamic libraries, also set V8_LIB_EXT to `.a` or similar.)

- Set WITH_V8=1

- To run the JIT tests, set `HL_JIT_TARGET=wasm-32-wasmrt` and run normally. The test suites which we have vetted to work include correctness, performance, error, and warning. (Some of the others could likely be made to work with modest effort.)

## Enabling wasm AOT

If you want to test ahead-of-time code generation (and you almost certainly will), you need to install Emscripten and a shell for running wasm+js code (e.g., d8, part of v8)

- The simplest is probably via the Emscripten emsdk (https://emscripten.org/docs/getting_started/downloads.html).

- After installing Emscripten, be sure that it is configured to use the version of LLVM that you configured earlier, rather than its built-in version; if you installed via `emsdk`, you need to edit `~/.emscripten` and set `LLVM_ROOT` point at the LLVM you have built. (If you fail with errors like `WASM_BACKEND selected but could not find lld (wasm-ld)`, you forgot to do this step)

- Set WASM_SHELL=/path/to/d8

- To run the AOT tests, set `HL_TARGET=wasm-32-wasmrt` and build the `test_aotwasm_generator` target. (Note that the normal AOT tests won't run usefully with this target, as extra logic to run under a wasm-enabled shell is required, and some tests are blacklisted.)

# Known Limitations And Caveats
- We have only tested with EMCC_WASM_BACKEND=1; using the fastcomp backend can probably be made to work but we haven't attempted to do so.
- Using the JIT requires that we link the `wasm-ld` tool into libHalide; with some work this need could (and should) probably be eliminated.
- CMake support hasn't been investigated yet, but should be straightforward.
- OSX and Linux-x64 have been tested. Windows hasn't; it should be supportable with some work. (Patches welcome.)
- apps/ not investigated yet.


TODO:
- 64-bit args
- use headless chrome instead or d8?


