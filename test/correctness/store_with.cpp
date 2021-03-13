#include "Halide.h"

using namespace Halide;

int main(int argc, char **argv) {

    if (1) {
        // Perform a pointwise operation in-place.
        Func f, g;
        Var x;
        f(x) = x;
        g(x) = f(x) + 3;
        f.compute_root().store_with(g);
        // Order doesn't matter for pointwise in-place ops, so use
        // parallelism. Recompute would be bad though, so we must
        // round up.
        g.vectorize(x, 8, TailStrategy::RoundUp).parallel(x);
        f.vectorize(x, 4, TailStrategy::RoundUp).parallel(x);
        Buffer<int> buf = g.realize(128);

        for (int i = 0; i < 100; i++) {
            int correct = i + 3;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // A scan done directly within the output buffer to elide a copy.
        Func f, g;
        Var x, y;

        f(x, y) = x + y;
        RDom r(0, 99);
        f(r + 1, y) += f(r, y);
        f(98 - r, y) += f(99 - r, y);
        g(x, y) = f(x, y);

        g.unroll(y, 5, TailStrategy::RoundUp);

        f.compute_at(g, y).store_with(g);

        Buffer<int> buf = g.realize(100, 100);

        for (int y = 0; y < 1; y++) {
            int correct[100];
            for (int x = 0; x < 100; x++) {
                correct[x] = x + y;
            }
            for (int x = 0; x < 99; x++) {
                correct[x + 1] += correct[x];
            }
            for (int x = 0; x < 99; x++) {
                correct[98 - x] += correct[99 - x];
            }

            for (int x = 0; x < 100; x++) {
                if (buf(x, y) != correct[x]) {
                    printf("%d: buf(%d, %d) = %d instead of %d\n", __LINE__, x, y, buf(x, y), correct[x]);
                }
            }
        }
    }

    if (1) {
        // Move an array one vector to the left, in-place
        Func f, g, h;
        Var x;

        f(x) = x;
        g(x) = f(x+8);
        h(x) = g(x);

        f.compute_at(g, x).vectorize(x, 8, TailStrategy::GuardWithIf);

        f.store_with(g);
        g.compute_root();
        h.compute_root();

        Buffer<int> buf = h.realize(100);

        for (int i = 0; i < 100; i++) {
            int correct = i + 8;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // Zero-copy concat by having the two args write directly into
        // the destination buffer.
        Func f, g, h;
        Var x, y;

        f(x) = 18701;
        g(x) = 345;
        h(x) = select(x < 100, f(x), g(x - 100));

        f.compute_root().store_with(h);
        g.compute_root().store_with(h, {x + 100});
        h.bound(x, 0, 200);
        Buffer<int> buf = h.realize(200);

        for (int i = 0; i < 200; i++) {
            int correct = i < 100 ? 18701 : 345;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // In-place convolution. Shift the producer over a little to
        // avoid being clobbered by the consumer. This would write out
        // of bounds, so g can't be the output.
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x-1) + f(x) + f(x+1);
        h(x) = g(x);
        // If f is compute_root, then the realization of f is not
        // within the realization of g, so it's actually an
        // error. Need to add error checking, or place the realization
        // somewhere that includes both. Right now it just produced a
        // missing symbol error.
        f.compute_at(g, Var::outermost()).store_with(g, {x+1});
        g.compute_root();
        Buffer<int> buf = h.realize(100);
        for (int i = 0; i < 100; i++) {
            int correct = 3 * i;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // 2D in-place convolution.
        Func f, g, h;
        Var x, y;
        f(x, y) = x + y;
        g(x, y) = f(x-1, y-1) + f(x+1, y+1);
        h(x, y) = g(x, y);

        g.compute_root();
        // Computation of f must be nested inside computation of g
        f.compute_at(g, Var::outermost()).store_with(g, {x+1, y+1});
        Buffer<int> buf = h.realize(100, 100);

        for (int y = 0; y < 100; y++) {
            for (int x = 0; x < 100; x++) {
                int correct = 2*(x + y);
                if (buf(x, y) != correct) {
                    printf("%d: buf(%d, %d) = %d instead of %d\n", __LINE__, x, y, buf(x, y), correct);
                }
            }
        }
    }

    if (1) {
        // 2D in-place convolution computed per scanline
        Func f, g, h;
        Var x, y;
        f(x, y) = x + y;
        g(x, y) = f(x-1, y-1) + f(x+1, y+1);
        h(x, y) = g(x, y);

        g.compute_root();
        // Store slices of f two scanlines down in the as-yet-unused region of g
        f.compute_at(g, y).store_with(g, {x, y+2});
        Buffer<int> buf = h.realize(100, 100);

        for (int y = 0; y < 100; y++) {
            for (int x = 0; x < 100; x++) {
                int correct = 2*(x + y);
                if (buf(x, y) != correct) {
                    printf("%d: buf(%d, %d) = %d instead of %d\n", __LINE__, x, y, buf(x, y), correct);
                }
            }
        }
    }

    if (1) {
        // 2D in-place convolution computed per scanline with sliding
        Func f, g, h;
        Var x, y;
        f(x, y) = x + y;
        g(x, y) = f(x-1, y-1) + f(x+1, y+1);
        h(x, y) = g(x, y);

        g.compute_root();
        f.store_root().compute_at(g, y).store_with(g, {x, y+3});
        h.realize(100, 100);
    }

    if (1) {
        // split then merge
        Func f, g, h, out;
        Var x;
        f(x) = x;
        g(x) = f(2*x) + 1;
        h(x) = f(2*x+1) * 2;
        out(x) = select(x % 2 == 0, g(x/2), h(x/2));

        f.compute_root().store_with(out);
        g.compute_root().store_with(out, {2*x}); // Store g at the even spots in out
        h.compute_root().store_with(out, {2*x+1});  // Store h in the odd spots

        Buffer<int> buf = out.realize(100);

        for (int i = 0; i < 100; i++) {
            int correct = (i & 1) ? (i * 2) : (i + 1);
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // split then merge, with parallelism
        Func f, g, h, out;
        Var x;
        f(x) = x;
        g(x) = f(2*x) + 1;
        h(x) = f(2*x+1) * 2;
        out(x) = select(x % 2 == 0, g(x/2), h(x/2));

        f.compute_root().vectorize(x, 8).store_with(out);
        // Store g at the even spots in out
        g.compute_root().vectorize(x, 8, TailStrategy::RoundUp).store_with(out, {2*x});
        // Store h in the odd spots
        h.compute_root().vectorize(x, 8, TailStrategy::RoundUp).store_with(out, {2*x+1});
        out.vectorize(x, 8, TailStrategy::RoundUp);

        Buffer<int> buf = out.realize(128);

        for (int i = 0; i < 100; i++) {
            int correct = (i & 1) ? (i * 2) : (i + 1);
            if (buf(i) != correct) {
                printf("buf(%d) = %d instead of %d\n", i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // A double integration in-place
        Func f, g, h;
        Var x;
        f(x) = x;
        RDom r(1, 99);
        g(x) = f(x);
        g(r) += g(r-1);
        h(x) = g(x);
        h(r) += h(r-1);

        f.compute_root().store_with(h);
        g.compute_root().store_with(h);
        h.bound(x, 0, 100);
        Buffer<int> buf = h.realize(100);

        for (int i = 0; i < 100; i++) {
            int correct = (i * (i + 1) * (i + 2)) / 6;
            if (buf(i) != correct) {
                printf("buf(%d) = %d instead of %d\n", i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // Something that only works because vector loop iterations
        // occur simultaneously, so stores from one lane definitely
        // aren't visible to others absent some other sequence point.
        Func f, g;
        Var x;
        f(x) = x;
        g(x) = f(31 - x);
        f.compute_root().store_with(g);
        g.bound(x, 0, 32).vectorize(x);
        Buffer<int> buf = g.realize(32);

        for (int i = 0; i < 32; i++) {
            int correct = 31 - i;
            if (buf(i) != correct) {
                printf("buf(%d) = %d instead of %d\n", i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // A tiled pyramid
        Func f, g, h;
        Var x, y;

        f(x, y) = x + y;

        g(x, y) = f(x/2, y/2) + 1;
        h(x, y) = g(x/2, y/2) + 2;

        // Store a 4x4 block of f densely in the top left of every 16x16 tile of h
        f.compute_at(h, Var::outermost())
            .store_with(h, {16*(x/4) + x%4, 16*(y/4) + y%4})
            .vectorize(x).unroll(y);

        // Store an 8x8 block of g similarly compacted in the bottom
        // right. It doesn't collide with f, and we're OK to overwrite
        // it when computing h because we compute h serially across y
        // and vectorized across x.
        g.compute_at(h, Var::outermost())
            .store_with(h, {16*(x/8) + x%8 + 8, 16*(y/8) + y%8 + 8})
            .vectorize(x).unroll(y);

        Var xi, yi;
        h.compute_at(h.in(), x).vectorize(x).unroll(y);
        h = h.in();
        h.align_bounds(x, 16).align_bounds(y, 16)
            .tile(x, y, xi, yi, 16, 16)
            .vectorize(xi).unroll(yi);

        Buffer<int> buf = h.realize(128, 128);

        for (int y = 0; y < 128; y++) {
            for (int x = 0; x < 128; x++) {
                int correct = x/4 + y/4 + 3;
                if (buf(x, y) != correct) {
                    printf("%d: buf(%d, %d) = %d instead of %d\n", __LINE__, x, y, buf(x, y), correct);
                    return -1;
                }
            }
        }
    }

    if (1) {
        // We can place the storage outside a parallel loop provided that there are no race conditions.
        Func f1, f2, g, h;
        Var x;
        RDom r(0, 100);
        f1(x) = x;
        f1(x) += r;
        f2(x) = x;
        // No race conditions on f2 because it's a race between atomic
        // stores of the same value. No race conditions on f1 because
        // distinct threads write to distinct sites.
        g(x) = f2(x - 1) + f2(x + 1) + f1(x);
        h(x) = g(x);

        Var xo, xi;
        g.compute_root().split(x, xo, xi, 16, TailStrategy::RoundUp).parallel(xo);
        f1.compute_at(g, xo).store_with(g, {x + 256});
        f2.compute_at(g, xo).store_with(g, {x + 512});
        h.bound(x, 0, 128);
        Buffer<int> buf = h.realize(128);

        for (int i = 0; i < 128; i++) {
            int correct = 4950 + i*3;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // Test mixing types and tuples (while preserving bit-widths)
        Func f, g, h;
        Var x;

        f(x) = {cast<uint8_t>(x), cast<float>(x)};
        g(x) = {cast<int8_t>(x), cast<uint32_t>(f(x)[0] + f(x)[1])};

        f.compute_root().store_with(g);

        Buffer<int8_t> b1(128);
        Buffer<uint32_t> b2(128);
        g.realize({b1, b2});

        // All of the types involved can store the numbers involved exactly.
        for (int i = 0; i < 128; i++) {
            int actual1 = (int)b1(i);
            int actual2 = (int)b2(i);
            int correct1 = i;
            int correct2 = 2*i;
            if (correct1 != actual1 || correct2 != actual2) {
                printf("%d: buf(%d) = {%d, %d} instead of {%d, %d}\n",
                       __LINE__, i, actual1, actual2, correct1, correct2);
                return -1;
            }
        }

    }

    if (1) {
        // Async is OK if we're entirely inside it, or if we prove we
        // can't clobber regardless of timing. First we test the case
        // where we're nested inside another async thing:
        Func f1, f2, g, h;
        Var x;
        f1(x) = x;
        f2(x) = f1(x);
        g(x) = f2(x) + 3;
        h(x) = g(x) + 8;
        f1.compute_at(f2, Var::outermost()).store_with(f2);
        f2.compute_at(g, Var::outermost());
        g.compute_root().async();
        Buffer<int> buf = h.realize(128);

        for (int i = 0; i < 128; i++) {
            int correct = i + 11;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // Then the case where we're stored inside one fork of the
        // async but computed inside another, but there can't possible
        // be a clobber anyway based on addresses alone, so ordering
        // doesn't matter.
        Func f1, f2, f3, g;
        Var x;
        f1(x) = x;
        f2(x) = 3*x;
        f3(x) = f1(x);
        g(x) = f2(x % 8) + f3(x % 8 + 8);

        f1.compute_at(f3, x).store_with(f2);
        f3.compute_at(g, Var::outermost()).async();
        f2.store_root().compute_at(g, Var::outermost());
        Buffer<int> buf = g.realize(128);

        for (int i = 0; i < 128; i++) {
            int correct = (i % 8)*3 + (i % 8) + 8;;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // store_with can be used for zero-copy reshape operations
        Func f, g, h;
        Var x, y;
        f(x) = x;
        g(x, y) = f(x + 4*y);
        h(x) = g(x%4, x/4);

        f.compute_root().store_with(h);
        g.bound(x, 0, 4).compute_root().store_with(h, {x + 4*y});
        Buffer<int> buf = h.realize(128);

        for (int i = 0; i < 128; i++) {
            int correct = i;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (1) {
        // store_with + compute_with to get a single loop that writes
        // an AoS layout. Assume f and g must be compute_root due to
        // some other constraint, or we could just inline them.

        Func f, g, h;
        Var x, i;
        f(x) = x + 3;
        g(x) = x * 17;
        h(i, x) = select(i == 0, f(x), g(x));
        f.compute_root().store_with(h, {0, x});
        g.compute_root().compute_with(f, {x}).store_with(h, {1, x});

        Buffer<int> buf = h.bound(i, 0, 2).realize(2, 128);

        for (int i = 0; i < 2; i++) {
            for (int x = 0; x < 128; x++) {
                int correct = i == 0 ? (x + 3) : (x * 17);
                if (buf(i, x) != correct) {
                    printf("%d: buf(%d, %d) = %d instead of %d\n", __LINE__, i, x, buf(i, x), correct);
                    return -1;
                }
            }
        }
    }

    if (1) {
        // store_with + storage folding on the destination buffer

        // We're going to jam two stencil footprints into the same
        // circular buffer, rotating around each other. The combined
        // buffer is one smaller (5) than the sum of the footprints
        // (3+3), because we can do the leading edge as an in-place
        // += operation.

        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x+1) + f(x-1);
        h(x) = g(x+1) + g(x-1);

        f.store_root().compute_at(h, x);
        g.store_root().compute_at(h, x);
        f.store_with(g, {x+1});
        g.fold_storage(x, 5);

        Buffer<int> buf = h.realize(128);

        for (int i = 0; i < 128; i++) {
            int correct = 4*i;
            if (buf(i) != correct) {
                printf("%d: buf(%d) = %d instead of %d\n", __LINE__, i, buf(i), correct);
                return -1;
            }
        }
    }

    if (get_jit_target_from_environment().has_gpu_feature()) {
        // Store two GPU buffers together. Mostly this is to test
        // that device copies happen sanely for store_with
        // buffers. All of the GPU logic happens after store_with
        // is lowered, so there's no particular reason to expect
        // it to be strange.
        Func f, g;
        Var x, y;

        f(x, y) = x + y;
        f(x, y) += 5;
        g(x, y) = f(x, y) + 6;
        g(x, y) += 7;

        // Place every other stage on the gpu
        Var xi, yi;
        f.compute_root().store_with(g).update().gpu_tile(x, y, xi, yi, 8, 8);
        g.update().gpu_tile(x, y, xi, yi, 8, 8);

        // We expect g to be copied to the host (in case the
        // output buffer is dirty on device), written on the host
        // for f's pure definition, then copied to gpu for f's
        // update definition, then copied back to host for g's
        // pure definition, then copied back to the device again
        // for g's update. If any of these copies don't happen
        // we'll get incorrect outputs.

        Buffer<int> buf = g.realize(128, 128);

        for (int y = 0; y < 128; y++) {
            for (int x = 0; x < 128; x++) {
                int correct = x + y + 5 + 6 + 7;
                if (buf(x, y) != correct) {
                    printf("%d: buf(%d, %d) = %d instead of %d\n", __LINE__, x, y, buf(x, y), correct);
                    return -1;
                }
            }
        }
    }

    if (1) {
        // Avoid all allocations inside a pipeline by passing in a
        // scratch buffer as a dummy output and setting up the layout
        // of the intermediates within it explicitly using
        // store_with. Bit of a hack, but lets you preallocate all
        // storage everything without having to make everything an
        // output (which would force them to be compute_root and not
        // let them reuse the same memory). Maybe also a useful
        // technique for preserving intermediates for debugging
        // without having to reschedule? The downside is that you're
        // required to provide enough storage to back all the
        // intermediate Funcs as if they were compute_root.

        Func f, g, h;
        Var x;

        f(x) = x;
        g(x) = f(x-1) + f(x+1);
        h(x) = g(x-1) + g(x+1);

        Var xi;
        h.split(x, x, xi, 8);
        Func scratch;
        scratch(x) = undef<int>();
        f.compute_at(h, x).store_with(scratch);
        // The split means that f's footprint might get shifted off
        // the left edge of its minimum required realization, so we
        // need to place f further away in memory from g than you
        // might think for this to be correct for all possible
        // output sizes.
        g.compute_at(h, x).store_with(scratch, {x + h.output_buffer().dim(0).extent() + 10});

        // Pick a size for h
        Buffer<int> h_buf(128);

        // We'll do an output bounds query to size the scratch buffer
        // as needed for the given size of h.
        Buffer<int> scratch_buf(nullptr, 0);

        Pipeline p({h, scratch});
        p.realize({h_buf, scratch_buf});

        int correct_scratch_size = 2*h_buf.dim(0).extent() + 13;
        if (scratch_buf.data() != nullptr ||
            scratch_buf.dim(0).extent() != correct_scratch_size) {
            printf("Scratch buf was supposed to be unallocated and of size %d. "
                   "Instead it has host pointer %p and is of size %d\n",
                   correct_scratch_size, (void *)scratch_buf.data(), scratch_buf.dim(0).extent());
            return -1;
        }

        // Preallocate the space needed for the pipeline intermediates.
        scratch_buf.allocate();

        // Run the pipeline.
        p.realize({h_buf, scratch_buf});

        for (int i = 0; i < 128; i++) {
            int correct = 4*i;
            if (h_buf(i) != correct) {
                printf("%d: h_buf(%d) = %d instead of %d\n", __LINE__, i, h_buf(i), correct);
                return -1;
            }
        }
    }

    // TODO: desirable extensions to store with:
    // - accommodate type or tuple dimensionality mismatches by adding new inner dimensions (e.g. widening downsamples in-place)
    // - the ability to store_with input buffers to express entire in-place pipelines
    // - the ability to store something in the unused bits of something else when we know Func value bounds

#ifdef WITH_EXCEPTIONS

#define ASSERT_UNREACHABLE do {printf("There was supposed to be an error before line %d\n", __LINE__); return -1;} while (0)

    const bool verbose = false;

    try {
        // Can't do in-place with shiftinwards tail strategies.
        Func f, g;
        Var x;
        f(x) = x;
        g(x) = f(x) + 3;
        f.compute_root().store_with(g);
        g.vectorize(x, 8, TailStrategy::ShiftInwards);
        g.compile_jit();
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't store_with the output in cases where it would grow the bounds of the output.
        Func f, g;
        Var x;
        f(x) = x;
        g(x) = f(x) + f(x+100);
        f.compute_root().store_with(g);
        g.realize(100);
        ASSERT_UNREACHABLE;
    } catch (RuntimeError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Don't clobber values we'll need later
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x-1) + f(x) + f(x+1);
        h(x) = g(x);
        f.compute_at(g, Var::outermost()).store_with(g);
        g.compute_root();
        h.compile_jit();
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't store multiple values at the same site
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x-1) + f(x) + f(x+1);
        h(x) = g(x);
        f.compute_at(g, Var::outermost()).store_with(g, {x/2 + 1000});
        g.compute_root().bound(x, 0, 100);
        h.compile_jit();
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't create race conditions by storing with something
        // outside a parallel loop and computing inside it.
        Func f, g, h;
        Var x;
        RDom r(0, 100);
        f(x) = x;
        f(x) += r;
        g(x) = f(x - 1) + f(x + 1);
        h(x) = g(x);

        Var xo, xi;
        g.compute_root().split(x, xo, xi, 16, TailStrategy::RoundUp).parallel(xo);
        f.compute_at(g, xo).store_with(g, {x + 256});
        h.bound(x, 0, 128);
        h.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Redundant recompute on the same memory is problematic even
        // without parallelism, if there are read-modify-writes.
        Func f, g, h;
        Var x;
        f(x) = x;
        RDom r(0, 256);
        f(r) += 1;
        g(x) = f(x);

        Var xo, xi;
        g.compute_root().split(x, xo, xi, 16, TailStrategy::RoundUp);
        f.compute_at(g, xo).store_with(g, {x + 256});
        g.bound(x, 0, 256);
        g.realize(256);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't store_with inline things
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x);
        h(x) = g(x);

        f.compute_root().store_with(g); // g is inlined!
        h.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // No transitive nonsense
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x);
        h(x) = g(x);

        f.compute_root().store_with(g);
        g.compute_root().store_with(h);
        h.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // No storing with things not in the pipeline
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x);
        h(x) = f(x);

        f.compute_root().store_with(g);
        g.compute_root();
        // h has no dependence on g, so even though it's compute root,
        // it won't have a realization.
        h.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't currently mix tuple widths
        Func f, g, h;
        Var x;

        f(x) = {cast<uint8_t>(x), cast<float>(x)};
        g(x) = cast<uint32_t>(f(x)[0] + f(x)[1]);
        f.compute_root().store_with(g);
        g.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't currently mix bit widths
        Func f, g, h;
        Var x;

        f(x) = x;
        g(x) = cast<int64_t>(f(x));
        f.compute_root().store_with(g);
        g.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Dimensionality of placement site must match dimensionality of target Func
        Func f, g;
        Var x;

        f(x) = x;
        g(x) = f(x);
        f.compute_root().store_with(g, {x, 4});
        g.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // The meaning of async depends on the storage scope of a
        // buffer, but a store_with Func doesn't really have one, so
        // you can't apply store_with and async to the same Func.
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x) + f(x+1);
        h(x) = g(x);
        f.store_root().compute_at(g, x).store_with(g).async();
        g.compute_root();
        h.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // f1 is computed inside something async but stored with something outside the async
        Func f1, f2, g, h;
        Var x;
        f1(x) = x;
        f2(x) = f1(x);
        g(x) = f2(x) + f2(x+1);
        h(x) = g(x);
        f1.store_at(g, Var::outermost()).compute_at(f2, Var::outermost()).store_with(g);
        f2.store_at(g, Var::outermost()).compute_at(g, x).async();
        g.compute_root();
        h.realize(128);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't fold an allocation that doesn't exist.
        Func f, g, h;
        Var x;
        f(x) = x;
        g(x) = f(x+1) + f(x-1);
        h(x) = g(0) + g(100);
        g.compute_root();
        // Use folded storage for f and place the ring buffer just
        // after g in the same allocation. Doesn't work, because
        // fold_storage doesn't just change coordinates, it also
        // changes the allocation.
        f.store_at(g, Var::outermost()).compute_at(g, x).store_with(g, {x + 101}).fold_storage(x, 4);
        h.realize(1);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    try {
        // Can't align an allocation that doesn't exist.
        Func f, g;
        Var x;
        f(x) = x;
        g(x) = f(x);
        g.compute_root();
        // Use folded storage for f and place the ring buffer just
        // after g in the same allocation. Doesn't work, because
        // fold_storage doesn't just change coordinates, it also
        // changes the allocation.
        f.compute_at(g, Var::outermost()).align_storage(x, 8).store_with(g);
        g.realize(100);
        ASSERT_UNREACHABLE;
    } catch (CompileError &e) {
        if (verbose) std::cerr << e.what() << "\n";
    }

    {
        // Forbid memoizing anything that shares storage with another
        // Func. Theoretically we could make it work if they're both
        // memoized and we concat the keys.

        try {
            // Memoized source.
            Func f, g;
            Var x;
            f(x) = x;
            g(x) = f(x);
            g.compute_root();
            f.compute_at(g, Var::outermost()).store_with(g).memoize();
            g.realize(100);
            ASSERT_UNREACHABLE;
        } catch (CompileError &e) {
            if (verbose) std::cerr << e.what() << "\n";
        }

        try {
            // Memoized destination.
            Func f, g;
            Var x;
            f(x) = x;
            g(x) = f(x);
            g.compute_root().memoize();
            f.compute_at(g, Var::outermost()).store_with(g);
            g.realize(100);
            ASSERT_UNREACHABLE;
        } catch (CompileError &e) {
            if (verbose) std::cerr << e.what() << "\n";
        }
    }

#else
    printf("Not testing store_with failure cases because Halide was compiled without exceptions\n");
    return 0;
#endif

    printf("Success!\n");

    return 0;
}
