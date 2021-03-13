#include <algorithm>

#include "CSE.h"
#include "CodeGen_GPU_Dev.h"
#include "ExprUsesVar.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Simplify.h"
#include "Solve.h"
#include "Substitute.h"
#include "TrimNoOps.h"
#include "Var.h"

#include <chrono>

extern std::string PROFILE_indent;
extern bool PROFILE_enabled;

#define PROFILE(...)            \
[&]()                           \
{                               \
    typedef std::chrono::high_resolution_clock clock_t; \
    PROFILE_indent.push_back(' '); \
    PROFILE_indent.push_back(' '); \
    auto ini = clock_t::now();  \
    __VA_ARGS__;                \
    auto end = clock_t::now();  \
    if (!PROFILE_indent.empty()) PROFILE_indent.pop_back();  \
    if (!PROFILE_indent.empty()) PROFILE_indent.pop_back();  \
    auto eps = std::chrono::duration<double>(end - ini).count();    \
    return(eps);                \
}()

#define PROFILE_P(label, ...) \
{   \
    auto eps = PROFILE(__VA_ARGS__);    \
    if (PROFILE_enabled) \
    printf("%s" #label "> %fs\n", PROFILE_indent.c_str(), eps); \
}

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::string;
using std::vector;

namespace {

/** Remove identity functions, even if they have side-effects. */
class StripIdentities : public IRMutator {
    using IRMutator::visit;

    Expr visit(const Call *op) override {
        if (op->is_intrinsic(Call::return_second) ||
            op->is_intrinsic(Call::likely) ||
            op->is_intrinsic(Call::likely_if_innermost)) {
            return mutate(op->args.back());
        } else {
            return IRMutator::visit(op);
        }
    }
};

/** Check if an Expr loads from the given buffer. */
class LoadsFromBuffer : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Load *op) override {
        if (op->name == buffer) {
            result = true;
        } else {
            IRVisitor::visit(op);
        }
    }

    string buffer;
public:
    bool result = false;
    LoadsFromBuffer(const string &b) : buffer(b) {}
};

bool loads_from_buffer(Expr e, string buf) {
    LoadsFromBuffer l(buf);
    e.accept(&l);
    return l.result;
}

/** Construct a sufficient condition for the visited stmt to be a no-op. */
class IsNoOp : public IRVisitor {
    using IRVisitor::visit;

    Expr make_and(Expr a, Expr b) {
        if (is_zero(a) || is_one(b)) return a;
        if (is_zero(b) || is_one(a)) return b;
        return a && b;
    }

    Expr make_or(Expr a, Expr b) {
        if (is_zero(a) || is_one(b)) return b;
        if (is_zero(b) || is_one(a)) return a;
        return a || b;
    }

    void visit(const Store *op) override {
        if (op->value.type().is_handle() || is_zero(op->predicate)) {
            condition = const_false();
        } else {
            if (is_zero(condition)) {
                return;
            }
            // If the value being stored is the same as the value loaded,
            // this is a no-op
            debug(3) << "Considering store: " << Stmt(op) << "\n";

            // Early-out: There's no way for that to be true if the
            // RHS does not load from the buffer being stored to.
            if (!loads_from_buffer(op->value, op->name)) {
                condition = const_false();
                return;
            }

            Expr equivalent_load = Load::make(op->value.type(), op->name, op->index,
                                              Buffer<>(), Parameter(), op->predicate, op->alignment);
            Expr is_no_op = equivalent_load == op->value;
            is_no_op = StripIdentities().mutate(is_no_op);
            // We need to call CSE since sometimes we have "let" stmt on the RHS
            // that makes the expr harder to solve, i.e. the solver will just give up
            // and return a conservative false on call to and_condition_over_domain().
            is_no_op = simplify(common_subexpression_elimination(is_no_op));
            debug(3) << "Anding condition over domain... " << is_no_op << "\n";
            is_no_op = and_condition_over_domain(is_no_op, Scope<Interval>::empty_scope());
            condition = make_and(condition, is_no_op);
            debug(3) << "Condition is now " << condition << "\n";
        }
    }

    void visit(const For *op) override {
        if (is_zero(condition)) {
            return;
        }
        Expr old_condition = condition;
        condition = const_true();
        op->body.accept(this);
        Scope<Interval> varying;
        varying.push(op->name, Interval(op->min, op->min + op->extent - 1));
        condition = simplify(common_subexpression_elimination(condition));
        debug(3) << "About to relax over " << op->name << " : " << condition << "\n";
        condition = and_condition_over_domain(condition, varying);
        debug(3) << "Relaxed: " << condition << "\n";
        condition = make_and(old_condition, make_or(condition, simplify(op->extent <= 0)));
    }

