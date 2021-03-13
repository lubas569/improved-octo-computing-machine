#include "SlidingWindow.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Scope.h"
#include "Debug.h"
#include "Substitute.h"
#include "IRPrinter.h"
#include "Simplify.h"
#include "Monotonic.h"
#include "Bounds.h"
#include "IREquality.h"
#include "ExprUsesVar.h"

namespace Halide {
namespace Internal {

using std::string;
using std::map;
using std::vector;

namespace {

// Does an expression depend on a particular variable?
class ExprDependsOnVar : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Variable *op) {
        if (op->name == var) result = true;
    }

    void visit(const Let *op) {
        op->value.accept(this);
        // The name might be hidden within the body of the let, in
        // which case there's no point descending.
        if (op->name != var) {
            op->body.accept(this);
        }
    }
public:

    bool result;
    string var;

    ExprDependsOnVar(string v) : result(false), var(v) {
    }
};

bool expr_depends_on_var(Expr e, string v) {
    ExprDependsOnVar depends(v);
    e.accept(&depends);
    return depends.result;
}


class ExpandExpr : public IRMutator {
    using IRMutator::visit;
    const Scope<Expr> &scope;

    void visit(const Variable *var) {
        if (scope.contains(var->name)) {
            expr = scope.get(var->name);
            debug(3) << "Fully expanded " << var->name << " -> " << expr << "\n";
        } else {
            expr = var;
        }
    }

public:
    ExpandExpr(const Scope<Expr> &s) : scope(s) {}

};

// Perform all the substitutions in a scope
Expr expand_expr(Expr e, const Scope<Expr> &scope) {
    ExpandExpr ee(scope);
    Expr result = ee.mutate(e);
    debug(3) << "Expanded " << e << " into " << result << "\n";
    return result;
}

}

// Perform sliding window optimization for a function over a
// particular serial for loop
class SlidingWindowOnFunctionAndLoop : public IRMutator {
    Function func;
    vector<bool> &slid;
    string loop_var;
    Expr loop_min, loop_step;
    Scope<Expr> scope;

    map<string, Expr> replacements;

    using IRMutator::visit;

    // Check if the dimension at index 'dim_idx' is always pure (i.e. equal to 'dim')
    // in the definition (including in its specializations)
    bool is_dim_always_pure(const Definition &def, const string& dim, int dim_idx) {
        const Variable *var = def.args()[dim_idx].as<Variable>();
        if ((!var) || (var->name != dim)) {
            return false;
        }

        for (const auto &s : def.specializations()) {
            bool pure = is_dim_always_pure(s.definition, dim, dim_idx);
            if (!pure) {
                return false;
            }
        }
        return true;
    }

