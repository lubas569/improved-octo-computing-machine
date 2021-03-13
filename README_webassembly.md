# WebAssembly Support for Halide

This branch represents an effort to support WebAssembly (Wasm) code generation from Halide, using the LLVM backend. There isn't yet a timetable for landing this code in the Halide master branch.

It is very much a work-in-progress at this point; be sure to read all of the current limitations. Some of the most important:

- SIMD can be enabled via Target::WasmSimd128, but is unlikely to work work (yet), as existing support in the LLVM backend and/or V8 isn't quite solid enough yet; we hope to be able to support this reliably soon.
- Multithreading is not yet ready to be supported -- there isn't even a Feature flag to enable it yet -- but we hope it will be soon.
- Halide's JIT for Wasm is extremely limited and really useful only for internal testing purposes.

# Additional Tooling Requirements:
- In additional to the usual install of LLVM and Clang, you'll need wasm-ld (via LLVM/tools/lld). All should be v9.x+ (current trunk)
- V8 library, v7.5+
- d8 shell tool, v7.5+
- Emscripten, 1.38.28+

Note that for all of the above, earlier versions might work, but have not been tested. (We intend to provide more concrete baseline requirements as development continues.)

# AOT Limitations

Halide outputs a Wasm object (.o) or static library (.a) file, much like any other architecture; to use it, of course, you must link it to suitable calling code. Additionally, you must link to something that provides an implementation of `libc`; as a practical matter, this means using the Emscripten tool to do your linking, as it provides the most complete such implementation we're aware of.

- Halide ahead-of-time tests assume/require that you have Emscripten installed and available on your system.

- Halide doesn't support multithreading in Wasm just yet; we hope to focus on that soon.

# JIT Limitations

It's important to reiterate that the JIT mode will never be appropriate for anything other than limited self tests, for a number of reasons:

- It requires linking both an instance of the V8 library and LLVM's wasm-ld tool into libHalide. (We would like to offer support for other Wasm engines in the future, e.g. SpiderMonkey, to provide more balanced testing, but there is no timetable for this.)
- Every JIT invocation requires redundant recompilation of the Halide runtime. (We hope to improve this once the LLVM Wasm backend can support dlopen() properly.)
- Wasm effectively runs in a private, 32-bit memory address space; while the host has access to that entire space, the reverse is not true, and thus any `define_extern` calls require copying all `halide_buffer_t` data across the Wasm<->host boundary in both directions.
- Host functions used via `define_extern` or `HalideExtern` cannot accept or return values that are pointer types or 64-bit integer types; this includes things like `const char *` and `user_context`. Fixing this is tractable, it was just omitted for now as the fix is nontrivial and the tests that are affected are mostly non-critical. (Note that `halide_buffer_t*` is explicitly supported as a special case, however.)
- Threading isn't supported at all (yet); all `parallel()` schedules will be run serially.
- The `.async()` directive isn't supported at all, not even in serial-emulation mode.
- You can't use `Param<void *>` (or any other arbitrary pointer type) with the Wasm jit.
- You can't use `Func.debug_to_file()`, `Func.set_custom_do_par_for()`, `Func.set_custom_do_task()`, or `Func.set_custom_allocator()`.
- The implementation of `malloc()` used by the JIT is incredibly basic and unsuitable for anything other than the most basic of tests.
- GPU usage (or any buffer usage that isn't 100% host-memory) isn't supported at all yet. (This should be doable, just omitted for now.)

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

- Install libv8 and the d8 shell tool (instructions omitted), or build from source if you prefer (instructions omitted). Note that using shared-library builds of V8 may be problematic on some platforms (e.g. OSX) due to libc++ conflict issues; using a static-library version may be simpler for those.

- Set V8_INCLUDE_PATH and V8_LIB_PATH to point to the paths for V8 include files and library, respectively.

- Set WITH_V8=1

- To run the JIT tests, set `HL_JIT_TARGET=wasm-32-wasmrt` and run normally. The test suites which we have vetted to work include correctness, performance, error, and warning. (Some of the others could likely be made to work with modest effort.)

## Enabling wasm AOT

If you want to test ahead-of-time code generation (and you almost certainly will), you need to install Emscripten and a shell for running wasm+js code (e.g., d8, part of v8)

- The simplest way to install is probably via the Emscripten emsdk (https://emscripten.org/docs/getting_started/downloads.html).

- After installing Emscripten, be sure that it is configured to use the version of LLVM that you configured earlier, rather than its built-in version; if you installed via `emsdk`, you need to edit `~/.emscripten` and set `LLVM_ROOT` point at the LLVM you have built. (If you fail with errors like `WASM_BACKEND selected but could not find lld (wasm-ld)`, you forgot to do this step)

- Set WASM_SHELL=/path/to/d8

- To run the AOT tests, set `HL_TARGET=wasm-32-wasmrt` and build the `test_aotwasm_generator` target. (Note that the normal AOT tests won't run usefully with this target, as extra logic to run under a wasm-enabled shell is required, and some tests are blacklisted.)

# Known Limitations And Caveats
- We have only tested with EMCC_WASM_BACKEND=1; using the fastcomp backend can probably be made to work but we haven't attempted to do so.
- Using the JIT requires that we link the `wasm-ld` tool into libHalide; with some work this need could (and should) probably be eliminated.
- CMake support hasn't been investigated yet, but should be straightforward.
- OSX and Linux-x64 have been tested. Windows hasn't; it should be supportable with some work. (Patches welcome.)
- The entire apps/ folder has not investigated yet. Many of them should be supportable with some work.
- We currently use d8 as a test environment for AOT code; we should probably use headless Chrome instead (this is probably required to allow for using threads in AOT code)


# Known TODO:

- There's some invasive hackiness in Codgen_LLVM to support the JIT trampolines; this really should be refactored to be less hacky.
- How close is SIMD to being ready? Need to work with the WebAssembly/V8 team to assess.
- Can we rework JIT to avoid the need to link in wasm-ld? This is probably doable, as the wasm object files produced by the LLVM backend are close enough to an executable form that we could likely make it work with some massaging on our side, but it's not clear whether this would be a bad idea or not (i.e., would it be unreasonably fragile).
- Improve the JIT to allow more of the tests to run; in particular, externs with 64-bit arguments (doable but omitted for expediency) and GPU support (ditto).
- Can we support threads in the JIT without an unreasonable amount of work? Unknown at this point.
- Someday, we should support alternate JIT/AOT test environments (e.g. SpiderMonkey/Firefox).


