#include "CodeGen_WebAssembly.h"
#include "Util.h"
#include "LLVM_Headers.h"

namespace Halide {
namespace Internal {

using std::vector;
using std::string;

using namespace llvm;

CodeGen_WebAssembly::CodeGen_WebAssembly(Target t) : CodeGen_Posix(t) {
    #if !(WITH_WEBASSEMBLY)
    user_error << "llvm build not configured with WebAssembly target enabled.\n";
    #endif
    user_assert(llvm_WebAssembly_enabled) << "llvm build not configured with WebAssembly target enabled.\n";
    user_assert(target.bits == 32) << "Only wasm32 is supported.";
}

string CodeGen_WebAssembly::mcpu() const {
    return "";
}

string CodeGen_WebAssembly::mattrs() const {
    // We believe support for this is wide enough as of early 2019
    // to simply enable it by default, rather than hide it behind a Feature
    string s = "+sign-ext";

    // TODO: not ready to enable by default
    // s += ",+bulk-memory";

    if (target.has_feature(Target::WasmSimd128)) {
        s += ",+simd128";
        user_warning << "Wasm simd128 isn't quite ready yet";
    }

    if (target.has_feature(Target::WasmThreads)) {
        s += ",+atomics";
    }

    user_assert(target.os == Target::WebAssemblyRuntime)
        << "wasmrt is the only supported 'os' for WebAssembly at this time.";

    // TODO: Emscripten doesn't seem to be able to validate wasm that contains this yet,
    // so only generate for JIT mode, where we know we can enable it.
    if (target.has_feature(Target::JIT)) {
        s += ",+nontrapping-fptoint";
    }

    return s;
}

bool CodeGen_WebAssembly::use_soft_float_abi() const {
    return false;
}

int CodeGen_WebAssembly::native_vector_bits() const {
    return 128;
}

}}