    void visit(const ProducerConsumer *op) {
        if (!op->is_producer || (op->name != func.name())) {
            IRMutator::visit(op);
        } else {

            stmt = op;

            // We're interested in the case where exactly one of the
            // dimensions of the buffer has a min/extent that depends
            // on the loop_var.
            string dim = "";
            int dim_idx = 0;
            Expr min_required, max_required;

            debug(3) << "Considering sliding " << func.name()
                     << " along loop variable " << loop_var << "\n"
                     << "Region provided:\n";

            string prefix = func.name() + ".s" + std::to_string(func.updates().size()) + ".";
            const vector<string> func_args = func.args();
            for (int i = 0; i < func.dimensions(); i++) {
                // Look up the region required of this function's last stage
                string var = prefix + func_args[i];
                if (!scope.contains(var + ".min") ||
                    !scope.contains(var + ".max")) {
                    // The lets that define these are just outside the
                    // produce node. They should be in terms of the
                    // var we're sliding over. If the var we're
                    // sliding over is defined *inside* these lets,
                    // they won't be in scope, and we've gone too var
                    // anyway.
                    return;
                }
                Expr min_req = scope.get(var + ".min");
                Expr max_req = scope.get(var + ".max");
                debug(3) << var << ":" << min_req << ", " << max_req  << "\n";
                min_req = expand_expr(min_req, scope);
                max_req = expand_expr(max_req, scope);
                debug(3) << var << ":" << min_req << ", " << max_req  << "\n";
                if (expr_depends_on_var(min_req, loop_var) ||
                    expr_depends_on_var(max_req, loop_var)) {
                    if (!dim.empty()) {
                        dim = "";
                        min_required = Expr();
                        max_required = Expr();
                        break;
                    } else {
                        dim = func_args[i];
                        dim_idx = i;
                        min_required = min_req;
                        max_required = max_req;
                    }
                }
            }

            if (slid[dim_idx]) {
                debug(3) << "Could not perform sliding window optimization of "
                         << func.name() << " over " << loop_var << " in dimension " << dim
                         << "because this function has already been slid over this dimension.\n";
                return;
            }


            if (!min_required.defined()) {
                debug(3) << "Could not perform sliding window optimization of "
                         << func.name() << " over " << loop_var << " because either zero "
                         << "or many dimensions of the function dependended on the loop var\n";
                return;
            }

            // If the function is not pure in the given dimension, give up. We also
            // need to make sure that it is pure in all the specializations
            bool pure = true;
            for (const Definition &def : func.updates()) {
                pure = is_dim_always_pure(def, dim, dim_idx);
                if (!pure) {
                    break;
                }
            }
            if (!pure) {
                debug(3) << "Could not performance sliding window optimization of "
                         << func.name() << " over " << loop_var << " because the function "
                         << "scatters along the related axis.\n";
                return;
            }

            bool can_slide_up = false;
            bool can_slide_down = false;

            Monotonic monotonic_min = is_monotonic(min_required, loop_var);
            Monotonic monotonic_max = is_monotonic(max_required, loop_var);

            if (monotonic_min == Monotonic::Increasing ||
                monotonic_min == Monotonic::Constant) {
                can_slide_up = true;
            }

            if (monotonic_max == Monotonic::Decreasing ||
                monotonic_max == Monotonic::Constant) {
                can_slide_down = true;
            }

            if (!can_slide_up && !can_slide_down) {
                debug(3) << "Not sliding " << func.name()
                         << " over dimension " << dim
                         << " along loop variable " << loop_var
                         << " because I couldn't prove it moved monotonically along that dimension\n"
                         << "Min is " << min_required << "\n"
                         << "Max is " << max_required << "\n";
                return;
            }

            // Ok, we've isolated a function, a dimension to slide
            // along, and loop variable to slide over.
            debug(3) << "Sliding " << func.name()
                     << " over dimension " << dim
                     << " along loop variable " << loop_var
                     << " with step " << loop_step
                     << "\n";

            Expr loop_var_expr = Variable::make(Int(32), loop_var);

            Expr prev_max_plus_one = substitute(loop_var, loop_var_expr - loop_step, max_required) + 1;
            Expr prev_min_minus_one = substitute(loop_var, loop_var_expr - loop_step, min_required) - 1;

            debug(0) << max_required << ", " << prev_max_plus_one << "\n";

            // If there's no overlap between adjacent iterations, we shouldn't slide.
            if (can_prove(min_required >= prev_max_plus_one) ||
                can_prove(max_required <= prev_min_minus_one)) {
                debug(3) << "Not sliding " << func.name()
                         << " over dimension " << dim
                         << " along loop variable " << loop_var
                         << " there's no overlap in the region computed across iterations\n"
                         << "Min is " << min_required << "\n"
                         << "Max is " << max_required << "\n";
                return;
            }

            Expr new_min, new_max;
            if (can_slide_up) {
                new_min = select(loop_var_expr <= loop_min, min_required, likely(prev_max_plus_one));
                new_max = max_required;
            } else {
                new_min = min_required;
                new_max = select(loop_var_expr <= loop_min, max_required, likely(prev_min_minus_one));
            }

            Expr early_stages_min_required = new_min;
            Expr early_stages_max_required = new_max;

            debug(3) << "Sliding " << func.name() << ", " << dim << "\n"
                     << "Pushing min up from " << min_required << " to " << new_min << "\n"
                     << "Shrinking max from " << max_required << " to " << new_max << "\n";
            slid[dim_idx] = true;

            // Now redefine the appropriate regions required
            if (can_slide_up) {
                replacements[prefix + dim + ".min"] = new_min;
            } else {
                replacements[prefix + dim + ".max"] = new_max;
            }

            for (size_t i = 0; i < func.updates().size(); i++) {
                string n = func.name() + ".s" + std::to_string(i) + "." + dim;
                replacements[n + ".min"] = Variable::make(Int(32), prefix + dim + ".min");
                replacements[n + ".max"] = Variable::make(Int(32), prefix + dim + ".max");
            }

            // Ok, we have a new min/max required and we're going to
            // rewrite all the lets that define bounds required. Now
            // we need to additionally expand the bounds required of
            // the last stage to cover values produced by stages
            // before the last one. Because, e.g., an intermediate
            // stage may be unrolled, expanding its bounds provided.
            if (!func.updates().empty()) {
                Box b = box_provided(op->body, func.name());
                if (can_slide_up) {
                    string n = prefix + dim + ".min";
                    Expr var = Variable::make(Int(32), n);
                    stmt = LetStmt::make(n, min(var, b[dim_idx].min), stmt);
                } else {
                    string n = prefix + dim + ".max";
                    Expr var = Variable::make(Int(32), n);
                    stmt = LetStmt::make(n, max(var, b[dim_idx].max), stmt);
                }
            }
        }
    }

