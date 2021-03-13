#include <stdio.h>
#include "Halide.h"

using namespace Halide;

int main(int argc, char **argv) {

    Var x("x");
    Func f("f");

    Param<int> u;
    Param<int> u_name("u_name");

    if (u.is_explicit_name()) {
       printf("Expected autogenerated name.\n");
       return -1;
    }

    if (!u_name.is_explicit_name()) {
       printf("Expected explicit name.\n");
       return -1;
    }

    f(x) = u;

    Target target = get_jit_target_from_environment();
    if (target.has_gpu_feature()) {
        Var xo, xi;
        f.gpu_tile(x, xo, xi, 256);
    } else if (target.features_any_of({Target::HVX_64, Target::HVX_128})) {
        f.hexagon().vectorize(x, 32);
    }

    u.set(17);
    Buffer<int> out_17 = f.realize(1024, target);

    // Copied Params should still refer to the same underlying Parameter,
    // so setting the copy should be equivalent to setting the original.
    Param<int> u_alias = u;
    u_alias.set(123);
    Buffer<int> out_123 = f.realize(1024, target);

    for (int i = 0; i < 1024; i++) {
        if (out_17(i) != 17 || out_123(i) != 123) {
            printf("Failed!\n");
            for (int i = 0; i < 1024; i++) {
                printf("%d %d\n", out_17(i), out_123(i));
            }
            return -1;
        }
    }

    printf("Success!\n");
    return 0;
}
