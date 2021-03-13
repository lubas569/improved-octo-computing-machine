#include "Halide.h"

#include <fstream>

using namespace Halide;
using namespace Halide::Internal;

using std::string;
using std::vector;
using std::map;
using std::set;
using std::pair;
using std::ostringstream;
using std::tuple;

// Convert from a Halide Expr to SMT2 to pass to z3
string expr_to_smt2(const Expr &e) {
    class ExprToSMT2 : public IRVisitor {
    public:
        std::ostringstream formula;

    protected:

        void visit(const IntImm *imm) override {
            formula << imm->value;
        }

        void visit(const UIntImm *imm) override {
            formula << imm->value;
        }

        void visit(const FloatImm *imm) override {
            formula << imm->value;
        }

        void visit(const StringImm *imm) override {
            formula << imm->value;
        }

        void visit(const Variable *var) override {
            formula << var->name;
        }

        void visit(const Add *op) override {
            formula << "(+ ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Sub *op) override {
            formula << "(- ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Mul *op) override {
            formula << "(* ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Div *op) override {
            formula << "(div ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Mod *op) override {
            formula << "(mod ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Min *op) override {
            formula << "(my_min ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Max *op) override {
            formula << "(my_max ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const EQ *op) override {
            formula << "(= ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const NE *op) override {
            formula << "(not (= ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << "))";
        }

        void visit(const LT *op) override {
            formula << "(< ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const LE *op) override {
            formula << "(<= ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const GT *op) override {
            formula << "(> ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const GE *op) override {
            formula << "(>= ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const And *op) override {
            formula << "(and ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Or *op) override {
            formula << "(or ";
            op->a.accept(this);
            formula << " ";
            op->b.accept(this);
            formula << ")";
        }

        void visit(const Not *op) override {
            formula << "(not ";
            op->a.accept(this);
            formula << ")";
        }

        void visit(const Select *op) override {
            formula << "(ite ";
            op->condition.accept(this);
            formula << " ";
            op->true_value.accept(this);
            formula << " ";
            op->false_value.accept(this);
            formula << ")";
        }

        void visit(const Cast *op) override {
            assert(false && "unhandled");
        }

        void visit(const Ramp *op) override {
            /*
            Expr equiv = op->base + lane_var * op->stride;
            equiv.accept(this);
            */
            assert(false && "unhandled");
        }

        void visit(const Let *op) override {
            formula << "(let ((" << op->name << " ";
            op->value.accept(this);
            formula << ")) ";
            op->body.accept(this);
            formula << ")";
        }

        void visit(const Broadcast *op) override {
            op->value.accept(this);
        }

    } to_smt2;

    e.accept(&to_smt2);
    return to_smt2.formula.str();
}

// Make an expression which can act as any other small integer expression in
// the given leaf terms, depending on the values of the integer opcodes. Not all possible programs are valid (e.g. due to type errors), so also returns an Expr on the inputs opcodes that encodes whether or not the program is well-formed.
pair<Expr, Expr> interpreter_expr(vector<Expr> terms, vector<Expr> use_counts, vector<Expr> opcodes) {
    // Each opcode is an enum identifying the op, followed by the indices of the two args.
    assert(opcodes.size() % 3 == 0);
    assert(terms.size() == use_counts.size());

    Expr program_is_valid = const_true();

    // Type type of each term. Encode int as 0, bool as 1.
    vector<Expr> types;
    for (auto t : terms) {
        if (t.type() == Int(32)) {
            types.push_back(0);
        } else if (t.type() == Bool()) {
            types.push_back(1);
        } else {
            std::cout << t;
            assert(false && "Unhandled wildcard type");
        }
    }

    for (size_t i = 0; i < opcodes.size(); i += 3) {
        Expr op = opcodes[i];
        Expr arg1_idx = opcodes[i+1];
        Expr arg2_idx = opcodes[i+2];

        // Get the args using a select tree. args are either the index of an existing value, or some constant.
        Expr arg1 = arg1_idx, arg2 = arg2_idx;

        // The constants are ints, so make out-of-range values zero.
        Expr arg1_type = 0, arg2_type = 0;
        for (size_t j = 0; j < terms.size(); j++) {
            arg1 = select(arg1_idx == (int)j, terms[j], arg1);
            arg2 = select(arg2_idx == (int)j, terms[j], arg2);
            arg1_type = select(arg1_idx == (int)j, types[j], arg1_type);
            arg2_type = select(arg2_idx == (int)j, types[j], arg2_type);
        }
        int s = (int)terms.size();
        arg1 = select(arg1_idx >= s, arg1_idx - s, arg1);
        arg2 = select(arg2_idx >= s, arg2_idx - s, arg2);

        // Perform the op.
        Expr result = arg1; // By default it's just equal to the first operand. This covers constants too.
        Expr result_type = arg1_type; // Most operators take on the type of the first arg and require the types to match.
        Expr types_ok = arg1_type == arg2_type;

        for (int j = 0; j < (int)use_counts.size(); j++) {
            // We've potentially soaked up one allowed use of each original term
            use_counts[j] -= select((arg1_idx == j) || (op != 0 && arg2_idx == j), 1, 0);
        }

        result = select(op == 1, arg1 + arg2, result);
        result = select(op == 2, arg1 - arg2, result);
        // Only use +/- on integers
        types_ok = (op < 1 || op > 3 || (arg1_type == 0 && arg2_type == 0));

        result = select(op == 3, arg1 * arg2, result);
        // We use bool * int for select(b, x, 0). bool * bool and int
        // * int also make sense.
        types_ok = types_ok || op == 3;

        result = select(op == 4, select(arg1 < arg2, 1, 0), result);
        result = select(op == 5, select(arg1 <= arg2, 1, 0), result);
        result = select(op == 6, select(arg1 == arg2, 1, 0), result);
        result = select(op == 7, select(arg1 != arg2, 1, 0), result);
        // These operators all return bools. They can usefully accept
        // bools too. E.g. A <= B is A implies B. A < B is !A && B.
        result_type = select(op >= 4 && op <= 7, 1, result_type);

        // min/max encodes for or/and, if the types are bool
        result = select(op == 8, min(arg1, arg2), result);
        result = select(op == 9, max(arg1, arg2), result);

        // Only generate div/mod with a few specific constant
        // denominators. Rely on predicates to generalize it.
        // Including these slows synthesis down dramatically.
        /*
        result = select(op == 10, arg1 / 2, result);
        result = select(op == 11, arg1 % 2, result);
        result = select(op == 12, arg1 / 3, result);
        result = select(op == 13, arg1 % 3, result);
        result = select(op == 14, arg1 / 4, result);
        result = select(op == 15, arg1 % 4, result);
        */
        types_ok = select(op > 9, arg1_type == 0 && arg2_idx == 0, types_ok);

        // Type-check it
        program_is_valid = program_is_valid && types_ok && (op <= 9 && op >= 0);

        // TODO: in parallel compute the op histogram, or at least the leading op strength
        terms.push_back(result);
        types.push_back(result_type);
    }

    for (auto u : use_counts) {
        program_is_valid = program_is_valid && (u >= 0);
    }

    return {terms.back(), program_is_valid};
}

