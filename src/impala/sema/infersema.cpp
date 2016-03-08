#include <sstream>

#include "thorin/util/array.h"
#include "thorin/util/log.h"
#include "thorin/util/push.h"

#include "impala/ast.h"
#include "impala/impala.h"
#include "impala/sema/typetable.h"

using namespace thorin;

namespace impala {

//------------------------------------------------------------------------------

class InferSema : public TypeTable {
public:
    // helpers

    const Type* instantiate(const Location& loc, const Type* type, ArrayRef<const ASTType*> args);

#if 0
    int take_addr_space(const PrefixExpr* prefix) {
        check(prefix->rhs());
        if (prefix->kind() == PrefixExpr::MUL) {
            auto type = prefix->rhs()->type();
            if (auto ptr = type->isa<PtrType>())
                return ptr->addr_space();
        }
        return 0;
    }
#endif

    void refine(const Type*&, const Type*);
    //const Type*& constrain(const Type*& t, const Type* u) { todo_ |= t -= u; return t; }
    const FnType*& constrain(const FnType*& t, const FnType* u);
    const Type*& constrain(const Type*& t, const Type* u);
    const Type*& constrain(const Type*& t, const Type* u, const Type* v) { return constrain(constrain(t, u), v); }
    const Type*& constrain(const Typeable* t, const Type* u) { return constrain(t->type_, u); }
    const Type*& constrain(const Typeable* t, const Type* u, const Type* v) { return constrain(constrain(t, u), v); }
    void fill_type_args(std::vector<const Type*>& type_args, const ASTTypes& ast_type_args, const Type* expected);
    const Type* safe_get_arg(const Type* type, size_t i) { return type && i < type->num_args() ? type->arg(i) : nullptr; }
    const Type* type(const Typeable* typeable) { return typeable->type_ ? typeable->type_ : typeable->type_ = unknown_type(); }

    const Type* join(const Type*, const Type*);

    // check wrappers

    void check(const ModContents* n) { n->check(*this); }
    const Type* check(const LocalDecl* local) { return constrain(local, local->check(*this)); }
    void check(const Item* n) { n->check(*this); }
    void check(const Stmt* n) { n->check(*this); }
    const Type* check(const Expr* expr, const Type* expected) { return constrain(expr, expr->check(*this, expected)); }
    const Type* check(const Expr* expr) {
        auto i = expr2expected_.find(expr);
        if (i == expr2expected_.end()) {
            auto unknown = unknown_type();
            auto p = expr2expected_.emplace(expr, unknown);
            assert(p.second);
            i = p.first;
        }
        return constrain(expr, expr->check(*this, i->second));
    }

    const TypeParam* check(const ASTTypeParam* ast_type_param) {
        if (ast_type_param->type())
            return ast_type_param->type_param();
        todo_ = true;
        return (ast_type_param->type_ = ast_type_param->check(*this))->as<TypeParam>();
    }

    const Type* check(const ASTType* ast_type) {
#if 0
        if (ast_type->type() && ast_type->type()->is_known())
            return ast_type->type();
        return constrain(ast_type, ast_type->check(*this));
#endif
        return nullptr;
    }

    const Type* check_call(const FnType*& fn_mono, const FnType* fn_poly, std::vector<const Type*>& type_args, const ASTTypes& ast_type_args, ArrayRef<const Expr*> args, const Type* expected);

private:
    struct Representative {
        Representative() {}
        Representative(const Type* parent)
            : parent(parent)
        {}

        const Type* parent = nullptr;
        int rank = 0;
    };

    Representative representative(const Type* type) {
        auto i = representatives_.find(type);
        if (i == representatives_.end()) {
            auto p = representatives_.emplace(type, type);
            assert_unused(p.second);
            return type;
        }
        return i->second;
    }

    thorin::HashMap<const Expr*, const Type*> expr2expected_;
    TypeMap<Representative> representatives_;
    bool todo_ = true;

