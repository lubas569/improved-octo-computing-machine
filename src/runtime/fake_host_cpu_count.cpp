#include "HalideRuntime.h"

extern "C" {

WEAK int halide_host_cpu_count() {
    return 1;
}

}