    void visit(const For *op) {
        // It's not safe to enter an inner loop whose bounds depend on
        // the var we're sliding over.
        Expr min = expand_expr(op->min, scope);
        Expr extent = expand_expr(op->extent, scope);
        if (is_one(extent)) {
            // Just treat it like a let
            Stmt s = LetStmt::make(op->name, min, op->body);
            s = mutate(s);
            // Unpack it back into the for
            const LetStmt *l = s.as<LetStmt>();
            internal_assert(l);
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, l->body);
        } else if (is_monotonic(min, loop_var) != Monotonic::Constant ||
                   is_monotonic(extent, loop_var) != Monotonic::Constant) {
            debug(3) << "Not entering loop over " << op->name
                     << " because the bounds depend on the var we're sliding over: "
                     << min << ", " << extent << "\n";
            stmt = op;
        } else {
            IRMutator::visit(op);

        }
    }

    void visit(const LetStmt *op) {
        debug(0) << op->name << ": " << op->value << ", " << expand_expr(op->value, scope) << "\n";
        scope.push(op->name, simplify(expand_expr(op->value, scope)));
        Stmt new_body = mutate(op->body);

        Expr value = op->value;

        map<string, Expr>::iterator iter = replacements.find(op->name);
        if (iter != replacements.end()) {
            value = iter->second;
            replacements.erase(iter);
        }

        if (new_body.same_as(op->body) && value.same_as(op->value)) {
            stmt = op;
        } else {
            stmt = LetStmt::make(op->name, value, new_body);
        }
        scope.pop(op->name);
    }

public:
    SlidingWindowOnFunctionAndLoop(Function f, vector<bool> &slid, string v, Expr v_min, Expr v_step) :
        func(f), slid(slid), loop_var(v), loop_min(v_min), loop_step(v_step) {}
};

// Perform sliding window optimization for a particular function
class SlidingWindowOnFunction : public IRMutator {
    Function func;

    // A variable that linearly walks forwards over time from min to
    // min + extent in jumps of size step.
    struct AffineVar {
        Expr min, extent, step;

        // Depth in the loop nest. Necessary to track for combining
        // multiple affine vars.
        int nesting_depth;
    };

    Scope<AffineVar> affine_vars;
    Scope<int> varying, parallel;

    // Track which dimensions we have slid over
    vector<bool> slid;

    int depth = 0;

    using IRMutator::visit;