// Returns the value of the predicate, whether the opcodes are valid,
// and whether or not the opcodes produce a predicate that's simpler
// (preferable to) some reference predicate.
tuple<Expr, Expr, Expr> predicate_expr(vector<Expr> lhs,
                                       vector<Expr> rhs,
                                       vector<Expr> opcodes,
                                       vector<Expr> opcodes_ref,
                                       map<string, Expr> *binding) {

    // For now we use explicit enumeration of combinations of
    // plausible constraints. We set up the list so that if A => B
    // then B occurs before A in the list. General possible things
    // come before specific things.

    // The values vector is sorted by complexity of the expression.

    vector<Expr> constraints;
    vector<pair<Expr, Expr>> values;
    constraints.push_back(const_true());

    for (auto e1 : lhs) {
        values.emplace_back(e1, const_true());
        values.emplace_back(-e1, const_true());
        constraints.push_back(e1 != 0);
        constraints.push_back(e1 >= 0);
        constraints.push_back(e1 <= 0);
        constraints.push_back(e1 > 0);
        constraints.push_back(e1 < 0);
        constraints.push_back(e1 == 0);
    }

    for (auto e1 : lhs) {
        bool commutative_ok = true;
        for (auto e2 : lhs) {
            if (e1.same_as(e2)) {
                commutative_ok = false;
                continue;
            }
            (void)commutative_ok;
            constraints.push_back(e1 <= e2 + 1);
            constraints.push_back(e1 <= e2);
            constraints.push_back(e1 < e2);
            constraints.push_back(e1 < e2 - 1);
            constraints.push_back(e1 % e2 == 0 && e2 > 0 && e2 < 16);
            constraints.push_back(e1 / e2 == 0 && e2 > 0 && e2 < 16);
            constraints.push_back(e1 == e2 - 1);
            constraints.push_back(e1 == e2 + 1);
            constraints.push_back(e1 == e2);

            if (commutative_ok) {
                constraints.push_back(e1 + e2 <= 1);
                constraints.push_back(e1 + e2 <= 0);
                constraints.push_back(e1 + e2 >= -1);
                constraints.push_back(e1 + e2 >= 0);
                constraints.push_back(e1 + e2 < 0);
                constraints.push_back(e1 + e2 > 0);
                constraints.push_back(e1 + e2 == 0);
                values.emplace_back(e1 + e2, const_true());
                values.emplace_back(min(e1, e2), const_true());
                values.emplace_back(max(e1, e2), const_true());
            }
            values.emplace_back(e1 - e2, const_true());
            values.emplace_back(e1 / e2, e2 > 0 && e2 < 16); // Division rounding down
            values.emplace_back((e1 - 1) / e2 + 1, e2 > 0 && e2 < 16); // Division rounding up
            values.emplace_back(e1 / e2, e2 > 0 && e2 < 16 && e1 % e2 == 0); // Exact division
            values.emplace_back(e1 % e2, e2 > 0 && e2 < 16);
        }
    }
    values.emplace_back(-1, const_true());
    values.emplace_back(0, const_true());
    values.emplace_back(1, const_true());
    values.emplace_back(2, const_true());

    for (auto e1 : lhs) {
        for (auto e2 : lhs) {
            for (auto e3 : lhs) {
                if (e2.same_as(e3)) break;
                constraints.push_back(e1 <= e2 + e3 + 1);
                constraints.push_back(e1 <= e2 + e3);
                constraints.push_back(e1 < e2 + e3);
                constraints.push_back(e1 >= e2 + e3 - 1);
                constraints.push_back(e1 >= e2 + e3);
                constraints.push_back(e1 > e2 + e3);
                constraints.push_back(e1 == e2 + e3);
                constraints.push_back(e1 == e2 * e3);
                constraints.push_back(e1 + e2 * e3 == 0);
            }
        }
    }

    Expr more_general_constraints = const_true();
    Expr same_constraints = const_true();
    for (size_t i = 0; i < rhs.size() + lhs.size(); i++) {
        same_constraints = same_constraints && (opcodes[i] == opcodes_ref[i]);
        more_general_constraints = more_general_constraints && (opcodes[i] <= opcodes_ref[i]);
    }
    Expr strictly_more_general_constraints = !same_constraints && more_general_constraints;

    // Each rhs expr should equal some simple function of the lhs exprs
    Expr result = const_true();
    Expr valid = const_true();

    assert(opcodes.size() == lhs.size() + rhs.size());

    for (size_t i = 0; i < rhs.size(); i++) {
        Expr r = rhs[i];
        Expr val = values[0].first;
        Expr cond = values[0].second;
        Expr op = opcodes[i];
        for (int j = 1; j < (int)values.size(); j++) {
            Expr c = (op == j);
            val = select(c, values[j].first, val);
            cond = select(c, (r == values[j].first) && values[j].second, cond);
        }

        result = result && cond;
        valid = valid && (op >= 0) && (op < (int)values.size());
        if (const Variable *var = r.as<Variable>()) {
            (*binding)[var->name] = val;
        }
    }

    // We have constraint per LHS expr. If we don't need that many,
    // one of the constraints in the list is "true".
    for (size_t i = 0; i < lhs.size(); i++) {
        Expr c = constraints[0];
        Expr op = opcodes[i + rhs.size()];
        for (int j = 1; j < (int)constraints.size(); j++) {
            c = select(op == j, constraints[j], c);
        }
        result = result && c;
        valid = valid && (op >= 0) && (op < (int)constraints.size());
    }

    return {result, valid, strictly_more_general_constraints};
}

bool is_whitespace(char c) {
    return c == ' '  || c == '\n' || c == '\t';
}

void consume_whitespace(char **cursor, char *end) {
    while (*cursor < end && is_whitespace(**cursor)) {
        (*cursor)++;
    }
}

bool consume(char **cursor, char *end, const char *expected) {
    char *tmp = *cursor;
    while (*tmp == *expected && tmp < end && *expected) {
        tmp++;
        expected++;
    }
    if ((*expected) == 0) {
        *cursor = tmp;
        return true;
    } else {
        return false;
    }
}

void expect(char **cursor, char *end, const char *pattern) {
    if (!consume(cursor, end, pattern)) {
        printf("Parsing failed. Expected %s, got %s\n",
               pattern, *cursor);
        abort();
    }
}

bool check(char **cursor, char *end, const char *pattern) {
    char *tmp_cursor = *cursor;
    return consume(&tmp_cursor, end, pattern);
}

string consume_token(char **cursor, char *end) {
    size_t sz = 0;
    while (*cursor + sz < end &&
           (std::isalnum((*cursor)[sz]) ||
            (*cursor)[sz] == '!' ||
            (*cursor)[sz] == '.' ||
            (*cursor)[sz] == '$' ||
            (*cursor)[sz] == '_')) sz++;
    string result{*cursor, sz};
    *cursor += sz;
    return result;
}

int64_t consume_int(char **cursor, char *end) {
    bool negative = consume(cursor, end, "-");
    int64_t n = 0;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        n *= 10;
        n += (**cursor - '0');
        (*cursor)++;
    }
    return negative ? -n : n;
}

Expr consume_float(char **cursor, char *end) {
    bool negative = consume(cursor, end, "-");
    int64_t integer_part = consume_int(cursor, end);
    int64_t fractional_part = 0;
    int64_t denom = 1;
    if (consume(cursor, end, ".")) {
        while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
            denom *= 10;
            fractional_part *= 10;
            fractional_part += (**cursor - '0');
            (*cursor)++;
        }
    }
    double d = integer_part + double(fractional_part) / denom;
    if (negative) {
        d = -d;
    }
    if (consume(cursor, end, "h")) {
        return make_const(Float(16), d);
    } else if (consume(cursor, end, "f")) {
        return make_const(Float(32), d);
    } else {
        return make_const(Float(64), d);
    }
}

bool parse_model(char **cursor, char *end, map<string, Expr> *bindings) {
    consume_whitespace(cursor, end);
    if (!consume(cursor, end, "(model")) return false;
    consume_whitespace(cursor, end);
    while (consume(cursor, end, "(define-fun")) {
        consume_whitespace(cursor, end);
        string name = consume_token(cursor, end);
        consume_whitespace(cursor, end);
        if (!consume(cursor, end, "()")) return false;
        consume_whitespace(cursor, end);
        if (consume(cursor, end, "Bool")) {
            // Don't care about this var
            consume_whitespace(cursor, end);
            if (!consume(cursor, end, "true)")) {
                if (!consume(cursor, end, "false)")) return false;
            }
            consume_whitespace(cursor, end);
        } else {
            if (!consume(cursor, end, "Int")) return false;
            consume_whitespace(cursor, end);
            if (consume(cursor, end, "(- ")) {
                string val = consume_token(cursor, end);
                if (!starts_with(name, "z3name!")) {
                    (*bindings)[name] = -std::atoi(val.c_str());
                }
                consume(cursor, end, ")");
            } else {
                string val = consume_token(cursor, end);
                if (!starts_with(name, "z3name!")) {
                    (*bindings)[name] = std::atoi(val.c_str());
                }
            }
            consume_whitespace(cursor, end);
            consume(cursor, end, ")");
            consume_whitespace(cursor, end);
        }
    }
    consume_whitespace(cursor, end);
    if (!consume(cursor, end, ")")) return false;
    return true;
}


class FindVars : public IRVisitor {
    Scope<> lets;

    void visit(const Variable *op) override {
        if (!lets.contains(op->name)) {
            vars[op->name]++;
        }
    }

    void visit(const Let *op) override {
        op->value.accept(this);
        {
            ScopedBinding<> bind(lets, op->name);
            op->body.accept(this);
        }
    }
public:
    std::map<string, int> vars;
};

enum Z3Result {
    Sat, Unsat, Unknown
};
Z3Result satisfy(Expr e, map<string, Expr> *bindings) {

    e = simplify(common_subexpression_elimination(e));

    if (is_one(e)) {
        return Sat;
    }
    if (is_zero(e)) {
        return Unsat;
    }
    if (!e.type().is_bool()) {
        std::cout << "Cannot satisfy non-boolean expression " << e << "\n";
        abort();
    }

    FindVars find_vars;

    e.accept(&find_vars);

    std::ostringstream z3_source;

    for (auto v : find_vars.vars) {
        z3_source << "(declare-const " << v.first << " Int)\n";
    }

    z3_source << "(define-fun my_min ((x Int) (y Int)) Int (ite (< x y) x y))\n"
              << "(define-fun my_max ((x Int) (y Int)) Int (ite (< x y) y x))\n";

    Expr orig = e;
    while (const Let *l = e.as<Let>()) {
        if (l->value.type().is_int() && l->value.type().bits() >= 32) {
            z3_source << "(declare-const " << l->name << " Int)\n";
        } else if (l->value.type().is_bool()) {
            z3_source << "(declare-const " << l->name << " Bool)\n";
        } else {
            break;
        }
        z3_source << "(assert (= " << l->name << " " << expr_to_smt2(l->value) << "))\n";
        e = l->body;
    }

    z3_source << "(assert " << expr_to_smt2(e) << ")\n"
              << "(check-sat)\n"
              << "(get-model)\n";
    /*
    std::cout << "z3 query:\n" << z3_source.str() << "\n";
    */

    string src = z3_source.str();

    TemporaryFile z3_file("query", "z3");
    TemporaryFile z3_output("output", "txt");
    write_entire_file(z3_file.pathname(), &src[0], src.size());

    std::string cmd = "z3 -T:6 " + z3_file.pathname() + " > " + z3_output.pathname();

    //int ret = system(cmd.c_str());
    int ret = pclose(popen(cmd.c_str(), "r"));

    auto result_vec = read_entire_file(z3_output.pathname());
    string result(result_vec.begin(), result_vec.end());

    if (starts_with(result, "unknown") || starts_with(result, "timeout")) {
        // std::cout << "z3 produced: " << result << "\n";
        return Unknown;
    }

    if (ret && !starts_with(result, "unsat")) {
        std::cout << "** z3 query failed with exit code " << ret << "\n"
                  << "** query was:\n" << src << "\n"
                  << "** output was:\n" << result << "\n";
        return Unknown;
    }

    if (starts_with(result, "unsat")) {
        return Unsat;
    } else {
        char *cursor = &(result[0]);
        char *end = &(result[result.size()]);
        if (!consume(&cursor, end, "sat")) {
            return Unknown;
        }
        parse_model(&cursor, end, bindings);
        return Sat;
    }
}

