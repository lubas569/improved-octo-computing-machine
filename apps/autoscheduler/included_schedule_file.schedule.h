
#ifndef included_schedule_file_SCHEDULE_H
#define included_schedule_file_SCHEDULE_H

// MACHINE GENERATED -- DO NOT EDIT
// This schedule was automatically generated by apps/autoscheduler/AutoSchedule
// for target=x86-64-osx-avx-avx2-f16c-fma-sse41
// with machine_params=16,16777216,40

#include "Halide.h"


inline void apply_schedule_included_schedule_file(
    ::Halide::Pipeline pipeline,
    ::Halide::Target target
) {
    using ::Halide::Func;
    using ::Halide::MemoryType;
    using ::Halide::RVar;
    using ::Halide::TailStrategy;
    using ::Halide::Var;

    Func relu = pipeline.get_func(4);
    Func conv = pipeline.get_func(3);
    Var c(relu.get_schedule().dims()[0].var);
    Var ci("ci");
    Var n(relu.get_schedule().dims()[3].var);
    Var x(relu.get_schedule().dims()[1].var);
    Var xi("xi");
    Var y(relu.get_schedule().dims()[2].var);
    Var yi("yi");
    RVar r4_x(conv.update(0).get_schedule().dims()[0].var);
    RVar r4_y(conv.update(0).get_schedule().dims()[1].var);
    RVar r4_z(conv.update(0).get_schedule().dims()[2].var);
    relu
        .split(x, x, xi, 2, TailStrategy::ShiftInwards)
        .split(c, c, ci, 8, TailStrategy::ShiftInwards)
        .split(y, y, yi, 4, TailStrategy::ShiftInwards)
        .unroll(xi)
        .unroll(yi)
        .vectorize(ci)
        .compute_root()
        .reorder(ci, xi, yi, c, y, x, n)
        .fuse(x, n, x)
        .parallel(x);
    conv.update(0)
        .split(c, c, ci, 8, TailStrategy::GuardWithIf)
        .unroll(x)
        .unroll(y)
        .vectorize(ci)
        .reorder(ci, c, x, y, n, r4_x, r4_y, r4_z);
    conv
        .store_in(MemoryType::Stack)
        .split(c, c, ci, 8, TailStrategy::ShiftInwards)
        .unroll(x)
        .unroll(y)
        .vectorize(ci)
        .compute_at(relu, c)
        .reorder(ci, c, x, y, n);

}

#endif  // included_schedule_file_SCHEDULE_H