    void visit(const For *op) {
        debug(3) << " Doing sliding window analysis over loop: " << op->name << "\n";

        Stmt new_body = op->body;

        if (op->for_type == ForType::Serial ||
            op->for_type == ForType::Unrolled) {
            AffineVar v {op->min, op->extent, 1, depth};
            affine_vars.push(op->name, v);
        } else {
            parallel.push(op->name, 0);
        }
        varying.push(op->name, 0);

        depth++;
        new_body = mutate(new_body);
        depth--;

        varying.pop(op->name);
        if (op->for_type == ForType::Serial ||
            op->for_type == ForType::Unrolled) {
            new_body = try_to_slide_over_affine_var(new_body, op->name, affine_vars.get(op->name));
            affine_vars.pop(op->name);
        } else {
            parallel.pop(op->name);
        }

        if (new_body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, new_body);
        }
    }

    Stmt try_to_slide_over_affine_var(Stmt body, const string &name, const AffineVar &v) {
        SlidingWindowOnFunctionAndLoop slider(func, slid, name, simplify(v.min), simplify(v.step));
        body = slider.mutate(body);
        return body;
    }

    bool is_constant(Expr e) {
        return !expr_uses_vars(e, varying);
    }

    bool is_affine(Expr e, AffineVar &result) {
        const Add *add = e.as<Add>();
        const Mul *mul = e.as<Mul>();
        const Variable *v = e.as<Variable>();
        AffineVar result_a, result_b;
        if (v && affine_vars.contains(v->name)) {
            result = affine_vars.get(v->name);
        } else if (add && is_affine(add->a, result) && is_constant(add->b)) {
            result.min += add->b;
        } else if (add && is_constant(add->a) && is_affine(add->b, result)) {
            result.min += add->a;
        } else if (mul && is_affine(mul->a, result) && is_constant(mul->b)) {
            result.min *= mul->b;
            result.step *= mul->b;
        } else if (mul && is_constant(mul->a) && is_affine(mul->b, result)) {
            result.min *= mul->a;
            result.step *= mul->a;
        } else if (add &&
                   is_affine(add->a, result_a) &&
                   is_affine(add->b, result_b) &&
                   ((result_a.nesting_depth < result_b.nesting_depth &&
                     can_prove(result_a.step == result_b.extent)) ||
                    (result_b.nesting_depth < result_a.nesting_depth &&
                     can_prove(result_b.step == result_a.extent)))) {
            if (result_a.nesting_depth < result_b.nesting_depth) {
                // outer + inner
                result = result_a;
                result.min += result_b.min;
                result.step = result_b.step;
                result.nesting_depth = result_b.nesting_depth;
            } else {
                // inner + outer;
                result = result_b;
                result.min += result_a.min;
                result.step = result_a.step;
                result.nesting_depth = result_a.nesting_depth;
            }
        } else {
            return false;
        }
        return true;
    }

    bool is_parallel(Expr e) {
        return expr_uses_vars(e, parallel);
    }

    void visit(const LetStmt *op) {
        debug(0) << "Visiting let: " << op->name << " = " << op->value << "\n";

        if (is_parallel(op->value)) {
            parallel.push(op->name, 0);
            varying.push(op->name, 0);
            IRMutator::visit(op);
            varying.pop(op->name);
            parallel.pop(op->name);
            return;
        }

        // We can slide over any var that linearly increases over
        // time. This isn't just for loops. Recognize some
        // combinations of affine vars into new affine vars that we
        // could potentially slide over.
        AffineVar v;
        if (is_affine(op->value, v)) {
            affine_vars.push(op->name, v);
            varying.push(op->name, 0);

            debug(0) << "New affine var: " << op->name << ", " << v.min << ", " << v.extent << ", " << v.step << "\n";

            Stmt body = try_to_slide_over_affine_var(op->body, op->name, v);
            body = mutate(body);

            varying.pop(op->name);
            affine_vars.pop(op->name);

            if (body.same_as(op->body)) {
                stmt = op;
            } else {
                stmt = LetStmt::make(op->name, op->value, body);
            }

            return;
        }

        if (is_constant(op->value)) {
            IRMutator::visit(op);
        } else {
            varying.push(op->name, 0);
            IRMutator::visit(op);
            varying.pop(op->name);
        }
    }

public:
    SlidingWindowOnFunction(Function f) : func(f), slid(f.dimensions()) {}
};

// Perform sliding window optimization for all functions
class SlidingWindow : public IRMutator {
    const map<string, Function> &env;

    using IRMutator::visit;

    void visit(const Realize *op) {
        // Find the args for this function
        map<string, Function>::const_iterator iter = env.find(op->name);

        // If it's not in the environment it's some anonymous
        // realization that we should skip (e.g. an inlined reduction)
        if (iter == env.end()) {
            IRMutator::visit(op);
            return;
        }

        // If the Function in question has the same compute_at level
        // as its store_at level, skip it.
        const Schedule &sched = iter->second.schedule();
        if (sched.compute_level() == sched.store_level()) {
            IRMutator::visit(op);
            return;
        }

        Stmt new_body = op->body;

        debug(3) << "Doing sliding window analysis on realization of " << op->name << "\n";

        new_body = SlidingWindowOnFunction(iter->second).mutate(new_body);

        new_body = mutate(new_body);

        if (new_body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, new_body);
        }
    }
public:
    SlidingWindow(const map<string, Function> &e) : env(e) {}

};

class PropagateConstants : public IRMutator {
    using IRMutator::visit;
    Scope<Expr> scope;

    void visit(const Variable *op) {
        if (scope.contains(op->name)) {
            Expr e = scope.get(op->name);
            if (e.defined()) {
                expr = e;
            } else {
                expr = op;
            }
        } else {
            expr = op;
        }
    }

    void visit(const LetStmt *op) {
        Expr val = simplify(mutate(op->value));
        if (is_const(val)) {
            scope.push(op->name, val);
        } else {
            // Push an undef Expr to hide any constant outer
            // definition of this symbol.
            scope.push(op->name, Expr());
        }
        Stmt body = mutate(op->body);
        if (val.same_as(op->value) && body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = LetStmt::make(op->name, val, body);
        }
    }
};

Stmt sliding_window(Stmt s, const map<string, Function> &env) {
    s = PropagateConstants().mutate(s);
    debug(0) << s << "\n";
    return SlidingWindow(env).mutate(s);
}

}
}
