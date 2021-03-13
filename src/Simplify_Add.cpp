#include "Simplify_Internal.h"

namespace Halide {
namespace Internal {

Expr Simplify::visit(const Add *op, ExprInfo *bounds) {
    ExprInfo a_bounds, b_bounds;
    Expr a = mutate(op->a, &a_bounds);
    Expr b = mutate(op->b, &b_bounds);

    if (bounds && no_overflow_int(op->type)) {
        bounds->min_defined = a_bounds.min_defined && b_bounds.min_defined;
        bounds->max_defined = a_bounds.max_defined && b_bounds.max_defined;
        bounds->min = a_bounds.min + b_bounds.min;
        bounds->max = a_bounds.max + b_bounds.max;
        bounds->alignment = a_bounds.alignment + b_bounds.alignment;
        bounds->trim_bounds_using_alignment();
    }

    if (may_simplify(op->type)) {

        // Order commutative operations by node type
        if (should_commute(a, b)) {
            std::swap(a, b);
            std::swap(a_bounds, b_bounds);
        }

        auto rewrite = IRMatcher::rewriter(IRMatcher::add(a, b), op->type);
        const int lanes = op->type.lanes();

        if (rewrite(c0 + c1, fold(c0 + c1)) ||
            rewrite(IRMatcher::Indeterminate() + x, a) ||
            rewrite(x + IRMatcher::Indeterminate(), b) ||
            rewrite(IRMatcher::Overflow() + x, a) ||
            rewrite(x + IRMatcher::Overflow(), b) ||
            rewrite(x + 0, x) ||
            rewrite(0 + x, x)) {
            return rewrite.result;
        }

        if (EVAL_IN_LAMBDA
            (rewrite(x + x, x * 2) ||
             rewrite(ramp(x, y) + ramp(z, w), ramp(x + z, y + w, lanes)) ||
             rewrite(ramp(x, y) + broadcast(z), ramp(x + z, y, lanes)) ||
             rewrite(broadcast(x) + broadcast(y), broadcast(x + y, lanes)) ||
             rewrite(select(x, y, z) + select(x, w, u), select(x, y + w, z + u)) ||
             rewrite(select(x, c0, c1) + c2, select(x, fold(c0 + c2), fold(c1 + c2))) ||
             //             rewrite(select(x, y, c1) + c2, select(x, y + c2, fold(c1 + c2))) ||
             rewrite(select(x, c0, y) + c2, select(x, fold(c0 + c2), y + c2)) ||

             rewrite((select(x, y, z) + w) + select(x, u, v), select(x, y + u, z + v) + w) ||
             rewrite((w + select(x, y, z)) + select(x, u, v), select(x, y + u, z + v) + w) ||
             rewrite(select(x, y, z) + (select(x, u, v) + w), select(x, y + u, z + v) + w) ||
             rewrite(select(x, y, z) + (w + select(x, u, v)), select(x, y + u, z + v) + w) ||
             rewrite((select(x, y, z) - w) + select(x, u, v), select(x, y + u, z + v) - w) ||
             rewrite(select(x, y, z) + (select(x, u, v) - w), select(x, y + u, z + v) - w) ||
             rewrite((w - select(x, y, z)) + select(x, u, v), select(x, u - y, v - z) + w) ||
             rewrite(select(x, y, z) + (w - select(x, u, v)), select(x, y - u, z - v) + w) ||

             rewrite((x + c0) + c1, x + fold(c0 + c1)) ||
             rewrite((x + c0) + y, (x + y) + c0) ||
             rewrite(x + (y + c0), (x + y) + c0) ||
             rewrite((c0 - x) + c1, fold(c0 + c1) - x) ||
             rewrite((c0 - x) + y, (y - x) + c0) ||
             rewrite((x - y) + y, x) ||
             rewrite(x + (y - x), y) ||
             rewrite(x + (c0 - y), (x - y) + c0) ||
             rewrite((x - y) + (y - z), x - z) ||
             rewrite((x - y) + (z - x), z - y) ||
             rewrite(x + y*c0, x - y*(-c0), c0 < 0 && -c0 > 0) ||
             rewrite(x*c0 + y, y - x*(-c0), c0 < 0 && -c0 > 0 && !is_const(y)) ||
             rewrite(x*y + z*y, (x + z)*y) ||
             rewrite(x*y + y*z, (x + z)*y) ||
             rewrite(y*x + z*y, y*(x + z)) ||
             rewrite(y*x + y*z, y*(x + z)) ||
             rewrite(x*c0 + y*c1, (x + y*fold(c1/c0)) * c0, c1 % c0 == 0) ||
             rewrite(x*c0 + y*c1, (x*fold(c0/c1) + y) * c1, c0 % c1 == 0) ||
             (no_overflow(op->type) &&
              (rewrite(x + x*y, x * (y + 1)) ||
               rewrite(x + y*x, (y + 1) * x) ||
               rewrite(x*y + x, x * (y + 1)) ||
               rewrite(y*x + x, (y + 1) * x, !is_const(x)) ||
               rewrite((x + c0)/c1 + c2, (x + fold(c0 + c1*c2))/c1) ||
               rewrite((x + (y + c0)/c1) + c2, x + (y + fold(c0 + c1*c2))/c1) ||
               rewrite(((y + c0)/c1 + x) + c2, x + (y + fold(c0 + c1*c2))/c1) ||
               rewrite((c0 - x)/c1 + c2, (fold(c0 + c1*c2) - x)/c1, c0 != 0 && c1 != 0) || // When c0 is zero, this would fight another rule
               rewrite(x + (x + y)/c0, (fold(c0 + 1)*x + y)/c0) ||
               rewrite(x + (y + x)/c0, (fold(c0 + 1)*x + y)/c0) ||
               rewrite(x + (y - x)/c0, (fold(c0 - 1)*x + y)/c0) ||
               rewrite(x + (x - y)/c0, (fold(c0 + 1)*x - y)/c0) ||
               rewrite((x - y)/c0 + x, (fold(c0 + 1)*x - y)/c0) ||
               rewrite((y - x)/c0 + x, (y + fold(c0 - 1)*x)/c0) ||
               rewrite((x + y)/c0 + x, (fold(c0 + 1)*x + y)/c0) ||
               rewrite((y + x)/c0 + x, (y + fold(c0 + 1)*x)/c0) ||
               rewrite(min(x, y - z) + z, min(x + z, y)) ||
               rewrite(min(y - z, x) + z, min(y, x + z)) ||
               rewrite(min(x, y + c0) + c1, min(x + c1, y), c0 + c1 == 0) ||
               rewrite(min(y + c0, x) + c1, min(y, x + c1), c0 + c1 == 0) ||
               rewrite(z + min(x, y - z), min(z + x, y)) ||
               rewrite(z + min(y - z, x), min(y, z + x)) ||
               rewrite(z + max(x, y - z), max(z + x, y)) ||
               rewrite(z + max(y - z, x), max(y, z + x)) ||
               rewrite(max(x, y - z) + z, max(x + z, y)) ||
               rewrite(max(y - z, x) + z, max(y, x + z)) ||
               rewrite(max(x, y + c0) + c1, max(x + c1, y), c0 + c1 == 0) ||
               rewrite(max(y + c0, x) + c1, max(y, x + c1), c0 + c1 == 0) ||
               rewrite(max(x, y) + min(x, y), x + y) ||
               rewrite(max(x, y) + min(y, x), x + y))) ||
             (no_overflow_int(op->type) &&
              (rewrite((x/y)*y + x%y, x) ||
               rewrite((z + x/y)*y + x%y, z*y + x) ||
               rewrite((x/y + z)*y + x%y, x + z*y) ||
               rewrite(x%y + ((x/y)*y + z), x + z) ||
               rewrite(x%y + ((x/y)*y - z), x - z) ||
               rewrite(x%y + (z + (x/y)*y), x + z) ||
               rewrite((x/y)*y + (x%y + z), x + z) ||
               rewrite((x/y)*y + (x%y - z), x - z) ||
               rewrite((x/y)*y + (z + x%y), x + z) ||
               rewrite(x/2 + x%2, (x + 1) / 2) ||

               rewrite(x + ((c0 - x)/c1)*c1, c0 - ((c0 - x) % c1), c1 > 0) ||
               rewrite(x + ((c0 - x)/c1 + y)*c1, y * c1 - ((c0 - x) % c1) + c0, c1 > 0) ||
               rewrite(x + (y + (c0 - x)/c1)*c1, y * c1 - ((c0 - x) % c1) + c0, c1 > 0) ||

               // Synthesized
               #if USE_SYNTHESIZED_RULES

               rewrite((((min(x, y)*(z + w)) + z) + w), ((min(x, y) + 1)*(w + z))) ||
               rewrite((((x + y)*z) + (w - (x*z))), ((y*z) + w)) ||
               rewrite((((x + y)*z) + (w - (y*z))), ((x*z) + w)) ||
               rewrite((((x - (y*c0))*c1) + ((y*c2) + z)), ((x*c1) + z), (c2 == (c0*c1))) ||
               rewrite((((x*y) + (z + (w*y))) + y), (z - (((-1 - x) - w)*y))) ||
               rewrite((((x*y)*z) + (w*y)), (((x*z) + w)*y)) ||
               rewrite((((x*y)*z) + (x*w)), (((y*z) + w)*x)) ||
               rewrite(((min((x + c0), y) + z) + c1), (min((y + c1), x) + z), ((c0 + c1) == 0)) ||
               rewrite(((min((x + c0), y)*c1) + c2), (min((y + fold((0 - c0))), x)*c1), (((c0*c1) + c2) == 0)) ||
               rewrite(((min((x - (y + z)), w) + y) + z), min(((y + z) + w), x)) ||
               rewrite(((min((x - (y + z)), w) + z) + u), (min((x - y), (z + w)) + u)) ||
               rewrite(((min((x - y), z) + (w + y)) + u), (min((y + z), x) + (w + u))) ||
               rewrite(((min((x - y), z) + (y + w)) + u), (min((y + z), x) + (w + u))) ||
               rewrite(((min((x - y), z) + w) + y), (min((y + z), x) + w)) ||
               rewrite(((min(min(x, (y + c1)), c1) + z) + c2), (min(min((x + c2), y), 0) + z), ((c1 + c2) == 0)) ||
               rewrite(((min(x, (y + c0)) + z) + c1), (min((x + c1), y) + z), ((c0 + c1) == 0)) ||
               rewrite(((min(x, (y + c0))*c1) + c2), (min((x + fold((0 - c0))), y)*c1), (((c0*c1) + c2) == 0)) ||
               rewrite(((x - (min((y + z), w) + u)) + z), ((x - u) - min((w - z), y))) ||
               rewrite(((x - (y + z)) + (w + z)), ((w - y) + x)) ||
               rewrite(((x - (y + z)) + z), (x - y)) ||
               rewrite(((x - (y*z)) + (w - (u*z))), ((w - ((u + y)*z)) + x)) ||
               rewrite(((x - max(y, ((z + w) + u))) + u), (x - max((y - u), (w + z)))) ||
               rewrite(((x - min((y + z), w)) + z), (x - min((w - z), y))) ||
               rewrite(((x - y) + (y + z)), (x + z)) ||
               rewrite(((x - y) + (z + y)), (x + z)) ||
               rewrite(((x*(y*z)) + (w*z)), (((x*y) + w)*z)) ||
               rewrite(((x*(y*z)) + (y*w)), (((x*z) + w)*y)) ||
               rewrite(((x*y) + ((y*z) + w)), (((x + z)*y) + w)) ||
               rewrite(((x*y) + ((z*y) + w)), (((x + z)*y) + w)) ||
               rewrite(((x*y) + ((z*y) - w)), (((x + z)*y) - w)) ||
               rewrite(((x*y) + (z - ((w + u)*y))), (z - (((u - x) + w)*y))) ||
               rewrite(((x*y) + (z - (y*w))), (((x - w)*y) + z)) ||
               rewrite((max(x, y) + (min(x, y) + z)), ((x + y) + z)) ||
               rewrite((min(((x - y) - z), w) + y), min((x - z), (w + y))) ||
               rewrite((min(((x - y)*z), w) + (y*z)), min((x*z), ((y*z) + w))) ||
               rewrite((min((x - (y + z)), c0) + (w + z)), (min((x - y), (z + c0)) + w)) ||
               rewrite((min((x - (y + z)), w) + (z + u)), (min((x - y), (z + w)) + u)) ||
               rewrite((min((x - (y + z)), w) + y), min((x - z), (y + w))) ||
               rewrite((min((x - (y + z)), w) + z), min((x - y), (z + w))) ||
               rewrite((min((x - y), z) + (w + y)), (min((y + z), x) + w)) ||
               rewrite((min((x - y), z) + (y + w)), (min((y + z), x) + w)) ||
               rewrite((min(min((x + c0), y), z) + c1), min((min(y, z) + c1), x), ((c0 + c1) == 0)) ||
               rewrite((min(min((x - y), z), w) + y), min((min(z, w) + y), x)) ||
               rewrite((min(min(x, ((y - z) + w)), u) + z), min((min(x, u) + z), (y + w))) ||
               rewrite((min(min(x, (y + c0)), z) + c1), min((min(x, z) + c1), y), ((c0 + c1) == 0)) ||
               rewrite((min(min(x, (y - z)), w) + z), min((min(x, w) + z), y)) ||
               rewrite((min(x, ((y - z) + w)) + z), min((y + w), (x + z))) ||
               rewrite((min(x, (y - z)) + (z + w)), (min((x + z), y) + w)) ||
               rewrite((min(x, y) + (max(min(x, z), y) + w)), (min(max(y, z), x) + (y + w))) ||
               rewrite((min(x, y) + min((min(x, y) + z), w)), (min((min(x, y) + z), w) + min(x, y))) ||
               rewrite((x + ((y*z) + (w + (u*z)))), (((u + y)*z) + (w + x))) ||
               rewrite((x + (y - (x + z))), (y - z)) ||
               rewrite((x + (y - (z + x))), (y - z)) ||

               #endif

               false)))) {
            return mutate(std::move(rewrite.result), bounds);
        }

        const Shuffle *shuffle_a = a.as<Shuffle>();
        const Shuffle *shuffle_b = b.as<Shuffle>();
        if (shuffle_a && shuffle_b &&
            shuffle_a->is_slice() &&
            shuffle_b->is_slice()) {
            if (a.same_as(op->a) && b.same_as(op->b)) {
                return hoist_slice_vector<Add>(op);
            } else {
                return hoist_slice_vector<Add>(Add::make(a, b));
            }
        }
    }

    if (a.same_as(op->a) && b.same_as(op->b)) {
        return op;
    } else {
        return Add::make(a, b);
    }
}

}
}
