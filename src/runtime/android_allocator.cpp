#include "HalideRuntime.h"
#include "runtime_internal.h"

extern "C" {

extern void *memalign(size_t alignment, size_t size);
extern void free(void *);

WEAK void *halide_default_malloc(void *user_context, size_t x) {
    // Android doesn't provide posix_memalign().
    const size_t alignment = halide_malloc_alignment();
    return memalign(alignment, x);
}

WEAK void halide_default_free(void *user_context, void *ptr) {
    free(ptr);
}

}

namespace Halide { namespace Runtime { namespace Internal {

WEAK halide_malloc_t custom_malloc = halide_default_malloc;
WEAK halide_free_t custom_free = halide_default_free;

}}} // namespace Halide::Runtime::Internal

extern "C" {

WEAK halide_malloc_t halide_set_custom_malloc(halide_malloc_t user_malloc) {
    halide_malloc_t result = custom_malloc;
    custom_malloc = user_malloc;
    return result;
}

WEAK halide_free_t halide_set_custom_free(halide_free_t user_free) {
    halide_free_t result = custom_free;
    custom_free = user_free;
    return result;
}

WEAK void *halide_malloc(void *user_context, size_t x) {
    return custom_malloc(user_context, x);
}

WEAK void halide_free(void *user_context, void *ptr) {
    custom_free(user_context, ptr);
}

}