    friend void type_inference(Init&, const ModContents*);
};

void type_inference(Init& init, const ModContents* mod) {
    static const int max_runs = 100;
    auto sema = new InferSema();
    init.typetable = sema;

    int i = 0;
    for (; sema->todo_ && i < max_runs; ++i) {
        sema->todo_ = false;
        sema->check(mod);
    }

    WLOG("iterations needed for type inference: %", i);
    if (i == max_runs)
        WLOG("needed max number of runs");
}

//------------------------------------------------------------------------------

const Type* InferSema::instantiate(const Location& loc, const Type* type, ArrayRef<const ASTType*> args) {
    if (args.size() == type->num_type_params()) {
        std::vector<const Type*> type_args;
        for (auto t : args)
            type_args.push_back(check(t));

        // TODO
        //stash_bound_check(loc, *type, type_args);
        return type->instantiate(type_args);
    } else
        error(loc) << "wrong number of instances for bound type variables: " << args.size() << " for " << type->num_type_params() << "\n";

    return type_error();
}

//------------------------------------------------------------------------------

/*
 * misc
 */

const TypeParam* ASTTypeParam::check(InferSema& sema) const { return sema.type_param(symbol()); }

void ASTTypeParamList::check_ast_type_params(InferSema& sema) const {
    for (auto ast_type_param : ast_type_params())
        sema.check(ast_type_param);
}

const Type* LocalDecl::check(InferSema& sema) const {
    if (ast_type())
        return sema.check(ast_type());
    else if (!type())
        return sema.unknown_type();
    return type();
}

const Type* Fn::check_body(InferSema& sema, const FnType* fn_type) const {
    return sema.check(body(), fn_type->return_type());
}

//------------------------------------------------------------------------------

/*
 * AST types
 */

const Type* ErrorASTType::check(InferSema& sema) const { return sema.type_error(); }

const Type* PrimASTType::check(InferSema& sema) const {
    switch (kind()) {
#define IMPALA_TYPE(itype, atype) case TYPE_##itype: return sema.prim_type(PrimType_##itype);
#include "impala/tokenlist.h"
        default: THORIN_UNREACHABLE;
    }
}

const Type* PtrASTType::check(InferSema& sema) const {
    auto referenced_type = sema.check(referenced_ast_type());
    switch (kind()) {
        case Borrowed: return sema.borrowed_ptr_type(referenced_type, addr_space());
        case Mut:      return sema.     mut_ptr_type(referenced_type, addr_space());
        case Owned:    return sema.   owned_ptr_type(referenced_type, addr_space());
    }
    THORIN_UNREACHABLE;
}

const Type* IndefiniteArrayASTType::check(InferSema& sema) const { return sema.indefinite_array_type(sema.check(elem_ast_type())); }
const Type* DefiniteArrayASTType::check(InferSema& sema) const { return sema.definite_array_type(sema.check(elem_ast_type()), dim()); }
const Type* SimdASTType::check(InferSema& sema) const { return sema.simd_type(sema.check(elem_ast_type()), size()); }

const Type* TupleASTType::check(InferSema& sema) const {
    Array<const Type*> types(num_args());
    for (size_t i = 0, e = num_args(); i != e; ++i)
        types[i] = sema.check(arg(i));

    return sema.tuple_type(types);
}

const Type* FnASTType::check(InferSema& sema) const {
    check_ast_type_params(sema);

    Array<const Type*> types(num_args());
    for (size_t i = 0, e = num_args(); i != e; ++i)
        types[i] = sema.check(arg(i));

    auto fn_type = sema.fn_type(types);
    for (auto ast_type_param : ast_type_params()) {
        sema.check(ast_type_param);
        fn_type->close({ast_type_param->type_param()}); // TODO close correctly
    }

    return fn_type;
}

const Type* Typeof::check(InferSema& sema) const { return sema.check(expr()); }

const Type* ASTTypeApp::check(InferSema& sema) const {
    if (decl()) {
        if (auto type_decl = decl()->isa<TypeDecl>()) {
            if (auto type = sema.type(type_decl))
                return sema.instantiate(loc(), type, args());
            else
                return sema.unknown_type(); // TODO don't create a new thing here
        }
    }

    return sema.type_error();
}

//------------------------------------------------------------------------------

/*
 * items
 */

void ModDecl::check(InferSema& sema) const {
    if (mod_contents())
        sema.check(mod_contents());
}

void ModContents::check(InferSema& sema) const {
    for (auto item : items())
        sema.check(item);
}

void ExternBlock::check(InferSema& sema) const {
    for (auto fn : fns())
        sema.check(fn);
}

#if 0
void Typedef::check(InferSema& sema) const {
    // TODO this is broken
    check_ast_type_params(sema);
    sema.check(ast_type());

    if (ast_type_params().size() > 0) {
        auto abs = sema.typedef_abs(sema.type(ast_type())); // TODO might be nullptr
        for (auto type_param : type_params())
            abs->bind(type_param->type_param());
    } else
        sema.constrain(this, sema.type(ast_type()));
}
#endif

void EnumDecl::check(InferSema&) const { /*TODO*/ }

void StructDecl::check(InferSema& sema) const {
    check_ast_type_params(sema);

    for (auto field : field_decls()) {
        if (auto field_type = sema.type(field)) {
            if (!field_type || field_type->is_unknown())
                return; // bail out for now if we don't yet know all field types
        }
    }

    auto struct_type = sema.struct_abs_type(this);

    for (auto field : field_decls())
        struct_type->set(field->index(), sema.type(field));

    for (auto ast_type_param : ast_type_params())
        struct_type->close({ast_type_param->type_param()}); // TODO close correctly

    type_ = struct_type;
}

void FieldDecl::check(InferSema& sema) const { sema.check(ast_type()); }

void FnDecl::check(InferSema& sema) const {
    check_ast_type_params(sema);

    Array<const Type*> param_types(num_params());
    for (size_t i = 0, e = num_params(); i != e; ++i)
        param_types[i] = sema.check(param(i));

    sema.constrain(this, sema.fn_type(param_types));

    for (auto ast_type_param : ast_type_params())
        fn_type()->close({ast_type_param->type_param()}); // TODO close correctly

    if (body() != nullptr)
        check_body(sema, fn_type());
}

void StaticItem::check(InferSema& sema) const {
    sema.constrain(this, sema.type(init()));
}

void TraitDecl::check(InferSema& /*sema*/) const {}
void ImplItem::check(InferSema& /*sema*/) const {}


//------------------------------------------------------------------------------

/*
 * expressions
 */

const Type* EmptyExpr::check(InferSema& sema, const Type*) const { return sema.unit(); }
const Type* LiteralExpr::check(InferSema& sema, const Type*) const { return sema.prim_type(literal2type()); }
const Type* CharExpr::check(InferSema& sema, const Type*) const { return sema.type_u8(); }
const Type* StrExpr::check(InferSema& sema, const Type*) const { return sema.definite_array_type(sema.type_u8(), values_.size()); }

const Type* FnExpr::check(InferSema& sema, const Type* expected) const {
    assert(ast_type_params().empty());

    Array<const Type*> param_types(num_params());
    for (size_t i = 0, e = num_params(); i != e; ++i)
        param_types[i] = sema.constrain(param(i), sema.safe_get_arg(expected, i));

    auto fn_type = sema.fn_type(param_types);
    assert(body() != nullptr);
    check_body(sema, fn_type);
    return fn_type;
}

const Type* PathExpr::check(InferSema& sema, const Type* expected) const {
    if (value_decl())
        return sema.constrain(value_decl(), expected);
    return sema.type_error();
}

const Type* PrefixExpr::check(InferSema& sema, const Type* expected) const {
    switch (kind()) {
        case AND: {
            auto expected_referenced_type = sema.safe_get_arg(expected, 0);
            auto rtype = sema.check(rhs(), expected_referenced_type);
            int addr_space = 0;
#if 0
            // keep the address space of the original pointer, if possible
            if (auto map = rhs()->isa<MapExpr>()) {
                if (auto prefix = map->lhs()->isa<PrefixExpr>())
                    addr_space = sema.take_addr_space(prefix);
            } else if (auto field = rhs()->isa<FieldExpr>()) {
                if (auto prefix = field->lhs()->isa<PrefixExpr>())
                    addr_space = sema.take_addr_space(prefix);
            } else if (auto prefix = rhs()->isa<PrefixExpr>()) {
                addr_space = sema.take_addr_space(prefix);
            }
#endif

            return sema.borrowed_ptr_type(rtype, addr_space);

        }
        case TILDE:
            return sema.owned_ptr_type(sema.check(rhs(), sema.safe_get_arg(expected, 0)));
        case MUL:
            return sema.check(rhs(), sema.borrowed_ptr_type(expected));
        case INC: case DEC:
        case ADD: case SUB:
        case NOT:
        case RUN: case HLT:
            return sema.check(rhs(), expected);
        case OR:  case OROR: // Lambda
            THORIN_UNREACHABLE;
    }
    THORIN_UNREACHABLE;
}

const Type* InfixExpr::check(InferSema& sema, const Type* expected) const {
    switch (kind()) {
        case EQ: case NE:
        case LT: case LE:
        case GT: case GE:
            sema.check(lhs(), sema.join(expected, sema.type(rhs())));
            sema.check(rhs(), sema.type(lhs()));
            return sema.type_bool();
        case OROR:
        case ANDAND:
            sema.check(lhs(), sema.type_bool());
            sema.check(rhs(), sema.type_bool());
            return sema.type_bool();
        case ADD: case SUB:
        case MUL: case DIV: case REM:
        case SHL: case SHR:
        case AND: case OR:  case XOR:
            sema.check(lhs(), sema.join(expected, sema.type(rhs())));
            sema.check(rhs(), sema.type(lhs()));
            return sema.type(rhs());
        case ASGN:
        case ADD_ASGN: case SUB_ASGN:
        case MUL_ASGN: case DIV_ASGN: case REM_ASGN:
        case SHL_ASGN: case SHR_ASGN:
        case AND_ASGN: case  OR_ASGN: case XOR_ASGN:
            sema.check(lhs(), sema.type(rhs()));
            sema.check(rhs(), sema.type(lhs()));
            return sema.unit();
    }

    THORIN_UNREACHABLE;
}

const Type* PostfixExpr::check(InferSema& sema, const Type* expected) const {
    return sema.check(lhs(), expected);
}

const Type* CastExpr::check(InferSema& sema, const Type*) const {
    sema.check(lhs());
    return sema.check(ast_type());
}

const Type* DefiniteArrayExpr::check(InferSema& sema, const Type* expected) const {
    auto expected_elem_type = sema.safe_get_arg(expected, 0);

    for (size_t i = 0, e = num_args(); i != e; ++i) {
        sema.refine(expected_elem_type, sema.type(arg((i+1) % e)));
        sema.check(arg(i), expected_elem_type);
    }

    for (auto arg : args())
        sema.refine(expected_elem_type, sema.type(arg));

    for (auto arg : args())
        sema.check(arg, expected_elem_type);

    return sema.definite_array_type(expected_elem_type, num_args());
}

const Type* SimdExpr::check(InferSema& sema, const Type* expected) const {
    auto expected_elem_type = sema.safe_get_arg(expected, 0);

    for (size_t i = 0, e = num_args(); i != e; ++i) {
        sema.refine(expected_elem_type, sema.type(arg((i+1)%e)));
        sema.check(arg(i), expected_elem_type);
    }

    return sema.simd_type(expected_elem_type, num_args());
}

const Type* RepeatedDefiniteArrayExpr::check(InferSema& sema, const Type* expected) const {
    auto expected_elem_type = sema.safe_get_arg(expected, 0);
    return sema.definite_array_type(sema.check(value(), expected_elem_type), count());
}

const Type* IndefiniteArrayExpr::check(InferSema& sema, const Type*) const {
    sema.check(dim());
    return sema.indefinite_array_type(sema.check(elem_ast_type()));
}

const Type* TupleExpr::check(InferSema& sema, const Type* expected) const {
    Array<const Type*> types(num_args());
    for (size_t i = 0, e = types.size(); i != e; ++i)
        types[i] = sema.check(arg(i), sema.safe_get_arg(expected, i));

    return sema.tuple_type(types);
}

void InferSema::fill_type_args(std::vector<const Type*>& type_args, const ASTTypes& ast_type_args, const Type* expected) {
    for (size_t i = 0, e = type_args.size(); i != e; ++i) {
        if (i < ast_type_args.size())
            constrain(type_args[i], check(ast_type_args[i]), safe_get_arg(expected, i));
        else if (!type_args[i])
            type_args[i] = unknown_type();
    }
}

const Type* StructExpr::check(InferSema& sema, const Type* expected) const {
    if (auto decl = path()->decl()) {
        if (auto typeable_decl = decl->isa<TypeableDecl>()) {
            if (auto decl_type = sema.type(typeable_decl)) {
                type_args_.resize(decl_type->num_type_params());
                sema.fill_type_args(type_args_, ast_type_args_, expected);

                if (auto struct_app = decl_type->instantiate(type_args_))
                    return struct_app;
            }
        }
    }

    return sema.type_error();
}

const Type* InferSema::check_call(const FnType*& fn_mono, const FnType* fn_poly, std::vector<const Type*>& type_args, const ASTTypes& ast_type_args, ArrayRef<const Expr*> args, const Type* expected) {
    type_args.resize(fn_poly->num_type_params());
    fill_type_args(type_args, ast_type_args, expected);

    constrain(fn_mono, fn_poly->instantiate(type_args)->as<FnType>());
    auto max_arg_index = std::min(args.size(), fn_mono->num_args());
    bool is_returning  = args.size()+1 == fn_mono->num_args();

    for (size_t i = 0; i != max_arg_index; ++i)
        constrain(args[i], fn_mono->arg(i));

    if (is_returning && expected) {
        Array<const Type*> args(fn_mono->num_args());
        *std::copy(fn_mono->args().begin(), fn_mono->args().end()-1, args.begin()) = expected;
        constrain(fn_mono, fn_type(args));
    }

    for (size_t i = 0; i != max_arg_index; ++i)
        check(args[i], fn_mono->arg(i));

    return fn_mono->return_type();
}

const Type* FieldExpr::check(InferSema& sema, const Type* /*TODO expected*/) const {
    auto ltype = sema.check(lhs());
    if (ltype->isa<PtrType>()) {
        PrefixExpr::create_deref(lhs_);
        ltype = sema.check(lhs());
    }

    if (auto struct_app = ltype->isa<StructAppType>()) {
        if (auto field_decl = struct_app->struct_abs_type()->struct_decl()->field_decl(symbol()))
            return struct_app->elem(field_decl->index());
    }

    return sema.type_error();
}

const Type* MapExpr::check(InferSema& sema, const Type* expected) const {
    auto ltype = sema.check(lhs());
    if (ltype->isa<PtrType>()) {
        PrefixExpr::create_deref(lhs_);
        ltype = sema.check(lhs());
    }

    if (auto fn_poly = ltype->isa<FnType>()) {
        return sema.check_call(fn_mono_, fn_poly, type_args_, ast_type_args(), args(), expected);
    } else {
        if (num_args() == 1)
            sema.check(arg(0));

        if (auto array = ltype->isa<ArrayType>()) {
            return array->elem_type();
        } else if (auto tuple_type = ltype->isa<TupleType>()) {
            if (auto lit = arg(0)->isa<LiteralExpr>())
                return tuple_type->arg(lit->get_u64());
        } else if (auto simd_type = ltype->isa<SimdType>()) {
            return simd_type->elem_type();
        }
    }

    return sema.type_error();
}

const Type* BlockExprBase::check(InferSema& sema, const Type* expected) const {
    for (auto stmt : stmts())
        sema.check(stmt);

    sema.check(expr(), expected);

    return expr() ? sema.type(expr()) : sema.unit()->as<Type>();
}

const Type* IfExpr::check(InferSema& sema, const Type* expected) const {
    sema.check(cond(), sema.type_bool());
    sema.constrain(then_expr(), sema.type(else_expr()), expected);
    sema.constrain(else_expr(), sema.type(then_expr()), expected);
    sema.check(then_expr(), expected);
    sema.check(else_expr(), expected);
    return sema.constrain(this, sema.type(then_expr()), sema.type(else_expr()));
}

const Type* WhileExpr::check(InferSema& sema, const Type*) const {
    sema.check(cond(), sema.type_bool());
    sema.check(break_decl());
    sema.check(continue_decl());
    sema.check(body(), sema.unit());
    return sema.unit();
}

const Type* ForExpr::check(InferSema& sema, const Type* expected) const {
    auto forexpr = expr();
    if (auto prefix = forexpr->isa<PrefixExpr>())
        if (prefix->kind() == PrefixExpr::RUN || prefix->kind() == PrefixExpr::HLT)
            forexpr = prefix->rhs();

    if (auto map = forexpr->isa<MapExpr>()) {
        auto ltype = sema.check(map->lhs());

        if (auto fn_for = ltype->isa<FnType>()) {
            if (fn_for->num_args() != 0) {
                if (auto fn_ret = fn_for->args().back()->isa<FnType>()) {
                    sema.constrain(break_decl_->type_, fn_ret); // inherit the type for break

                    // copy over args and check call
                    Array<const Expr*> args(map->args().size()+1);
                    *std::copy(map->args().begin(), map->args().end(), args.begin()) = fn_expr();
                    return sema.check_call(map->fn_mono_, fn_for, map->type_args_, map->ast_type_args(), args, expected);
                }
            }
        }
    }

    return sema.unit();
}

//------------------------------------------------------------------------------

/*
 * statements
 */

void ExprStmt::check(InferSema& sema) const { sema.check(expr()); }
void ItemStmt::check(InferSema& sema) const { sema.check(item()); }

void LetStmt::check(InferSema& sema) const {
    auto expected = sema.check(local());
    if (init())
        sema.check(init(), expected);
}

//------------------------------------------------------------------------------

}