Var v0("x"), v1("y"), v2("z"), v3("w"), v4("u"), v5("v5"), v6("v6"), v7("v7"), v8("v8"), v9("v9");
Var v10("v10"), v11("v11"), v12("v12"), v13("v13"), v14("v14"), v15("v15"), v16("v16"), v17("v17"), v18("v18"), v19("v19");
Var v20("v20"), v21("v21"), v22("v22"), v23("v23"), v24("v24"), v25("v25"), v26("v26"), v27("v27"), v28("v28"), v29("v29");

Expr reboolify(const Expr &e) {
    if (e.type().is_bool()) return e;
    // e is an integer expression encoding a bool. We want to convert it back to the bool
    if (const Min *op = e.as<Min>()) {
        return reboolify(op->a) && reboolify(op->b);
    } else if (const Max *op = e.as<Max>()) {
        return reboolify(op->a) || reboolify(op->b);
    } else if (const LE *op = e.as<LE>()) {
        return !reboolify(op->a) || reboolify(op->b);
    } else if (const LT *op = e.as<LT>()) {
        return !reboolify(op->a) && reboolify(op->b);
    } else {
        return e == 1;
    }
}

// Use CEGIS to construct an equivalent expression to the input of the given size.
Expr super_simplify(Expr e, int size) {
    bool was_bool = e.type().is_bool();
    Expr orig = e;
    if (was_bool) {
        e = select(e, 1, 0);
    }

    // We may assume there's no undefined behavior in the existing
    // left-hand-side.
    class CheckForUB : public IRVisitor {
        using IRVisitor::visit;
        void visit(const Mod *op) override {
            safe = safe && (op->b != 0);
        }
        void visit(const Div *op) override {
            safe = safe && (op->b != 0);
        }
        void visit(const Let *op) override {
            assert(false && "CheckForUB not written to handle Lets");
        }
    public:
        Expr safe = const_true();
    } ub_checker;
    e.accept(&ub_checker);

    FindVars find_vars;
    e.accept(&find_vars);
    vector<Expr> leaves, use_counts;
    for (auto v : find_vars.vars) {
        leaves.push_back(Variable::make(Int(32), v.first));
        use_counts.push_back(v.second);
    }

    vector<map<string, Expr>> counterexamples;

    map<string, Expr> current_program;

    vector<Expr> symbolic_opcodes;
    for (int i = 0; i < size*3; i++) {
        Var op("op" + std::to_string(i));
        symbolic_opcodes.push_back(op);

        // The initial program is some garbage
        current_program[op.name()] = 0;
    }

    map<string, Expr> all_vars_zero;
    for (auto v : find_vars.vars) {
        all_vars_zero[v.first] = 0;
    }

    auto p = interpreter_expr(leaves, use_counts, symbolic_opcodes);
    Expr program = p.first;
    Expr program_works = (e == program) && p.second;
    program = simplify(common_subexpression_elimination(program));
    program_works = simplify(common_subexpression_elimination(program_works));

    std::mt19937 rng(0);
    std::uniform_int_distribution<int> random_int(-3, 3);

    while (1) {
        // First sythesize a counterexample to the current program.
        Expr current_program_works = substitute(current_program, program_works);
        map<string, Expr> counterexample = all_vars_zero;

        /*
        std::cout << "Candidate RHS:\n"
                  << simplify(simplify(substitute_in_all_lets(substitute(current_program, program)))) << "\n";
        */

        // Start with just random fuzzing. If that fails, we'll ask Z3 for a counterexample.
        int counterexamples_found_with_fuzzing = 0;
        for (int i = 0; i < 5; i++) {
            map<string, Expr> rand_binding = all_vars_zero;
            for (auto &it : rand_binding) {
                it.second = random_int(rng);
            }
            auto interpreted = simplify(substitute(rand_binding, ub_checker.safe && !current_program_works));
            if (is_one(interpreted)) {
                counterexamples.push_back(rand_binding);
                // We probably only want to add a couple
                // counterexamples at a time
                counterexamples_found_with_fuzzing++;
                if (counterexamples_found_with_fuzzing >= 2) {
                    break;
                }
            }
        }

        if (counterexamples_found_with_fuzzing == 0) {
            auto result = satisfy(ub_checker.safe && !current_program_works, &counterexample);
            if (result == Unsat) {
                // Woo!
                Expr e = simplify(substitute_in_all_lets(common_subexpression_elimination(substitute(current_program, program))));
                if (was_bool) {
                    e = simplify(reboolify(e));
                }
                // TODO: Figure out why I need to simplify twice
                // here. There are still exprs for which the
                // simplifier requires repeated applications, and
                // it's not supposed to.
                e = simplify(e);

                // std::cout << "*** Success: " << orig << " -> " << result << "\n\n";
                return e;
            } else if (result == Sat) {
                /*
                  std::cout << "Counterexample: ";
                  const char *prefix = "";
                  for (auto it : counterexample) {
                  std::cout << prefix << it.first << " = " << it.second;
                  prefix = ", ";
                  }
                  std::cout << "\n";
                */
                counterexamples.push_back(counterexample);
            } else {
                return Expr();
            }
        }

        // std::cout << "Counterexample found...\n";

        // Now synthesize a program that fits all the counterexamples
        Expr works_on_counterexamples = const_true();
        for (auto &c : counterexamples) {
            works_on_counterexamples = works_on_counterexamples && substitute(c, program_works);
        }
        if (satisfy(works_on_counterexamples, &current_program) != Sat) {
            // Failed to synthesize a program
            // std::cout << "Failed to find a program of size " << size << "\n";
            return Expr();
        }
        // We have a new program

        // If we start to have many many counterexamples, we should
        // double-check things are working as intended.
        if (counterexamples.size() > 30) {
            Expr sanity_check = simplify(substitute(current_program, works_on_counterexamples));
            // Might fail to be the constant true due to overflow, so just make sure it's not the constant false
            if (is_zero(sanity_check)) {
                Expr p = simplify(common_subexpression_elimination(substitute(current_program, program)));
                std::cout << "Synthesized program doesn't actually work on counterexamples!\n"
                          << "Original expr: " << e << "\n"
                          << "Program: " << p << "\n"
                          << "Check: " << sanity_check << "\n"
                          << "Counterexamples: \n";
                for (auto c : counterexamples) {
                    const char *prefix = "";
                    for (auto it : c) {
                        std::cout << prefix << it.first << " = " << it.second;
                        prefix = ", ";
                    }
                    std::cout << "\n";
                }
                abort();
            }
        }

        /*
        std::cout << "Current program:";
        for (const auto &o : symbolic_opcodes) {
            std::cout << " " << current_program[o.as<Variable>()->name];
        }
        std::cout << "\n";
        */
    }
}