    void visit(const IfThenElse *op) override {
        if (is_zero(condition)) {
            return;
        }
        Expr total_condition = condition;
        condition = const_true();
        op->then_case.accept(this);
        // This is a no-op if we're previously a no-op, and the
        // condition is false or the if body is a no-op.
        total_condition = make_and(total_condition, make_or(!op->condition, condition));
        condition = const_true();
        if (op->else_case.defined()) {
            op->else_case.accept(this);
            total_condition = make_and(total_condition, make_or(op->condition, condition));
        }
        condition = total_condition;
    }

    void visit(const Call *op) override {
        // If the loop calls an impure function, we can't remove the
        // call to it. Most notably: image_store.
        if (!op->is_pure()) {
            condition = const_false();
            return;
        }
        IRVisitor::visit(op);
    }

    template<typename LetOrLetStmt>
    void visit_let(const LetOrLetStmt *op) {
        IRVisitor::visit(op);
        if (expr_uses_var(condition, op->name)) {
            condition = Let::make(op->name, op->value, condition);
        }
    }

    void visit(const LetStmt *op) override {
        visit_let(op);
    }

    void visit(const Let *op) override {
        visit_let(op);
    }

public:
    Expr condition = const_true();
};

class SimplifyUsingBounds : public IRMutator {
    struct ContainingLoop {
        string var;
        Interval i;
    };
    vector<ContainingLoop> containing_loops;

    using IRMutator::visit;

    // Can we prove a condition over the non-rectangular domain of the for loops we're in?
    Expr common_subexpression_elimination_local(const Expr& expr)
    {
        Expr ret_expr;
        PROFILE_P("common_subexpression_elimination()",
        ret_expr = common_subexpression_elimination(expr);
        );
        return ret_expr;
    };
    bool expr_uses_var_local(Expr e, const std::string &v)
    {
        bool ret;
        //PROFILE_P("expr_uses_var()",
        ret = expr_uses_var(e, v);
        //);
        return ret;
    }
    bool can_prove_local(Expr e)
    {
        bool ret;
        //PROFILE_P("can_prove()",
        ret = can_prove(e);
        //);
        return ret;
    }
    SolverResult solve_expression_local(Expr e, const std::string &variable)
    {
        SolverResult ret;
        PROFILE_P("solve_expression()",
        ret = solve_expression(e, variable);
        );
        return ret;
    }
    Expr and_condition_over_domain_local(Expr c, const Scope<Interval> &varying)
    {
        Expr ret_expr;
        //PROFILE_P("and_condition_over_domain()",
        ret_expr = and_condition_over_domain(c, varying);
        //);
        return ret_expr;
    }
    Expr simplify_local(Expr e)
    {
        Expr ret_expr;
        PROFILE_P("simplify()",
        ret_expr = simplify(e);
        );
        return ret_expr;
    }
    bool is_one_local(const Expr& e)
    {
        bool ret;
        //PROFILE_P("is_one()",
        ret = is_one(e);
        //);
        return ret;
    }
    bool provably_true_over_domain(Expr test) {
        bool ret;
        PROFILE_P("provably_true_over_domain()",
        debug(3) << "Attempting to prove: " << test << "\n";
        for (size_t i = containing_loops.size(); i > 0; i--) {
            // Because the domain is potentially non-rectangular, we
            // need to take each variable one-by-one, simplifying in
            // between to allow for cancellations of the bounds of
            // inner loops with outer loop variables.
            auto loop = containing_loops[i-1];
            if (is_const(test)) {
                break;
            } else if (!expr_uses_var_local(test, loop.var)) {
                continue;
            }  else if (loop.i.is_bounded() &&
                        can_prove_local(loop.i.min == loop.i.max) &&
                        expr_uses_var_local(test, loop.var)) {
                // If min == max then either the domain only has one correct value, which we
                // can substitute directly.
                // Need to call CSE here since simplify() is sometimes unable to simplify expr with
                // non-trivial 'let' value, e.g. (let x = min(10, y-1) in (x < y))
                test = common_subexpression_elimination_local(Let::make(loop.var, loop.i.min, test));
            } else if (loop.i.is_bounded() &&
                       can_prove_local(loop.i.min >= loop.i.max) &&
                       expr_uses_var_local(test, loop.var)) {
                // If min >= max then either the domain only has one correct value,
                // or the domain is empty, which implies both min/max are true under
                // the domain.
                // Need to call CSE here since simplify() is sometimes unable to simplify expr with
                // non-trivial 'let' value, e.g. (let x = 10 in x < y) || (let x = min(10, y-1) in (x < y))
                test = common_subexpression_elimination_local(Let::make(loop.var, loop.i.min, test) ||
                                                              Let::make(loop.var, loop.i.max, test));
            } else {
                Scope<Interval> s;
                // Rearrange the expression if possible so that the
                // loop var only occurs once.
                SolverResult solved = solve_expression_local(test, loop.var);
                if (solved.fully_solved) {
                    test = solved.result;
                }
                s.push(loop.var, loop.i);
                test = and_condition_over_domain_local(test, s);
            }
            test = simplify_local(test);
            debug(3) << " -> " << test << "\n";
        }
        ret = is_one_local(test);
        );
        return ret;
    }

