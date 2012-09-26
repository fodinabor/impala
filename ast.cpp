#include "impala/ast.h"

#include "anydsl/cfg.h"
#include "anydsl/util/cast.h"

#include "impala/type.h"

using anydsl::Box;
using anydsl::Location;
using anydsl::Position;
using anydsl::Symbol;
using anydsl::dcast;

namespace impala {

//------------------------------------------------------------------------------

Decl::Decl(const Token& tok, const Type* type, const Position& pos2)
    : symbol_(tok.symbol())
    , type_(type)
{
    set_loc(tok.pos1(), pos2);
}

void Fct::set(const Decl* decl, const ScopeStmt* body) {
    decl_ = decl;
    body_ = body;
    set_loc(decl->pos1(), body->pos2());
}

bool Fct::continuation() const { 
    return pi()->ret()->is_noret(); 
}

const Pi* Fct::pi() const { 
    return decl_->type()->as<Pi>(); 
}

anydsl::Symbol Fct::symbol() const { 
    return decl_->symbol(); 
}

/*
 * Expr
 */

Literal::Literal(const Location& loc, Kind kind, Box box)
    : kind_(kind)
    , box_(box)
{
    loc_= loc;
}

Tuple::Tuple(const Position& pos1)
{
    loc_.set_pos1(pos1);
}

Id::Id(const Token& tok) 
    : symbol_(tok.symbol())
{
    loc_ = tok.loc();
}

PrefixExpr::PrefixExpr(const Position& pos1, Kind kind, const Expr* rhs)
    : kind_(kind)
{
    ops_.push_back(rhs);
    set_loc(pos1, rhs->pos2());
}

InfixExpr::InfixExpr(const Expr* lhs, Kind kind, const Expr* rhs)
    : kind_(kind)
{
    ops_.push_back(lhs);
    ops_.push_back(rhs);
    set_loc(lhs->pos1(), rhs->pos2());
}

PostfixExpr::PostfixExpr(const Expr* lhs, Kind kind, const Position& pos2) 
    : kind_(kind)
{
    ops_.push_back(lhs);
    set_loc(lhs->pos1(), pos2);
}

//const Def* Ref::load() const {
//}

IndexExpr::IndexExpr(const anydsl::Position& pos1, const Expr* lhs, const Expr* index, const anydsl::Position& pos2) {
    ops_.push_back(lhs);
    ops_.push_back(index);
    set_loc(pos1, pos2);
}

Call::Call(const Expr* fct) {
    ops_.push_back(fct);
}

void Call::set_pos2(const Position& pos2) {
    assert(!ops_.empty());
    HasLocation::set_loc(ops_.front()->pos1(), pos2);
}

/*
 * Stmt
 */

ExprStmt::ExprStmt(const Expr* expr, const Position& pos2)
    : expr_(expr)
{
    set_loc(expr->pos1(), pos2);
}

DeclStmt::DeclStmt(const Decl* decl, const Expr* init, const Position& pos2)
    : decl_(decl)
    , init_(init)
{
    set_loc(decl->pos1(), pos2);
}

IfElseStmt::IfElseStmt(const Position& pos1, const Expr* cond, const Stmt* thenStmt, const Stmt* elseStmt)
    : cond_(cond)
    , thenStmt_(thenStmt)
    , elseStmt_(elseStmt)
{
    set_loc(pos1, elseStmt->pos2());
}

void WhileStmt::set(const Position& pos1, const Expr* cond, const Stmt* body) {
    Loop::set(cond, body);
    set_loc(pos1, body->pos2());
}

void DoWhileStmt::set(const Position& pos1, const Stmt* body, const Expr* cond, const Position& pos2) {
    Loop::set(cond, body);
    set_loc(pos1, pos2);
}

ForStmt::~ForStmt() {
    if (isDecl())
        delete initDecl_;
    else
        delete initExpr_;
}

void ForStmt::set(const Position& pos1, const Expr* cond, const Expr* step, const Stmt* body) {
    Loop::set(cond, body);
    step_ = step;
    set_loc(pos1, body->pos2());
}


BreakStmt::BreakStmt(const Position& pos1, const Position& pos2, const Loop* loop) 
    : loop_(loop)
{
    set_loc(pos1, pos2);
}

ContinueStmt::ContinueStmt(const Position& pos1, const Position& pos2, const Loop* loop) 
    : loop_(loop)
{
    set_loc(pos1, pos2);
}

ReturnStmt::ReturnStmt(const Position& pos1, const Expr* expr, const Fct* fct, const Position& pos2)
    : expr_(expr)
    , fct_(fct)
{
    set_loc(pos1, pos2);
}

} // namespace impala