// Use CEGIS to construct a sufficient condition for the given boolean
// argument. The condition must be true on at least the list of
// example cases given.
Expr synthesize_sufficient_condition(Expr lhs, Expr rhs, int size,
                                     vector<map<string, Expr>> positive_examples,
                                     map<string, Expr> *binding) {
    Expr orig = lhs == rhs;
    Expr e = select(lhs == rhs, 1, 0);

    vector<Expr> lhs_leaves, rhs_leaves;

    // Always require denominators are small positive constants, or
    // we're likely to stray outside of what z3 can handle.
    class BoundDenominators : public IRVisitor {
        void visit(const Div *op) override {
            IRVisitor::visit(op);
            result = result && (op->b > 0) && (op->b < 16);
        }
        void visit(const Mod *op) override {
            IRVisitor::visit(op);
            result = result && (op->b > 0) && (op->b < 16);
        }
    public:
        Expr result = const_true();
    } bound_denominators;
    lhs.accept(&bound_denominators);
    Expr denominators_bounded = simplify(bound_denominators.result);

    // Get the vars we're allowed to use in the predicate. Just use
    // the vars in the first positive example.
    map<string, Expr> all_vars_zero;
    assert(!positive_examples.empty());
    for (auto it : positive_examples[0]) {
        if (expr_uses_var(lhs, it.first)) {
            lhs_leaves.push_back(Variable::make(it.second.type(), it.first));
        } else {
            rhs_leaves.push_back(Variable::make(it.second.type(), it.first));
        }
        all_vars_zero[it.first] = make_zero(it.second.type());
    }

    // Identify the vars that cannot show up in the predicate.
    map<string, Expr> secondary_vars_are_zero;
    FindVars find_vars;
    orig.accept(&find_vars);
    for (auto v : find_vars.vars) {
        if (all_vars_zero.find(v.first) == all_vars_zero.end()) {
            secondary_vars_are_zero[v.first] = 0;
        }
    }

    // Maybe we'll get lucky and can just cancel all the secondary
    // vars to get an exact predicate. If that's the case we don't
    // need to synthesize anything.
    {
        Expr e = orig;
        for (auto it : secondary_vars_are_zero) {
            e = simplify(solve_expression(e, it.first).result);
        }
        bool eliminated = true;
        for (auto it : secondary_vars_are_zero) {
            if (expr_uses_var(e, it.first)) {
                eliminated = false;
            }
        }
        if (eliminated) {
            return e;
        }
    }

    // Example bindings of the predicate vars for which lhs != rhs
    vector<map<string, Expr>> negative_examples;

    // The current predicate, in terms of the opcodes used to describe
    // it. There's one opcode per predicate variable.
    map<string, Expr> current_predicate;

    vector<Expr> symbolic_opcodes, symbolic_opcodes_ref;
    for (size_t i = 0; i < lhs_leaves.size() + rhs_leaves.size(); i++) {
        Var op("op_" + std::to_string(i));
        symbolic_opcodes.push_back(op);
        Var op_ref("op_" + std::to_string(i) + "_ref");
        symbolic_opcodes_ref.push_back(op_ref);

        // The initial predicate is some garbage
        current_predicate[op.name()] = 0;
    }

    auto p = predicate_expr(lhs_leaves, rhs_leaves, symbolic_opcodes, symbolic_opcodes_ref, binding);
    Expr predicate = std::get<0>(p) && denominators_bounded;
    Expr predicate_valid = std::get<1>(p);
    Expr strictly_more_general_than_ref = std::get<2>(p);
    Expr false_positive = (predicate && lhs != rhs) && predicate_valid;
    Expr false_negative = (!predicate && lhs == rhs) && predicate_valid;
    Expr predicate_works = (!predicate || lhs == rhs) && predicate_valid;
    predicate = simplify(common_subexpression_elimination(predicate));
    predicate_valid = simplify(common_subexpression_elimination(predicate_valid));
    false_positive = simplify(common_subexpression_elimination(false_positive));
    false_negative = simplify(common_subexpression_elimination(false_negative));
    predicate_works = simplify(common_subexpression_elimination(predicate_works));
    strictly_more_general_than_ref = simplify(common_subexpression_elimination(strictly_more_general_than_ref));

    std::mt19937 rng(0);
    std::uniform_int_distribution<int> random_int(-256, 256);

    Expr most_general_predicate_found;
    map<string, Expr> most_general_predicate_opcodes;
    bool toggle = false;

    while (negative_examples.size() < 30) {
        // First synthesize a false positive for the current
        // predicate. This is a set of constants for which the
        // predicate is true, but the expression is false.

        /*
        Expr current_predicate_valid = simplify(substitute(current_predicate, predicate_valid));
        // The validity of the program should not depend on the args,
        // and we should only be synthesizing valid programs.
        if (!is_one(current_predicate_valid)) {
            std::cout << "Current predicate malformed: " << current_predicate_valid << "\n";
            abort();
        }
        */

        std::cout << "Candidate predicate: "
                  << simplify(substitute_in_all_lets(simplify(substitute(current_predicate, predicate)))) << "\n";

        Expr false_positive_for_current_predicate = simplify(substitute(current_predicate, false_positive));
        Expr false_negative_for_current_predicate = simplify(substitute(current_predicate, false_negative));
        map<string, Expr> negative_example = all_vars_zero;

        // Start with just random fuzzing. If that fails, we'll ask Z3 for a negative example.
        int negative_examples_found_with_fuzzing = 0;
        for (int i = 0; i < 5; i++) {
            map<string, Expr> rand_binding = all_vars_zero;
            for (auto &it : rand_binding) {
                it.second = random_int(rng);
            }
            auto interpreted = simplify(substitute(rand_binding, false_positive_for_current_predicate));

            if (!is_one(interpreted)) continue;

            for (auto it : rand_binding) {
                std::cout << it.first << " = " << it.second << "\n";
            }

            negative_examples.push_back(rand_binding);
            negative_examples_found_with_fuzzing++;
            break;
        }

        if (negative_examples_found_with_fuzzing == 0) {
            // std::cout << "Satisfying: " << false_positive_for_current_predicate << "\n";

            Z3Result result = Unsat;
            if (lhs.type().is_bool()) {
                // For boolean lhs/rhs, we get a better sampling of the
                // space of false positives if we try for cases where the
                // lhs is false and the rhs is true as often as cases
                // where the lhs is true and the rhs is false.
                if (toggle) {
                    result = satisfy(false_positive_for_current_predicate && !rhs, &negative_example);
                } else {
                    result = satisfy(false_positive_for_current_predicate && rhs, &negative_example);
                }
            } else {
                if (toggle) {
                    result = satisfy(false_positive_for_current_predicate && lhs < rhs, &negative_example);
                } else {
                    result = satisfy(false_positive_for_current_predicate && rhs < lhs , &negative_example);
                }
            }
            toggle = !toggle;
            if (result == Unsat) {
                result = satisfy(false_positive_for_current_predicate, &negative_example);
            }

            if (result == Unsat) {
                // Z3 says there are no false positives, which means
                // this predicate is good. Make a note of it and start
                // the hunt for a more general predicate.
                most_general_predicate_found = simplify(substitute_in_all_lets(simplify(substitute(current_predicate, predicate))));
                std::cout << "No false positives found\n";
                most_general_predicate_opcodes = current_predicate;
            } else if (result == Sat) {
                std::cout << "Found a new false positive\n";
                negative_examples.push_back(negative_example);
                /*
                for (auto it : negative_example) {
                    std::cout << it.first << " = " << it.second << "\n";
                }
                */
                std::cout << "Under this false positive, lhs = " << simplify(substitute(negative_example, lhs))
                          << " rhs = " << simplify(substitute(negative_example, rhs))
                          << "\n";
            } else {
                std::cout << "Search for false positives was inconclusive.\n";
                break;
            }
        }

        // Now synthesize the most general predicate that's false on
        // the negative examples and true on the positive
        // examples. We'll do it by synthesizing any predicate, then
        // iteratively trying to synthesize a strictly more general
        // one.
        Expr false_on_negative_examples = const_true();
        for (auto &c : negative_examples) {
            false_on_negative_examples = false_on_negative_examples && substitute(c, !predicate);
        }
        Expr true_on_positive_examples = const_true();
        for (const auto &m : positive_examples) {
            true_on_positive_examples = true_on_positive_examples && substitute(m, predicate);
        }

        std::cout << "Synthesizing new predicate using "
                  << positive_examples.size() << " positive examples and "
                  << negative_examples.size() << " negative examples\n";

        Expr cond = false_on_negative_examples && true_on_positive_examples && predicate_valid;
        if (satisfy(cond, &current_predicate) != Sat) {
            // Failed to synthesize a better predicate
            std::cout << "Failed to find a predicate that fits all the examples\n";
            break;
        }

        // Generalize it
        while (1) {
            map<string, Expr> reference_predicate;
            for (auto it : current_predicate) {
                reference_predicate[it.first + "_ref"] = it.second;
            }
            Expr more_general = substitute(reference_predicate, strictly_more_general_than_ref);
            auto r = satisfy(cond && more_general, &current_predicate);
            if (r == Sat) {
                // We found a simpler predicate.
                continue;
            } else {
                // We failed to generalize this predicate any
                // further. Hunt for new false positives.
                break;
            }
        }

        /*
        // Sanity check - does the predicate indeed fit all the
        // positive examples and none of the negative ones.
        {
            Expr p = substitute(current_predicate, predicate);
            for (auto &c : negative_examples) {
                Expr e = substitute(c, p);
                if (!is_zero(simplify(e))) {
                    std::cout << "Predicate was supposed to be false on negative example: " << e << "\n" << simplify(e) << "\n";
                    abort();
                }
            }
            for (const auto &c : positive_examples) {
                Expr e = substitute(c, p);
                if (!is_one(simplify(e))) {
                    std::cout << "Predicate was supposed to be true on positive example: " << e << "\n" << simplify(e) << "\n";
                    abort();
                }
            }
        }
        */

        if (most_general_predicate_found.defined()) {
            Expr current = simplify(simplify(substitute_in_all_lets(substitute(current_predicate, predicate))));
            if (can_prove(most_general_predicate_found == current)) {
                // We have hit a fixed-point.
                break;
            }
        }
    }

    // Construct an Expr for the value of each RHS variable.
    for (auto &it : *binding) {
        it.second = simplify(common_subexpression_elimination(substitute(most_general_predicate_opcodes, it.second)));
    }

    // TODO: Attempt to relax bounds on denominators

    return most_general_predicate_found;
}