    Expr visit(const Min *op) override {
        Expr expr;
        //PROFILE_P("SimplifyUsingBounds::visit(Min)",
        if (!op->type.is_int() || op->type.bits() < 32) {
            expr = IRMutator::visit(op);
        } else {
            Expr a = mutate(op->a);
            Expr b = mutate(op->b);
            Expr test = a <= b;
            if (provably_true_over_domain(a <= b)) {
                expr = a;
            } else if (provably_true_over_domain(b <= a)) {
                expr = b;
            } else {
                expr = Min::make(a, b);
            }
        }
        //);
        return expr;
    }

    Expr visit(const Max *op) override {
        Expr expr;
        //PROFILE_P("SimplifyUsingBounds::visit(Max)",
        if (!op->type.is_int() || op->type.bits() < 32) {
            expr = IRMutator::visit(op);
        } else {
            Expr a = mutate(op->a);
            Expr b = mutate(op->b);
            if (provably_true_over_domain(a >= b)) {
                expr = a;
            } else if (provably_true_over_domain(b >= a)) {
                expr = b;
            } else {
                expr = Max::make(a, b);
            }
        }
        //);
        return expr;
    }

    template<typename Cmp>
    Expr visit_cmp(const Cmp *op) {
        Expr expr;
        //PROFILE_P("SimplifyUsingBounds::visit_cmp()",
        expr = IRMutator::visit(op);
        if (provably_true_over_domain(expr)) {
            expr = make_one(op->type);
        } else if (provably_true_over_domain(!expr)) {
            expr = make_zero(op->type);
        }
        //);
        return expr;
    }

    Expr visit(const LE *op) override {
        Expr ret_expr;
        //PROFILE_P("SimplifyUsingBounds::visit(LE)",
        ret_expr = visit_cmp(op);
        //);
        return ret_expr;
    }

    Expr visit(const LT *op) override {
        Expr ret_expr;
        //PROFILE_P("SimplifyUsingBounds::visit(LT)",
        ret_expr = visit_cmp(op);
        //);
        return ret_expr;
    }

    Expr visit(const GE *op) override {
        Expr ret_expr;
        //PROFILE_P("SimplifyUsingBounds::visit(GE)",
        ret_expr = visit_cmp(op);
        //);
        return ret_expr;
    }

    Expr visit(const GT *op) override {
        Expr ret_expr;
        //PROFILE_P("SimplifyUsingBounds::visit(GT)",
        ret_expr = visit_cmp(op);
        //);
        return ret_expr;
    }

    Expr visit(const EQ *op) override {
        Expr ret_expr;
        //PROFILE_P("SimplifyUsingBounds::visit(EQ)",
        ret_expr = visit_cmp(op);
        //);
        return ret_expr;
    }

    Expr visit(const NE *op) override {
        Expr ret_expr;
        //PROFILE_P("SimplifyUsingBounds::visit(NE)",
        ret_expr = visit_cmp(op);
        //);
        return ret_expr;
    }

    template<typename StmtOrExpr, typename LetStmtOrLet>
    StmtOrExpr visit_let(const LetStmtOrLet *op) {
        Expr value;
        StmtOrExpr body;
        //PROFILE_P("SimplifyUsingBounds::visit(LetStmtOrLet)",
        value = mutate(op->value);
        containing_loops.push_back({op->name, {value, value}});
        body = mutate(op->body);
        containing_loops.pop_back();
        //);
        return LetStmtOrLet::make(op->name, value, body);
    }

    Expr visit(const Let *op) override {
        return visit_let<Expr, Let>(op);
    }

    Stmt visit(const LetStmt *op) override {
        return visit_let<Stmt, LetStmt>(op);
    }

