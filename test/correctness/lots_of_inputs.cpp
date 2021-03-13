#include "Halide.h"

using namespace Halide;

int main(int argc, char **argv) {
    Var x, y;
    std::vector<ImageParam> inputs;
    Expr e = 0.0f;
    for (int i = 0; i < 1024; i++) {
        inputs.push_back(ImageParam(Float(32), 2));
        e += inputs.back()(x, y);
    }

    Func f;
    f(x, y) = e;

    // If we don't scale linearly in the number of inputs, this is
    // going to stall out.
    f.realize(1024, 1024);

    printf("Success!\n");

    return 0;

}