// Enumerate all possible patterns that would match any portion of the
// given expression.
vector<Expr> all_possible_lhs_patterns(const Expr &e) {
    // Convert the expression to a DAG
    class DAGConverter : public IRMutator {
    public:

        using IRMutator::mutate;

        int current_parent = -1;

        Expr mutate(const Expr &e) override {
            if (building.empty()) {
                int current_id = (int)id_for_expr.size();
                auto it = id_for_expr.emplace(e, current_id);
                bool unseen = it.second;
                current_id = it.first->second;

                if (unseen) {
                    if (expr_for_id.size() < id_for_expr.size()) {
                        expr_for_id.resize(id_for_expr.size());
                        children.resize(id_for_expr.size());
                    }
                    expr_for_id[current_id] = e;
                    int old_parent = current_parent;
                    current_parent = current_id;
                    IRMutator::mutate(e);
                    current_parent = old_parent;
                }

                if (current_parent != -1) {
                    children[current_parent].insert(current_id);
                }

                return e;
            } else {
                // Building a subexpr
                auto it = id_for_expr.find(e);
                assert(it != id_for_expr.end());
                if (building.count(it->second)) {
                    return IRMutator::mutate(e);
                } else {
                    int new_id = (int)renumbering.size();
                    new_id = renumbering.emplace(it->second, new_id).first->second;
                    // We're after end
                    const char *names[] = {"x", "y", "z", "w", "u", "v"};
                    string name = "v" + std::to_string(new_id);
                    if (new_id >= 0 && new_id < 6) {
                        name = names[new_id];
                    }
                    return Variable::make(e.type(), name);
                }
            }
        }

        // Map between exprs and node ids
        map<Expr, int, IRDeepCompare> id_for_expr;
        vector<Expr> expr_for_id;
        // The DAG structure. Every node has outgoing edges (child
        // nodes) and incoming edges (parent nodes).
        vector<set<int>> children;

        // The current expression being built
        set<int> building;
        map<int, int> renumbering;

        bool may_add_to_frontier(const set<int> &rejected, const set<int> &current, int n) {
            if (rejected.count(n)) return false;
            if (current.count(n)) return false;
            if (expr_for_id[n].as<Variable>()) return false;
            return true;
        }

        vector<Expr> result;

        // Generate all subgraphs of a directed graph
        void generate_subgraphs(const set<int> &rejected,
                                const set<int> &current,
                                const set<int> &frontier)  {
            // Pick an arbitrary frontier node to consider
            int v = -1;
            for (auto n : frontier) {
                if (may_add_to_frontier(rejected, current, n)) {
                    v = n;
                    break;
                }
            }

            if (v == -1) {
                if (!current.empty()) {
                    building = current;
                    renumbering.clear();
                    Expr pat = mutate(expr_for_id[*(building.begin())]);
                    // Apply some rejection rules
                    if (building.size() <= 1 || renumbering.size() > 6) {
                        // Too few inner nodes or too many wildcards
                    } else {
                        result.push_back(pat);
                    }
                }
                return;
            }

            const set<int> &ch = children[v];

            set<int> r = rejected, c = current, f = frontier;

            f.erase(v);

            bool must_include = false; //is_const(expr_for_id[v]);
            bool may_include = true; //!is_const(expr_for_id[v]);
            if (!must_include) {
                // Generate all subgraphs with this frontier node not
                // included (replaced with a variable).
                r.insert(v);

                // std::cout << "Excluding " << expr_for_id[v] << "\n";
                generate_subgraphs(r, c, f);
            }

            // Generate all subgraphs with this frontier node included
            if (may_include && (must_include || c.size() < 10)) { // Max out at some number of unique nodes
                c.insert(v);
                for (auto n : ch) {
                    if (may_add_to_frontier(rejected, current, n)) {
                        f.insert(n);
                    }
                }
                // std::cout << "Including " << expr_for_id[v] << "\n";
                generate_subgraphs(rejected, c, f);
            }
        }
    } all_subexprs;

    all_subexprs.mutate(e);

    // Enumerate all sub-dags
    set<int> rejected, current, frontier;
    frontier.insert(0);
    for (int i = 0; i < (int)all_subexprs.children.size(); i++) {
        // Don't consider leaves for roots. We can't simplify "x" or
        // "3".
        if (all_subexprs.children[i].empty()) continue;
        frontier.insert(i);
        all_subexprs.generate_subgraphs(rejected, current, frontier);
        frontier.clear();
    }

    return all_subexprs.result;
}

// Does expr a describe a pattern that expr b would match. For example
// more_general_than(x + y, (x*3) + y) returns true.
bool more_general_than(const Expr &a, const Expr &b, map<string, Expr> &bindings);

template<typename Op>
bool more_general_than(const Expr &a, const Op *b, map<string, Expr> &bindings) {
    map<string, Expr> backup = bindings;
    if (more_general_than(a, b->a, bindings)) {
        return true;
    }
    bindings = backup;

    if (more_general_than(a, b->b, bindings)) {
        return true;
    }
    bindings = backup;

    if (const Op *op_a = a.as<Op>()) {
        return (more_general_than(op_a->a, b->a, bindings) &&
                more_general_than(op_a->b, b->b, bindings));
    }
    return false;

}

bool more_general_than(const Expr &a, const Expr &b, map<string, Expr> &bindings) {
    if (const Variable *var = a.as<Variable>()) {
        const Variable *var_b = b.as<Variable>();
        auto it = bindings.find(var->name);
        if (it != bindings.end()) {
            return equal(it->second, b);
        } else {
            bool const_wild = var->name[0] == 'c';
            bool b_const_wild = var_b && (var_b->name[0] == 'c');
            bool b_const = is_const(b);
            bool may_bind = !const_wild || (const_wild && (b_const_wild || b_const));
            if (may_bind) {
                bindings[var->name] = b;
                return true;
            } else {
                return false;
            }
        }
    }

    if (is_const(a) && is_const(b)) {
        return equal(a, b);
    }

    if (const Min *op = b.as<Min>()) {
        return more_general_than(a, op, bindings);
    }

    if (const Min *op = b.as<Min>()) {
        return more_general_than(a, op, bindings);
    }

    if (const Max *op = b.as<Max>()) {
        return more_general_than(a, op, bindings);
    }

    if (const Add *op = b.as<Add>()) {
        return more_general_than(a, op, bindings);
    }

    if (const Sub *op = b.as<Sub>()) {
        return more_general_than(a, op, bindings);
    }

    if (const Mul *op = b.as<Mul>()) {
        return more_general_than(a, op, bindings);
    }

    if (const Div *op = b.as<Div>()) {
        return more_general_than(a, op, bindings);
    }

    if (const LE *op = b.as<LE>()) {
        return more_general_than(a, op, bindings);
    }

    if (const LT *op = b.as<LT>()) {
        return more_general_than(a, op, bindings);
    }

    if (const EQ *op = b.as<EQ>()) {
        return more_general_than(a, op, bindings);
    }

    if (const NE *op = b.as<NE>()) {
        return more_general_than(a, op, bindings);
    }

    if (const Not *op = b.as<Not>()) {
        map<string, Expr> backup = bindings;
        if (more_general_than(a, op->a, bindings)) {
            return true;
        }
        bindings = backup;

        const Not *op_a = a.as<Not>();
        return (op_a &&
                more_general_than(op_a->a, op->a, bindings));
    }

    if (const Select *op = b.as<Select>()) {
        map<string, Expr> backup = bindings;
        if (more_general_than(a, op->condition, bindings)) {
            return true;
        }
        bindings = backup;

        if (more_general_than(a, op->true_value, bindings)) {
            return true;
        }
        bindings = backup;

        if (more_general_than(a, op->false_value, bindings)) {
            return true;
        }
        bindings = backup;

        const Select *op_a = a.as<Select>();
        return (op_a &&
                more_general_than(op_a->condition, op->condition, bindings) &&
                more_general_than(op_a->true_value, op->true_value, bindings) &&
                more_general_than(op_a->false_value, op->false_value, bindings));
    }

    return false;
}

bool more_general_than(const Expr &a, const Expr &b) {
    map<string, Expr> bindings;
    return more_general_than(a, b, bindings);
}


// Compute some basic information about an Expr: op counts, variables
// used, etc.
class CountOps : public IRGraphVisitor {
    using IRGraphVisitor::visit;
    using IRGraphVisitor::include;

    void visit(const Variable *op) override {
        if (op->type != Int(32)) {
            has_unsupported_ir = true;
        } else if (vars_used.count(op->name)) {
            has_repeated_var = true;
        } else {
            vars_used.insert(op->name);
        }
    }

    void visit(const Div *op) override {
        has_div_mod = true;
        IRGraphVisitor::visit(op);
    }