    Stmt visit(const For *op) override {
        // Simplify the loop bounds.
        Expr min;
        Expr extent;
        Stmt body;
        PROFILE_P("SimplifyUsingBounds::visit(For)",
        min = mutate(op->min);
        extent = mutate(op->extent);
        containing_loops.push_back({op->name, {min, min + extent - 1}});
        body = mutate(op->body);
        containing_loops.pop_back();
        );
        return For::make(op->name, min, extent, op->for_type, op->device_api, body);
    }
public:
    SimplifyUsingBounds(const string &v, const Interval &i) {
        containing_loops.push_back({v, i});
    }

    SimplifyUsingBounds() {}
};

class TrimNoOps : public IRMutator {
    using IRMutator::visit;

    Stmt visit(const For *op) override {

        // Bounds of GPU loops can't depend on outer gpu loop vars
        if (CodeGen_GPU_Dev::is_gpu_var(op->name)) {
            debug(3) << "TrimNoOps found gpu loop var: " << op->name << "\n";
            return IRMutator::visit(op);
        }

        Stmt body;
        //PROFILE_P("TrimNoOps::visit(For) -- body = mutate()",
        body = mutate(op->body);
        //);

        debug(3) << "\n\n ***** Trim no ops in loop over " << op->name << "\n";

        IsNoOp is_no_op;
        //PROFILE_P("TrimNoOps::visit(For) -- IsNoOp -- body.accept()",
        body.accept(&is_no_op);
        //);
        debug(3) << "Condition is " << is_no_op.condition << "\n";

        //PROFILE_P("TrimNoOps::visit(For) -- simplify(simplify(common_subexpression_elimination()))",
        is_no_op.condition = simplify(simplify(common_subexpression_elimination(is_no_op.condition)));
        //);

        debug(3) << "Simplified condition is " << is_no_op.condition << "\n";

        if (is_one(is_no_op.condition)) {
            // This loop is definitely useless
            return Evaluate::make(0);
        } else if (is_zero(is_no_op.condition)) {
            // This loop is definitely needed
            return For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
        }

        // The condition is something interesting. Try to see if we
        // can trim the loop bounds over which the loop does
        // something.
        Interval i;
        //PROFILE_P("TrimNoOps::visit(For) -- solve_for_outer_interval",
        i = solve_for_outer_interval(!is_no_op.condition, op->name);
        //);

        debug(3) << "Interval is: " << i.min << ", " << i.max << "\n";

        if (i.is_everything()) {
            // Nope.
            return For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
        }

        if (i.is_empty()) {
            // Empty loop
            return Evaluate::make(0);
        }

        // Simplify the body to take advantage of the fact that the
        // loop range is now truncated
        PROFILE_P("TrimNoOps::visit(For) -- SimplifyUsingBounds",
        body = simplify(SimplifyUsingBounds(op->name, i).mutate(body));
        );

        Stmt stmt;
        //PROFILE_P("TrimNoOps::visit(For) -- tail",
        string new_min_name = unique_name(op->name + ".new_min");
        string new_max_name = unique_name(op->name + ".new_max");
        string old_max_name = unique_name(op->name + ".old_max");
        Expr new_min_var = Variable::make(Int(32), new_min_name);
        Expr new_max_var = Variable::make(Int(32), new_max_name);
        Expr old_max_var = Variable::make(Int(32), old_max_name);

        // Convert max to max-plus-one
        if (i.has_upper_bound()) {
            i.max = i.max + 1;
        }

        // Truncate the loop bounds to the region over which it's not
        // a no-op.
        Expr old_max = op->min + op->extent;
        Expr new_min, new_max;
        if (i.has_lower_bound()) {
            new_min = clamp(i.min, op->min, old_max_var);
        } else {
            new_min = op->min;
        }
        if (i.has_upper_bound()) {
            new_max = clamp(i.max, new_min_var, old_max_var);
        } else {
            new_max = old_max;
        }

        Expr new_extent = new_max_var - new_min_var;

        stmt = For::make(op->name, new_min_var, new_extent, op->for_type, op->device_api, body);
        stmt = LetStmt::make(new_max_name, new_max, stmt);
        stmt = LetStmt::make(new_min_name, new_min, stmt);
        stmt = LetStmt::make(old_max_name, old_max, stmt);
        stmt = simplify(stmt);

        debug(3) << "Rewrote loop.\n"
                 << "Old: " << Stmt(op) << "\n"
                 << "New: " << stmt << "\n";
        //);

        return stmt;
    }
};

}  // namespace

Stmt trim_no_ops(Stmt s) {
    s = TrimNoOps().mutate(s);
    return s;
}

}  // namespace Internal
}  // namespace Halide
