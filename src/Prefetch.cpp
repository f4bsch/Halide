#include <algorithm>
#include <map>
#include <string>
#include <limits>

#include "Prefetch.h"
#include "CodeGen_Internal.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Bounds.h"
#include "Scope.h"
#include "Simplify.h"
#include "Substitute.h"
#include "Util.h"

namespace Halide {
namespace Internal {

using std::map;
using std::string;
using std::vector;
using std::stack;

class InjectPrefetch : public IRMutator {
public:
    InjectPrefetch(const map<string, Function> &e)
        : env(e) { }
private:
    const map<string, Function> &env;
    Scope<Interval> scope;

private:
    using IRMutator::visit;

    // Strip down the tuple name, e.g. f.*.var into f
    string tuple_func(const string &name) {
        vector<string> v = split_string(name, ".");
        internal_assert(v.size() > 0);
        return v[0];
    }

    // Strip down the tuple name, e.g. f.*.var into var
    string tuple_var(const string &name) {
        vector<string> v = split_string(name, ".");
        internal_assert(v.size() > 0);
        return v[v.size()-1];
    }

    Function get_func(const string &name) {
        map<string, Function>::const_iterator iter = env.find(name);
        internal_assert(iter != env.end()) << "Realize node refers to function not in environment.\n";
        return iter->second;
    }

    void visit(const LetStmt *op) {
        Interval in = bounds_of_expr_in_scope(op->value, scope);
        scope.push(op->name, in);
        op->body.accept(this);
        scope.pop(op->name);
    }

    void visit(const For *op) {
        // At this stage of lowering, loop_min and loop_max
        // conveniently exist in scope.
        Interval in(Variable::make(Int(32), op->name + ".loop_min"),
                    Variable::make(Int(32), op->name + ".loop_max"));

        scope.push(op->name, in);

        Stmt body = op->body;

        string func_name = tuple_func(op->name);
        string var_name  = tuple_var(op->name);
        vector<Prefetch> &prefetches = get_func(func_name).schedule().prefetches();

        std::cerr << "Prefetch: " << op->name << " " << func_name << " " << var_name;
        if (prefetches.empty()) {
            std::cerr << " No prefetches in schedule\n";
        } else {
            std::cerr << " Checking prefetches\n";
        }

        // Todo: Check to see if op->name is in prefetches
        for (const Prefetch &p : prefetches) {
            std::cerr << "Prefetch: " << p.var
                               << " " << p.offset << "\n";
            if (p.var == var_name) {
                std::cerr << " matched on " << var_name << "\n";
                string fetch_func = "halide.hexagon.l2fetch.Rtt";

                Interval prein(in.min + p.offset, in.max + p.offset);
                scope.push(op->name, prein);

                map<string, Box> r;
                r = boxes_required(op, scope);

                // Add prefetch to body on inputs
                // Todo: For each input...
                Expr tmp = Expr(0);
                Stmt prefetch =
                    Evaluate::make(Call::make(Int(32), fetch_func,
                                      {tmp, p.offset}, Call::Extern));
                body = Block::make({prefetch, body});
            }
        }

        body = mutate(body);
        stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);

        scope.pop(op->name);
    }

};

Stmt inject_prefetch(Stmt s, const std::map<std::string, Function> &env)
{
    return InjectPrefetch(env).mutate(s);
}

}
}