    void visit(const Mod *op) override {
        has_div_mod = true;
        IRGraphVisitor::visit(op);
    }

    void visit(const Call *op) override {
        has_unsupported_ir = true;
    }

    void visit(const Cast *op) override {
        has_unsupported_ir = true;
    }

    void visit(const Load *op) override {
        has_unsupported_ir = true;
    }

    set<Expr, IRDeepCompare> unique_exprs;

public:

    void include(const Expr &e) override {
        if (is_const(e)) {
            num_constants++;
        } else {
            unique_exprs.insert(e);
            IRGraphVisitor::include(e);
        }
    }

    int count() {
        return unique_exprs.size() - (int)vars_used.size();
    }

    int num_constants = 0;

    bool has_div_mod = false;
    bool has_unsupported_ir = false;
    bool has_repeated_var = false;
    set<string> vars_used;
};

Expr parse_halide_expr(char **cursor, char *end, Type expected_type) {
    consume_whitespace(cursor, end);

    struct TypePattern {
        const char *cast_prefix = nullptr;
        const char *constant_prefix = nullptr;
        Type type;
        string cast_prefix_storage, constant_prefix_storage;
        TypePattern(Type t) {
            ostringstream cast_prefix_stream, constant_prefix_stream;
            cast_prefix_stream << t << '(';
            cast_prefix_storage = cast_prefix_stream.str();
            cast_prefix = cast_prefix_storage.c_str();

            constant_prefix_stream << '(' << t << ')';
            constant_prefix_storage = constant_prefix_stream.str();
            constant_prefix = constant_prefix_storage.c_str();
            type = t;
        }
    };

    static TypePattern typenames[] = {
        {UInt(1)},
        {Int(8)},
        {UInt(8)},
        {Int(16)},
        {UInt(16)},
        {Int(32)},
        {UInt(32)},
        {Int(64)},
        {UInt(64)},
        {Float(64)},
        {Float(32)}};
    for (auto t : typenames) {
        if (consume(cursor, end, t.cast_prefix)) {
            Expr a = cast(t.type, parse_halide_expr(cursor, end, Type{}));
            expect(cursor, end, ")");
            return a;
        }
        if (consume(cursor, end, t.constant_prefix)) {
            return make_const(t.type, consume_int(cursor, end));
        }
    }
    if (consume(cursor, end, "(let ")) {
        string name = consume_token(cursor, end);
        consume_whitespace(cursor, end);
        expect(cursor, end, "=");
        consume_whitespace(cursor, end);

        Expr value = parse_halide_expr(cursor, end, Type{});

        consume_whitespace(cursor, end);
        expect(cursor, end, "in");
        consume_whitespace(cursor, end);

        Expr body = parse_halide_expr(cursor, end, expected_type);

        Expr a = Let::make(name, value, body);
        expect(cursor, end, ")");
        return a;
    }
    if (consume(cursor, end, "min(")) {
        Expr a = parse_halide_expr(cursor, end, expected_type);
        expect(cursor, end, ",");
        Expr b = parse_halide_expr(cursor, end, expected_type);
        consume_whitespace(cursor, end);
        expect(cursor, end, ")");
        return min(a, b);
    }
    if (consume(cursor, end, "max(")) {
        Expr a = parse_halide_expr(cursor, end, expected_type);
        expect(cursor, end, ",");
        Expr b = parse_halide_expr(cursor, end, expected_type);
        consume_whitespace(cursor, end);
        expect(cursor, end, ")");
        return max(a, b);
    }
    if (consume(cursor, end, "select(")) {
        Expr a = parse_halide_expr(cursor, end, Bool());
        expect(cursor, end, ",");
        Expr b = parse_halide_expr(cursor, end, expected_type);
        expect(cursor, end, ",");
        Expr c = parse_halide_expr(cursor, end, expected_type);
        consume_whitespace(cursor, end);
        expect(cursor, end, ")");
        return select(a, b, c);
    }
    Call::ConstString binary_intrinsics[] =
        {Call::bitwise_and,
         Call::bitwise_or,
         Call::shift_left,
         Call::shift_right};
    for (auto intrin : binary_intrinsics) {
        if (consume(cursor, end, intrin)) {
            expect(cursor, end, "(");
            Expr a = parse_halide_expr(cursor, end, expected_type);
            expect(cursor, end, ",");
            Expr b = parse_halide_expr(cursor, end, expected_type);
            consume_whitespace(cursor, end);
            expect(cursor, end, ")");
            return Call::make(a.type(), intrin, {a, b}, Call::PureIntrinsic);
        }
    }

    if (consume(cursor, end, "round_f32(")) {
        Expr a = parse_halide_expr(cursor, end, Float(32));
        expect(cursor, end, ")");
        return round(a);
    }
    if (consume(cursor, end, "ceil_f32(")) {
        Expr a = parse_halide_expr(cursor, end, Float(32));
        expect(cursor, end, ")");
        return ceil(a);
    }
    if (consume(cursor, end, "floor_f32(")) {
        Expr a = parse_halide_expr(cursor, end, Float(32));
        expect(cursor, end, ")");
        return floor(a);
    }
    if (consume(cursor, end, "likely(")) {
        Expr a = parse_halide_expr(cursor, end, expected_type);
        expect(cursor, end, ")");
        return likely(a);
    }
    if (consume(cursor, end, "likely_if_innermost(")) {
        Expr a = parse_halide_expr(cursor, end, expected_type);
        expect(cursor, end, ")");
        return likely(a);
    }

    if (consume(cursor, end, "(")) {
        Expr a = parse_halide_expr(cursor, end, Type{});
        Expr result;
        consume_whitespace(cursor, end);
        if (consume(cursor, end, "+")) {
            result = a + parse_halide_expr(cursor, end, expected_type);
        }
        if (consume(cursor, end, "*")) {
            result = a * parse_halide_expr(cursor, end, expected_type);
        }
        if (consume(cursor, end, "-")) {
            result = a - parse_halide_expr(cursor, end, expected_type);
        }
        if (consume(cursor, end, "/")) {
            result = a / parse_halide_expr(cursor, end, expected_type);
        }
        if (consume(cursor, end, "%")) {
            result = a % parse_halide_expr(cursor, end, expected_type);
        }
        if (consume(cursor, end, "<=")) {
            result = a <= parse_halide_expr(cursor, end, Type{});
        }
        if (consume(cursor, end, "<")) {
            result = a < parse_halide_expr(cursor, end, Type{});
        }
        if (consume(cursor, end, ">=")) {
            result = a >= parse_halide_expr(cursor, end, Type{});
        }
        if (consume(cursor, end, ">")) {
            result = a > parse_halide_expr(cursor, end, Type{});
        }
        if (consume(cursor, end, "==")) {
            result = a == parse_halide_expr(cursor, end, Type{});
        }
        if (consume(cursor, end, "!=")) {
            result = a != parse_halide_expr(cursor, end, Type{});
        }
        if (consume(cursor, end, "&&")) {
            result = a && parse_halide_expr(cursor, end, Bool());
        }
        if (consume(cursor, end, "||")) {
            result = a || parse_halide_expr(cursor, end, Bool());
        }
        if (result.defined()) {
            consume_whitespace(cursor, end);
            expect(cursor, end, ")");
            return result;
        }
    }
    if (consume(cursor, end, "v")) {
        if (expected_type == Type{}) {
            expected_type = Int(32);
        }
        Expr a = Variable::make(expected_type, "v" + std::to_string(consume_int(cursor, end)));
        return a;
    }
    if ((**cursor >= '0' && **cursor <= '9') || **cursor == '-') {
        Expr e = make_const(Int(32), consume_int(cursor, end));
        if (**cursor == '.') {
            e += consume_float(cursor, end);
        }
        return e;
    }
    if (consume(cursor, end, "true")) {
        return const_true();
    }
    if (consume(cursor, end, "false")) {
        return const_false();
    }
    if (consume(cursor, end, "!")) {
        return !parse_halide_expr(cursor, end, Bool());
    }

    if ((**cursor >= 'a' && **cursor <= 'z') || **cursor == '.') {
        char **tmp = cursor;
        string name = consume_token(tmp, end);
        if (consume(tmp, end, "[")) {
            *cursor = *tmp;
            Expr index = parse_halide_expr(cursor, end, Int(32));
            expect(cursor, end, "]");
            if (expected_type == Type{}) {
                expected_type = Int(32);
            }
            return Load::make(expected_type, name, index, Buffer<>(), Parameter(), const_true(), ModulusRemainder());
        } else {
            if (expected_type == Type{}) {
                expected_type = Int(32);
            }
            return Variable::make(expected_type, name);
        }
    }

    std::cerr << "Failed to parse Halide Expr starting at " << *cursor << "\n";
    abort();
    return Expr();
}

// Replace all integer constants with wildcards
class ReplaceConstants : public IRMutator {
    using IRMutator::visit;
    Expr visit(const IntImm *op) override {
        auto it = bound_values.find(op->value);
        // Assume repeated instance of the same var is the same
        // wildcard var. If we have rules where that isn't true we'll
        // need to see examples where the values differ.
        if (it == bound_values.end()) {
            string name = "c" + std::to_string(counter++);
            binding[name] = op;
            Expr v = Variable::make(op->type, name);
            bound_values[op->value] = v;
            return v;
        } else {
            return it->second;
        }
    }
    Expr visit(const Variable *op) override {
        free_vars.insert(op->name);
        return op;
    }

    map<int64_t, Expr> bound_values;
    // TODO: float constants

public:
    int counter = 0;
    map<string, Expr> binding;
    set<string> free_vars;
};

enum class Dir {Up, Down};
Dir flip(Dir d) {
    if (d == Dir::Up) {
        return Dir::Down;
    } else {
        return Dir::Up;
    }
}

// Try to remove divisions from an expression, possibly by making it
// larger or smaller by a small amount, depending on the direction
// argument.
Expr simplify_with_slop(Expr e, Dir d) {
    if (const LE *le = e.as<LE>()) {
        Expr a = le->a, b = le->b;
        const Div *div = a.as<Div>();
        if (!div) div = b.as<Div>();
        if (div && is_one(simplify(div->b > 0))) {
            a *= div->b;
            b *= div->b;
        }
        a = simplify(a);
        b = simplify(b);
        return simplify_with_slop(a, flip(d)) <= simplify_with_slop(b, d);
    }
    if (const LT *lt = e.as<LT>()) {
        Expr a = lt->a, b = lt->b;
        const Div *div = a.as<Div>();
        if (!div) div = b.as<Div>();
        if (div && is_one(simplify(div->b > 0))) {
            a *= div->b;
            b *= div->b;
        }
        a = simplify(a);
        b = simplify(b);
        return simplify_with_slop(a, flip(d)) < simplify_with_slop(b, d);
    }
    if (const And *a = e.as<And>()) {
        return simplify_with_slop(a->a, d) && simplify_with_slop(a->b, d);
    }
    if (const Or *o = e.as<Or>()) {
        return simplify_with_slop(o->a, d) || simplify_with_slop(o->b, d);
    }
    if (const Select *s = e.as<Select>()) {
        return select(s->condition, simplify_with_slop(s->true_value, d), simplify_with_slop(s->false_value, d));
    }
    if (const Min *m = e.as<Min>()) {
        return min(simplify_with_slop(m->a, d), simplify_with_slop(m->b, d));
    }
    if (const Max *m = e.as<Max>()) {
        return max(simplify_with_slop(m->a, d), simplify_with_slop(m->b, d));
    }
    if (const Min *m = e.as<Min>()) {
        return min(simplify_with_slop(m->a, d), simplify_with_slop(m->b, d));
    }
    if (const Add *a = e.as<Add>()) {
        return simplify_with_slop(a->a, d) + simplify_with_slop(a->b, d);
    }
    if (const Sub *s = e.as<Sub>()) {
        return simplify_with_slop(s->a, d) - simplify_with_slop(s->b, flip(d));
    }
    if (const Mul *m = e.as<Mul>()) {
        if (is_const(m->b)) {
            if (const Div *div = m->a.as<Div>()) {
                if (is_zero(simplify(m->b % div->b))) {
                    // (x / 3) * 6
                    // -> ((x / 3) * 3) * 2
                    // -> (x + 2) * 2 or x * 2 depending on direction
                    // This is currently the only place slop is injected
                    Expr num = div->a;
                    if (d == Dir::Down) {
                        num -= div->b - 1;
                    }
                    return num * (m->b / div->b);
                }
            }

            if (can_prove(m->b > 0)) {
                return simplify_with_slop(m->a, d) * m->b;
            } else {
                return simplify_with_slop(m->a, flip(d)) * m->b;
            }
        }
        if (const Div *div = m->a.as<Div>()) {
            if (equal(div->b, m->b)) {
                // (x / y) * y
                Expr num = div->a;
                if (d == Dir::Down) {
                    num -= div->b - 1;
                }
                return num * (m->b / div->b);
            }
        }
    }
    if (const Div *div = e.as<Div>()) {
        if (is_const(div->b)) {
            if (can_prove(div->b > 0)) {
                return simplify_with_slop(div->a, d) / div->b;
            } else {
                return simplify_with_slop(div->a, flip(d)) / div->b;
            }
        }
    }
    return e;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cout << "Usage: ./super_simplify halide_exprs.txt\n";
        return 0;
    }

    // Generate LHS patterns from raw exprs
    vector<Expr> exprs;
    std::cout << "Reading expressions from file\n";
    std::ifstream input;
    input.open(argv[1]);
    for (string line; std::getline(input, line);) {
        if (line.empty()) continue;
        // It's possible to comment out lines for debugging
        if (line[0] == '#') continue;

        // There are some extraneous newlines in some of the files. Balance parentheses...
        size_t open, close;
        while (1) {
            open = std::count(line.begin(), line.end(), '(');
            close = std::count(line.begin(), line.end(), ')');
            if (open == close) break;
            string next;
            assert(std::getline(input, next));
            line += next;
        }

        std::cout << "Parsing expression: '" << line << "'\n";
        char *start = &line[0];
        char *end = &line[line.size()];
        exprs.push_back(parse_halide_expr(&start, end, Type{}));
    }

    // Try to load a blacklist of patterns to skip over that are known
    // to fail. Delete the blacklist whenever you make a change that
    // might make things work for more expressions.
    set<Expr, IRDeepCompare> blacklist;
    if (file_exists("blacklist.txt")) {
        std::cout << "Loading pattern blacklist\n";
        std::ifstream b;
        b.open("blacklist.txt");
        for (string line; std::getline(b, line);) {
            char *start = &line[0];
            char *end = &line[line.size()];
            blacklist.insert(parse_halide_expr(&start, end, Type{}));
        }
    }

    std::cout << blacklist.size() << " blacklisted patterns\n";

    map<Expr, int, IRDeepCompare> patterns_without_constants;

    set<Expr, IRDeepCompare> patterns;
    size_t handled = 0, total = 0;
    for (auto &e : exprs) {
        e = substitute_in_all_lets(e);
        Expr orig = e;
        e = simplify(e);
        Expr second = simplify(e);
        while (!equal(e, second)) {
            std::cerr << "Warning: Expression required multiple applications of the simplifier:\n"
                      << e << " -> " << second << "\n";
            e = second;
            second = simplify(e);
        }
        std::cout << "Simplified: " << e << "\n";
        total++;
        if (is_one(e)) {
            handled++;
        } else {
            {
                ReplaceConstants replacer;
                int count = patterns_without_constants[replacer.mutate(e)]++;
                // We don't want tons of exprs that are the same except for different constants
                if (count > 10) {
                    std::cout << "Skipping. Already seen it too many times\n";
                    continue;
                }
            }

            for (auto p : all_possible_lhs_patterns(e)) {
                // We prefer LT rules to LE rules. The LE simplifier just redirects to the LT simplifier.
                /*
                  if (const LE *le = p.as<LE>()) {
                  p = le->b < le->a;
                  }
                */

                if (!blacklist.count(p) && !patterns.count(p)) {
                    ReplaceConstants replacer;
                    int count = patterns_without_constants[replacer.mutate(p)]++;
                    if (count < 10) {
                        // We don't need tons of examples of the same
                        // rule with different constants.
                        patterns.insert(p);
                    }
                }
            }
        }
    }

    std::cout << patterns.size() << " candidate lhs patterns generated \n";

    std::cout << handled << " / " << total << " rules already simplify to true\n";

    // Generate rules from patterns
    vector<std::future<void>> futures;
    ThreadPool<void> pool;
    std::mutex mutex;
    vector<pair<Expr, Expr>> rules;
    int done = 0;

    {
        std::lock_guard<std::mutex> lock(mutex);
        for (int lhs_ops = 1; lhs_ops < 8; lhs_ops++) {
            for (auto p : patterns) {
                CountOps count_ops;
                count_ops.include(p);

                if (count_ops.count() != lhs_ops ||
                    count_ops.has_unsupported_ir ||
                    !(count_ops.has_repeated_var ||
                      count_ops.num_constants > 1)) {
                    continue;
                }

                std::cout << "PATTERN " << lhs_ops << " : " << p << "\n";
                futures.emplace_back(pool.async([=, &mutex, &rules, &futures, &done]() {
                            Expr e;
                            for (int budget = 0; !e.defined() && budget < lhs_ops; budget++) {
                                e = super_simplify(p, budget);
                            }
                            bool success = false;
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                if (e.defined()) {
                                    bool suppressed = false;
                                    for (auto &r : rules) {
                                        if (more_general_than(r.first, p)) {
                                            std::cout << "Ignoring specialization of earlier rule\n";
                                            suppressed = true;
                                            break;
                                        }
                                        if (more_general_than(p, r.first)) {
                                            std::cout << "Replacing earlier rule with this more general form:\n"
                                                      << "{" << p << ", " << e << "},\n";
                                            r.first = p;
                                            r.second = e;
                                            suppressed = true;
                                            break;
                                        }
                                    }
                                    if (!suppressed) {
                                        std::cout << "RULE: " << p << " = " << e << "\n";
                                        rules.emplace_back(p, e);
                                        success = true;
                                    }
                                }
                                done++;
                                if (done % 100 == 0) {
                                    std::cout << done << " / " << futures.size() << "\n";
                                }
                                if (!success) {
                                    // Add it to the blacklist so we
                                    // don't waste time on this
                                    // pattern again. Delete the
                                    // blacklist whenever you make a
                                    // change that might make things
                                    // work for new patterns.
                                    std::ofstream b;
                                    b.open("blacklist.txt", std::ofstream::out | std::ofstream::app);
                                    b << p << "\n";
                                }
                            }
                        }));
            }
        }
    }

    for (auto &f : futures) {
        f.get();
    }

    // Filter rules, though specialization should not have snuck through the filtering above
    vector<pair<Expr, Expr>> filtered;

    for (auto r1 : rules) {
        bool duplicate = false;
        pair<Expr, Expr> suppressed_by;
        for (auto r2 : rules) {
            bool g = more_general_than(r2.first, r1.first) && !equal(r1.first, r2.first);
            if (g) {
                suppressed_by = r2;
            }
            duplicate |= g;
        }
        if (!duplicate) {
            filtered.push_back(r1);
        } else {
            // std::cout << "This LHS: " << r1.first << " was suppressed by this LHS: " << suppressed_by.first << "\n";
        }
    }

    std::sort(filtered.begin(), filtered.end(), [](const pair<Expr, Expr> &r1, const pair<Expr, Expr> &r2) {
            return IRDeepCompare{}(r1.first, r2.first);
        });

    // Now try to generalize rules involving constants by replacing constants with wildcards and synthesizing a predicate.

    vector<tuple<Expr, Expr, Expr>> predicated_rules;
    vector<pair<Expr, Expr>> failed_predicated_rules;

    // Abstract away the constants and cluster the rules by LHS structure
    map<Expr, vector<map<string, Expr>>, IRDeepCompare> generalized;

    for (auto r : filtered) {
        std::cout << "Trying to generalize " << r.first << " -> " << r.second << "\n";
        Expr orig = r.first == r.second;
        ReplaceConstants replacer;
        r.first = replacer.mutate(r.first);
        r.second = replacer.mutate(r.second);
        std::cout << "Generalized LHS: " << r.first << "\n";
        if (replacer.counter == 0) {
            // No need to generalize this one
            predicated_rules.emplace_back(r.first, r.second, const_true());
        } else {
            generalized[r.first == r.second].emplace_back(std::move(replacer.binding));
        }
    }

    futures.clear();

    for (auto it : generalized) {
        futures.emplace_back(pool.async([=, &mutex, &predicated_rules, &failed_predicated_rules]() {
                    const EQ *eq = it.first.as<EQ>();
                    assert(eq);
                    Expr lhs = eq->a, rhs = eq->b;
                    map<string, Expr> binding;
                    Expr predicate = synthesize_sufficient_condition(lhs, rhs, 0, it.second, &binding);
                    if (!predicate.defined()) {
                        // Attempt to simplify lhs == rhs in a way
                        // that makes it less frequently true and deduce a
                        // predicate for that instead.
                        Expr new_lhs;
                        if (lhs.type().is_bool()) {
                            if (is_one(rhs)) {
                                new_lhs = simplify(simplify_with_slop(lhs, Dir::Down));
                            } else if (is_zero(rhs)) {
                                new_lhs = simplify(simplify_with_slop(lhs, Dir::Up));
                            }
                        }
                        if (new_lhs.defined() && !is_zero(new_lhs) && !equal(new_lhs, simplify(lhs))) {
                            std::cout << "Lossily simplified lhs: " << lhs << " -> " << new_lhs << "\n";
                            // Try again
                            predicate = synthesize_sufficient_condition(new_lhs, rhs, 0, it.second, &binding);
                        }
                    }

                    if (!predicate.defined()) {
                        std::lock_guard<std::mutex> lock(mutex);
                        failed_predicated_rules.emplace_back(lhs, rhs);
                        return;
                    }

                    // Mine the predicate for LHS var ==
                    // constant/other LHS var and move those
                    // constraints into the binding instead. Also
                    // de-dup terms in the predicate.
                    vector<Expr> pending = {simplify(predicate)};
                    set<Expr, IRDeepCompare> simpler_predicate;
                    while (!pending.empty()) {
                        Expr next = pending.back();
                        pending.pop_back();
                        if (const And *a = next.as<And>()) {
                            pending.push_back(a->a);
                            pending.push_back(a->b);
                            continue;
                        }

                        if (const EQ *e = next.as<EQ>()) {
                            Expr a = e->a, b = e->b;
                            const Variable *var_a = a.as<Variable>();
                            const Variable *var_b = b.as<Variable>();
                            if (var_a && var_b) {
                                // We want the lower-numbered vars on
                                // the right, so that we replaced
                                // things like c2 with things like c0,
                                // and not vice-versa. The simplifier
                                // does the opposite. So if they're
                                // both vars, just flip 'em.
                                std::swap(a, b);
                                std::swap(var_a, var_b);
                            }
                            // We only want LHS vars
                            if (var_a && !expr_uses_var(lhs, var_a->name)) {
                                var_a = nullptr;
                            }
                            if (var_b && !expr_uses_var(lhs, var_b->name)) {
                                var_b = nullptr;
                            }
                            if (var_a && (var_b || is_const(b))) {
                                for (auto &it : binding) {
                                    it.second = substitute(var_a->name, b, it.second);
                                }
                                binding[var_a->name] = b;
                                continue;
                            }
                        }

                        simpler_predicate.insert(next);
                    }

                    predicate = const_true();
                    for (auto &t : simpler_predicate) {
                        predicate = predicate && t;
                    }

                    predicate = simplify(substitute(binding, predicate));
                    predicate = simplify(substitute_in_all_lets(predicate));
                    lhs = substitute(binding, lhs);

                    // In the RHS, we want to wrap fold() around computed combinations of the constants
                    for (auto &it : binding) {
                        if (!is_const(it.second) && !it.second.as<Variable>()) {
                            it.second = Call::make(it.second.type(), "fold", {it.second}, Call::PureExtern);
                        }
                    }

                    rhs = substitute(binding, rhs);

                    // After doing the substitution we might be able
                    // to statically fold (e.g. we may get c0 + 0).
                    class SimplifyFolds : public IRMutator {
                        using IRMutator::visit;

                        Expr visit(const Call *op) override {
                            if (op->name == "fold") {
                                Expr e = simplify(op->args[0]);
                                if (is_const(e) || e.as<Variable>()) {
                                    return e;
                                } else {
                                    return Call::make(op->type, "fold", {e}, Call::PureExtern);
                                }
                            } else {
                                return IRMutator::visit(op);
                            }
                        }
                    } simplify_folds;
                    rhs = simplify_folds.mutate(rhs);

                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        predicated_rules.emplace_back(lhs, rhs, predicate);
                        std::cout << "PREDICATED RULE: " << predicate << " => " << lhs << " = " << rhs << "\n";
                    }
                }));
    }

    for (auto &f : futures) {
        f.get();
    }

    for (auto r : failed_predicated_rules) {
        std::cout << "Failed to synthesize a predicate for rule: "
                  << r.first << " -> " << r.second
                  << " from these instances:\n";
        Expr eq = r.first == r.second;
        const vector<map<string, Expr>> &examples = generalized[eq];
        for (const auto &e : examples) {
            std::cout << "FAILED: " << substitute(e, eq) << "\n";
        }
    }

    // Filter again, now that constants are gone.
    vector<tuple<Expr, Expr, Expr>> predicated_filtered;

    for (auto r1 : predicated_rules) {
        bool duplicate = false;
        tuple<Expr, Expr, Expr> suppressed_by;
        Expr lhs1 = std::get<0>(r1);
        for (auto r2 : predicated_rules) {
            Expr lhs2 = std::get<0>(r2);
            bool g = more_general_than(lhs2, lhs1) && !equal(lhs1, lhs2);
            if (g) {
                suppressed_by = r2;
            }
            duplicate |= g;
        }
        if (!duplicate) {
            predicated_filtered.push_back(r1);
        } else {
            // std::cout << "This LHS: " << r1.first << " was suppressed by this LHS: " << suppressed_by.first << "\n";
        }
    }

    std::sort(predicated_filtered.begin(), predicated_filtered.end(),
              [](const tuple<Expr, Expr, Expr> &r1, const tuple<Expr, Expr, Expr> &r2) {
                  return IRDeepCompare{}(std::get<0>(r1), std::get<0>(r2));
              });

    IRNodeType old = IRNodeType::IntImm;
    for (auto r : predicated_filtered) {
        Expr lhs = std::get<0>(r);
        Expr rhs = std::get<1>(r);
        Expr predicate = std::get<2>(r);
        IRNodeType t = lhs.node_type();
        if (t != old) {
            std::cout << t << ":\n";
            old = t;
        }
        if (is_one(predicate)) {
            std::cout << "    rewrite(" << lhs << ", " << rhs << ") ||\n";
        } else {
            std::cout << "    rewrite(" << lhs << ", " << rhs << ", " << predicate << ") ||\n";
        }
    }


    return 0;
}
