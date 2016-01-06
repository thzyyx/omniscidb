/**
 * @file    Analyzer.h
 * @author  Wei Hong <wei@map-d.com>
 * @brief   Defines data structures for the semantic analysis phase of query processing
 *
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 **/
#ifndef ANALYZER_H
#define ANALYZER_H

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <list>
#include <set>
#include <type_traits>
#include <glog/logging.h>
#include "../Shared/sqltypes.h"
#include "../Shared/sqldefs.h"
#include "../Catalog/Catalog.h"

namespace Analyzer {
class Expr;
}

template <typename Tp, typename... Args>
inline typename std::enable_if<std::is_base_of<Analyzer::Expr, Tp>::value, std::shared_ptr<Tp>>::type makeExpr(
    Args&&... args) {
  return std::make_shared<Tp>(std::forward<Args>(args)...);
}

namespace Analyzer {

class ColumnVar;
class TargetEntry;
class Expr;
typedef std::list<const Expr*> DomainSet;

/*
 * @type Expr
 * @brief super class for all expressions in parse trees and in query plans
 */
class Expr : public std::enable_shared_from_this<Expr> {
 public:
  Expr(SQLTypes t, bool notnull) : type_info(t, notnull), contains_agg(false) {}
  Expr(SQLTypes t, int d, bool notnull) : type_info(t, d, 0, notnull), contains_agg(false) {}
  Expr(SQLTypes t, int d, int s, bool notnull) : type_info(t, d, s, notnull), contains_agg(false) {}
  Expr(const SQLTypeInfo& ti, bool has_agg = false) : type_info(ti), contains_agg(has_agg) {}
  virtual ~Expr() {}
  const SQLTypeInfo& get_type_info() const { return type_info; }
  void set_type_info(const SQLTypeInfo& ti) { type_info = ti; }
  bool get_contains_agg() const { return contains_agg; }
  void set_contains_agg(bool a) { contains_agg = a; }
  virtual std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info);
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const {};
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const = 0;  // make a deep copy of self
                                                                  /*
                                                                   * @brief normalize_simple_predicate only applies to boolean expressions.
                                                                   * it checks if it is an expression comparing a column
                                                                   * with a constant.  if so, it returns a normalized copy of the predicate with ColumnVar
                                                                   * always as the left operand with rte_idx set to the rte_idx of the ColumnVar.
                                                                   * it returns nullptr with rte_idx set to -1 otherwise.
                                                                   */
  virtual std::shared_ptr<Analyzer::Expr> normalize_simple_predicate(int& rte_idx) const {
    rte_idx = -1;
    return nullptr;
  }
  /*
   * @brief seperate conjunctive predicates into scan predicates, join predicates and constant
   * predicates.
   */
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const {}
  /*
   * @brief collect_rte_idx collects the indices of all the range table
   * entries involved in an expression
   */
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const {}
  /*
   * @brief collect_column_var collects all unique ColumnVar nodes in an expression
   * If include_agg = false, it does not include to ColumnVar nodes inside
   * the argument to AggExpr's.  Otherwise, they are included.
   * It does not make copies of the ColumnVar
   */
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {}
  /*
   * @brief rewrite_with_targetlist rewrite ColumnVar's in expression with entries in a targetlist.
   * targetlist expressions are expected to be only Var's or AggExpr's.
   * returns a new expression copy
   */
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return deep_copy();
  };
  /*
   * @brief rewrite_with_child_targetlist rewrite ColumnVar's in expression with entries in a child
   * plan's targetlist.
   * targetlist expressions are expected to be only Var's or ColumnVar's
   * returns a new expression copy
   */
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return deep_copy();
  };
  /*
   * @brief rewrite_agg_to_var rewrite ColumnVar's in expression with entries in an AggPlan's
   * targetlist.
   * targetlist expressions are expected to be only Var's or ColumnVar's or AggExpr's
   * All AggExpr's are written into Var's.
   * returns a new expression copy
   */
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const {
    return deep_copy();
  }
  virtual bool operator==(const Expr& rhs) const = 0;
  virtual void print() const = 0;
  virtual void add_unique(std::list<const Expr*>& expr_list) const;
  /*
   * @brief find_expr traverse Expr hierarchy and adds the node pointer to
   * the expr_list if the function f returns true.
   * Duplicate Expr's are not added the list.
   * Cannot use std::set because we don't have an ordering function.
   */
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const {
    if (f(this))
      add_unique(expr_list);
  }
  /*
   * @brief decompress adds cast operator to decompress encoded result
   */
  std::shared_ptr<Analyzer::Expr> decompress();
  /*
   * @brief perform domain analysis on Expr and fill in domain
   * information in domain_set.  Empty domain_set means no information.
   */
  virtual void get_domain(DomainSet& domain_set) const { domain_set.clear(); }

 protected:
  SQLTypeInfo type_info;  // SQLTypeInfo of the return result of this expression
  bool contains_agg;
};

/*
 * @type ColumnVar
 * @brief expression that evaluates to the value of a column in a given row from a base table.
 * It is used in parse trees and is only used in Scan nodes in a query plan
 * for scanning a table while Var nodes are used for all other plans.
 */
class ColumnVar : public Expr {
 public:
  ColumnVar(const SQLTypeInfo& ti, int r, int c, int i) : Expr(ti), table_id(r), column_id(c), rte_idx(i) {}
  int get_table_id() const { return table_id; }
  int get_column_id() const { return column_id; }
  int get_rte_idx() const { return rte_idx; }
  EncodingType get_compression() const { return type_info.get_compression(); }
  int get_comp_param() const { return type_info.get_comp_param(); }
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const;
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const { rte_idx_set.insert(rte_idx); }
  static bool colvar_comp(const ColumnVar* l, const ColumnVar* r) {
    return l->get_table_id() < r->get_table_id() ||
           (l->get_table_id() == r->get_table_id() && l->get_column_id() < r->get_column_id());
  }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    colvar_set.insert(this);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const;
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;

 protected:
  int table_id;   // the global table id
  int column_id;  // the column id
  int rte_idx;    // 0-based range table index. only used by the analyzer and planner.
};

/*
 * @type Var
 * @brief expression that evaluates to the value of a column in a given row generated
 * from a query plan node.  It is only used in plan nodes above Scan nodes.
 * The row can be produced by either the inner or the outer plan in case of a join.
 * It inherits from ColumnVar to keep track of the lineage through the plan nodes.
 * The table_id will be set to 0 if the Var does not correspond to an original column value.
 */
class Var : public ColumnVar {
 public:
  enum WhichRow { kINPUT_OUTER, kINPUT_INNER, kOUTPUT, kGROUPBY };
  Var(const SQLTypeInfo& ti, int r, int c, int i, WhichRow o, int v) : ColumnVar(ti, r, c, i), which_row(o), varno(v) {}
  Var(const SQLTypeInfo& ti, WhichRow o, int v) : ColumnVar(ti, 0, 0, -1), which_row(o), varno(v) {}
  WhichRow get_which_row() const { return which_row; }
  void set_which_row(WhichRow r) { which_row = r; }
  int get_varno() const { return varno; }
  void set_varno(int n) { varno = n; }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void print() const;
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const { rte_idx_set.insert(-1); }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {}
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return deep_copy();
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return deep_copy();
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const;

 private:
  WhichRow which_row;  // indicate which row this Var should project from.  It can be from the outer
                       // input plan or the inner input plan (for joins) or the output row in the
                       // current plan.
  int varno;           // the column number in the row.  1-based
};

/*
 * @type Constant
 * @brief expression for a constant value
 */
class Constant : public Expr {
 public:
  Constant(SQLTypes t, bool n) : Expr(t, !n), is_null(n) {
    if (n)
      set_null_value();
  }
  Constant(SQLTypes t, bool n, Datum v) : Expr(t, !n), is_null(n), constval(v) {
    if (n)
      set_null_value();
  }
  Constant(const SQLTypeInfo& ti, bool n, Datum v) : Expr(ti), is_null(n), constval(v) {
    if (n)
      set_null_value();
  }
  virtual ~Constant();
  bool get_is_null() const { return is_null; }
  Datum get_constval() const { return constval; }
  void set_constval(Datum d) { constval = d; }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info);
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;

 private:
  bool is_null;    // constant is NULL
  Datum constval;  // the constant value
  void cast_number(const SQLTypeInfo& new_type_info);
  void cast_string(const SQLTypeInfo& new_type_info);
  void cast_from_string(const SQLTypeInfo& new_type_info);
  void cast_to_string(const SQLTypeInfo& new_type_info);
  void do_cast(const SQLTypeInfo& new_type_info);
  void set_null_value();
};

/*
 * @type UOper
 * @brief represents unary operator expressions.  operator types include
 * kUMINUS, kISNULL, kEXISTS, kCAST
 */
class UOper : public Expr {
 public:
  UOper(const SQLTypeInfo& ti, bool has_agg, SQLOps o, std::shared_ptr<Analyzer::Expr> p)
      : Expr(ti, has_agg), optype(o), operand(p) {}
  UOper(SQLTypes t, SQLOps o, std::shared_ptr<Analyzer::Expr> p)
      : Expr(t, o == kISNULL ? true : p->get_type_info().get_notnull()), optype(o), operand(p) {}
  SQLOps get_optype() const { return optype; }
  const Expr* get_operand() const { return operand.get(); }
  const std::shared_ptr<Expr> get_own_operand() const { return operand; }
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const;
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const { operand->collect_rte_idx(rte_idx_set); }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    operand->collect_column_var(colvar_set, include_agg);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<UOper>(type_info, contains_agg, optype, operand->rewrite_with_targetlist(tlist));
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<UOper>(type_info, contains_agg, optype, operand->rewrite_with_child_targetlist(tlist));
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<UOper>(type_info, contains_agg, optype, operand->rewrite_agg_to_var(tlist));
  }
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;
  virtual std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info);

 private:
  SQLOps optype;                            // operator type, e.g., kUMINUS, kISNULL, kEXISTS
  std::shared_ptr<Analyzer::Expr> operand;  // operand expression
};

/*
 * @type BinOper
 * @brief represents binary operator expressions.  it includes all
 * comparison, arithmetic and boolean binary operators.  it handles ANY/ALL qualifiers
 * in case the right_operand is a subquery.
 */
class BinOper : public Expr {
 public:
  BinOper(const SQLTypeInfo& ti,
          bool has_agg,
          SQLOps o,
          SQLQualifier q,
          std::shared_ptr<Analyzer::Expr> l,
          std::shared_ptr<Analyzer::Expr> r)
      : Expr(ti, has_agg), optype(o), qualifier(q), left_operand(l), right_operand(r) {}
  BinOper(SQLTypes t, SQLOps o, SQLQualifier q, std::shared_ptr<Analyzer::Expr> l, std::shared_ptr<Analyzer::Expr> r)
      : Expr(t, l->get_type_info().get_notnull() && r->get_type_info().get_notnull()),
        optype(o),
        qualifier(q),
        left_operand(l),
        right_operand(r) {}
  SQLOps get_optype() const { return optype; }
  SQLQualifier get_qualifier() const { return qualifier; }
  const Expr* get_left_operand() const { return left_operand.get(); }
  const Expr* get_right_operand() const { return right_operand.get(); }
  static SQLTypeInfo analyze_type_info(SQLOps op,
                                       const SQLTypeInfo& left_type,
                                       const SQLTypeInfo& right_type,
                                       SQLTypeInfo* new_left_type,
                                       SQLTypeInfo* new_right_type);
  static SQLTypeInfo common_numeric_type(const SQLTypeInfo& type1, const SQLTypeInfo& type2);
  static SQLTypeInfo common_string_type(const SQLTypeInfo& type1, const SQLTypeInfo& type2);
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const;
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual std::shared_ptr<Analyzer::Expr> normalize_simple_predicate(int& rte_idx) const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const {
    left_operand->collect_rte_idx(rte_idx_set);
    right_operand->collect_rte_idx(rte_idx_set);
  }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    left_operand->collect_column_var(colvar_set, include_agg);
    right_operand->collect_column_var(colvar_set, include_agg);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<BinOper>(type_info,
                             contains_agg,
                             optype,
                             qualifier,
                             left_operand->rewrite_with_targetlist(tlist),
                             right_operand->rewrite_with_targetlist(tlist));
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<BinOper>(type_info,
                             contains_agg,
                             optype,
                             qualifier,
                             left_operand->rewrite_with_child_targetlist(tlist),
                             right_operand->rewrite_with_child_targetlist(tlist));
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<BinOper>(type_info,
                             contains_agg,
                             optype,
                             qualifier,
                             left_operand->rewrite_agg_to_var(tlist),
                             right_operand->rewrite_agg_to_var(tlist));
  }
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;

 private:
  SQLOps optype;           // operator type, e.g., kLT, kAND, kPLUS, etc.
  SQLQualifier qualifier;  // qualifier kANY, kALL or kONE.  Only relevant with right_operand is Subquery
  std::shared_ptr<Analyzer::Expr> left_operand;   // the left operand expression
  std::shared_ptr<Analyzer::Expr> right_operand;  // the right operand expression
};

class Query;

/*
 * @type Subquery
 * @brief subquery expression.  Note that the type of the expression is the type of the
 * TargetEntry in the subquery instead of the set.
 */
class Subquery : public Expr {
 public:
  Subquery(const SQLTypeInfo& ti, Query* q) : Expr(ti), parsetree(q) /*, plan(nullptr)*/ {}
  virtual ~Subquery();
  const Query* get_parsetree() const { return parsetree; }
  // const Plan *get_plan() const { return plan; }
  // void set_plan(Plan *p) { plan = p; } // subquery plan is set by the optimizer
  virtual std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info);
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const {
    CHECK(false);
  }
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const { CHECK(false); }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    CHECK(false);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const {
    CHECK(false);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const {
    CHECK(false);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const {
    CHECK(false);
  }
  virtual bool operator==(const Expr& rhs) const {
    CHECK(false);
    return false;
  }
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const { CHECK(false); }

 private:
  Query* parsetree;  // parse tree of the subquery
};

/*
 * @type InValues
 * @brief represents predicate expr IN (v1, v2, ...)
 * v1, v2, ... are can be either Constant or Parameter.
 */
class InValues : public Expr {
 public:
  InValues(std::shared_ptr<Analyzer::Expr> a, std::list<std::shared_ptr<Analyzer::Expr>>& l)
      : Expr(kBOOLEAN, true), arg(a), value_list(l) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::list<std::shared_ptr<Analyzer::Expr>>& get_value_list() const { return value_list; }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const { arg->collect_rte_idx(rte_idx_set); }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    arg->collect_column_var(colvar_set, include_agg);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const;
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;

 private:
  std::shared_ptr<Analyzer::Expr> arg;                    // the argument left of IN
  std::list<std::shared_ptr<Analyzer::Expr>> value_list;  // the list of values right of IN
};

/*
 * @type CharLengthExpr
 * @brief expression for the CHAR_LENGTH expression.
 * arg must evaluate to char, varchar or text.
 */
class CharLengthExpr : public Expr {
 public:
  CharLengthExpr(std::shared_ptr<Analyzer::Expr> a, bool e)
      : Expr(kINT, a->get_type_info().get_notnull()), arg(a), calc_encoded_length(e) {}
  const Expr* get_arg() const { return arg.get(); }
  bool get_calc_encoded_length() const { return calc_encoded_length; }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const { arg->collect_rte_idx(rte_idx_set); }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    arg->collect_column_var(colvar_set, include_agg);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<CharLengthExpr>(arg->rewrite_with_targetlist(tlist), calc_encoded_length);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<CharLengthExpr>(arg->rewrite_with_child_targetlist(tlist), calc_encoded_length);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<CharLengthExpr>(arg->rewrite_agg_to_var(tlist), calc_encoded_length);
  }
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;

 private:
  std::shared_ptr<Analyzer::Expr> arg;
  bool calc_encoded_length;
};

/*
 * @type LikeExpr
 * @brief expression for the LIKE predicate.
 * arg must evaluate to char, varchar or text.
 */
class LikeExpr : public Expr {
 public:
  LikeExpr(std::shared_ptr<Analyzer::Expr> a,
           std::shared_ptr<Analyzer::Expr> l,
           std::shared_ptr<Analyzer::Expr> e,
           bool i,
           bool s)
      : Expr(kBOOLEAN, a->get_type_info().get_notnull()),
        arg(a),
        like_expr(l),
        escape_expr(e),
        is_ilike(i),
        is_simple(s) {}
  const Expr* get_arg() const { return arg.get(); }
  const Expr* get_like_expr() const { return like_expr.get(); }
  const Expr* get_escape_expr() const { return escape_expr.get(); }
  bool get_is_ilike() const { return is_ilike; }
  bool get_is_simple() const { return is_simple; }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const { arg->collect_rte_idx(rte_idx_set); }
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    arg->collect_column_var(colvar_set, include_agg);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<LikeExpr>(arg->rewrite_with_targetlist(tlist),
                              like_expr->deep_copy(),
                              escape_expr ? escape_expr->deep_copy() : nullptr,
                              is_ilike,
                              is_simple);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<LikeExpr>(arg->rewrite_with_child_targetlist(tlist),
                              like_expr->deep_copy(),
                              escape_expr ? escape_expr->deep_copy() : nullptr,
                              is_ilike,
                              is_simple);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const {
    return makeExpr<LikeExpr>(arg->rewrite_agg_to_var(tlist),
                              like_expr->deep_copy(),
                              escape_expr ? escape_expr->deep_copy() : nullptr,
                              is_ilike,
                              is_simple);
  }
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;

 private:
  std::shared_ptr<Analyzer::Expr> arg;          // the argument to the left of LIKE
  std::shared_ptr<Analyzer::Expr> like_expr;    // expression that evaluates to like string
  std::shared_ptr<Analyzer::Expr> escape_expr;  // expression that evaluates to escape string, can be nullptr
  bool is_ilike;                                // is this ILIKE?
  bool is_simple;  // is this simple, meaning we can use fast path search (fits '%str%' pattern with
                   // no inner '%','_','[',']'
};

/*
 * @type AggExpr
 * @brief expression for builtin SQL aggregates.
 */
class AggExpr : public Expr {
 public:
  AggExpr(const SQLTypeInfo& ti, SQLAgg a, std::shared_ptr<Analyzer::Expr> g, bool d)
      : Expr(ti, true), aggtype(a), arg(g), is_distinct(d) {}
  AggExpr(SQLTypes t, SQLAgg a, Expr* g, bool d, int idx)
      : Expr(SQLTypeInfo(t, g == nullptr ? true : g->get_type_info().get_notnull()), true),
        aggtype(a),
        arg(g),
        is_distinct(d) {}
  SQLAgg get_aggtype() const { return aggtype; }
  const Expr* get_arg() const { return arg.get(); }
  bool get_is_distinct() const { return is_distinct; }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const {
    if (arg)
      arg->collect_rte_idx(rte_idx_set);
  };
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const {
    if (include_agg && arg != nullptr)
      arg->collect_column_var(colvar_set, include_agg);
  }
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const;
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;

 private:
  SQLAgg aggtype;                       // aggregate type: kAVG, kMIN, kMAX, kSUM, kCOUNT
  std::shared_ptr<Analyzer::Expr> arg;  // argument to aggregate
  bool is_distinct;                     // true only if it is for COUNT(DISTINCT x).
};

/*
 * @type CaseExpr
 * @brief the CASE-WHEN-THEN-ELSE expression
 */
class CaseExpr : public Expr {
 public:
  CaseExpr(const SQLTypeInfo& ti,
           bool has_agg,
           const std::list<std::pair<std::shared_ptr<Analyzer::Expr>, std::shared_ptr<Analyzer::Expr>>>& w,
           std::shared_ptr<Analyzer::Expr> e)
      : Expr(ti, has_agg), expr_pair_list(w), else_expr(e) {}
  const std::list<std::pair<std::shared_ptr<Analyzer::Expr>, std::shared_ptr<Analyzer::Expr>>>& get_expr_pair_list()
      const {
    return expr_pair_list;
  }
  const Expr* get_else_expr() const { return else_expr.get(); }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const;
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const;
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;
  virtual std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info);
  virtual void get_domain(DomainSet& domain_set) const;

 private:
  std::list<std::pair<std::shared_ptr<Analyzer::Expr>, std::shared_ptr<Analyzer::Expr>>>
      expr_pair_list;                         // a pair of expressions for each WHEN expr1 THEN expr2.  expr1 must be of
                                              // boolean type.  all expr2's must be of compatible types and will be
                                              // promoted to the common type.
  std::shared_ptr<Analyzer::Expr> else_expr;  // expression for ELSE.  nullptr if omitted.
};

/*
 * @type ExtractExpr
 * @brief the EXTRACT expression
 */
class ExtractExpr : public Expr {
 public:
  ExtractExpr(const SQLTypeInfo& ti, bool has_agg, ExtractField f, std::shared_ptr<Analyzer::Expr> e)
      : Expr(ti, has_agg), field(f), from_expr(e) {}
  ExtractField get_field() const { return field; }
  const Expr* get_from_expr() const { return from_expr.get(); }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const;
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const;
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;

 private:
  ExtractField field;
  std::shared_ptr<Analyzer::Expr> from_expr;
};

/*
 * @type DatetruncExpr
 * @brief the DATE_TRUNC expression
 */
class DatetruncExpr : public Expr {
 public:
  DatetruncExpr(const SQLTypeInfo& ti, bool has_agg, DatetruncField f, std::shared_ptr<Analyzer::Expr> e)
      : Expr(ti, has_agg), field(f), from_expr(e) {}
  DatetruncField get_field() const { return field; }
  const Expr* get_from_expr() const { return from_expr.get(); }
  virtual std::shared_ptr<Analyzer::Expr> deep_copy() const;
  virtual void check_group_by(const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const;
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const;
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const;
  virtual void collect_column_var(std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>& colvar_set,
                                  bool include_agg) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(const std::vector<TargetEntry*>& tlist) const;
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(const std::vector<TargetEntry*>& tlist) const;
  virtual bool operator==(const Expr& rhs) const;
  virtual void print() const;
  virtual void find_expr(bool (*f)(const Expr*), std::list<const Expr*>& expr_list) const;

 private:
  DatetruncField field;
  std::shared_ptr<Analyzer::Expr> from_expr;
};

/*
 * @type TargetEntry
 * @brief Target list defines a relational projection.  It is a list of TargetEntry's.
 */
class TargetEntry {
 public:
  TargetEntry(const std::string& n, std::shared_ptr<Analyzer::Expr> e, bool u) : resname(n), expr(e), unnest(u) {}
  virtual ~TargetEntry() {}
  const std::string& get_resname() const { return resname; }
  void set_resname(const std::string& name) { resname = name; }
  Expr* get_expr() const { return expr.get(); }
  std::shared_ptr<Expr> get_own_expr() const { return expr; }
  void set_expr(std::shared_ptr<Analyzer::Expr> e) { expr = e; }
  bool get_unnest() const { return unnest; }
  virtual void print() const;

 private:
  std::string resname;                   // alias name, e.g., SELECT salary + bonus AS compensation,
  std::shared_ptr<Analyzer::Expr> expr;  // expression to evaluate for the value
  bool unnest;                           // unnest a collection type
};

/*
 * @type RangeTblEntry
 * @brief Range table contains all the information about the tables/views
 * and columns referenced in a query.  It is a list of RangeTblEntry's.
 */
class RangeTblEntry {
 public:
  RangeTblEntry(const std::string& r, const TableDescriptor* t, Query* v) : rangevar(r), table_desc(t), view_query(v) {}
  virtual ~RangeTblEntry();
  /* @brief get_column_desc tries to find the column in column_descs and returns the column
   * descriptor if found.
   * otherwise, look up the column from Catalog, add the descriptor to column_descs and
   * return the descriptor.  return nullptr if not found
   * @param catalog the catalog for the current database
   * @param name name of column to look up
   */
  const ColumnDescriptor* get_column_desc(const Catalog_Namespace::Catalog& catalog, const std::string& name);
  const std::list<const ColumnDescriptor*>& get_column_descs() const { return column_descs; }
  const std::string& get_rangevar() const { return rangevar; }
  int32_t get_table_id() const { return table_desc->tableId; }
  const std::string& get_table_name() const { return table_desc->tableName; }
  const TableDescriptor* get_table_desc() const { return table_desc; }
  const Query* get_view_query() const { return view_query; }
  void expand_star_in_targetlist(const Catalog_Namespace::Catalog& catalog,
                                 std::vector<TargetEntry*>& tlist,
                                 int rte_idx);
  void add_all_column_descs(const Catalog_Namespace::Catalog& catalog);

 private:
  std::string rangevar;  // range variable name, e.g., FROM emp e, dept d
  const TableDescriptor* table_desc;
  std::list<const ColumnDescriptor*> column_descs;  // column descriptors for all columns referenced in this query
  Query* view_query;                                // parse tree for the view query
};

/*
 * @type OrderEntry
 * @brief represents an entry in ORDER BY clause.
 */
struct OrderEntry {
  OrderEntry(int t, bool d, bool nf) : tle_no(t), is_desc(d), nulls_first(nf){};
  ~OrderEntry() {}
  void print() const;
  int tle_no;       /* targetlist entry number: 1-based */
  bool is_desc;     /* true if order is DESC */
  bool nulls_first; /* true if nulls are ordered first.  otherwise last. */
};

/*
 * @type Query
 * @brief parse tree for a query
 */
class Query {
 public:
  Query()
      : is_distinct(false),
        where_predicate(nullptr),
        having_predicate(nullptr),
        order_by(nullptr),
        next_query(nullptr),
        is_unionall(false),
        stmt_type(kSELECT),
        num_aggs(0),
        result_table_id(0),
        limit(0),
        offset(0) {}
  virtual ~Query();
  bool get_is_distinct() const { return is_distinct; }
  int get_num_aggs() const { return num_aggs; }
  const std::vector<TargetEntry*>& get_targetlist() const { return targetlist; }
  std::vector<TargetEntry*>& get_targetlist_nonconst() { return targetlist; }
  const std::vector<RangeTblEntry*>& get_rangetable() const { return rangetable; }
  const Expr* get_where_predicate() const { return where_predicate.get(); }
  const std::list<std::shared_ptr<Analyzer::Expr>>& get_group_by() const { return group_by; };
  const Expr* get_having_predicate() const { return having_predicate.get(); }
  const std::list<OrderEntry>* get_order_by() const { return order_by; }
  const Query* get_next_query() const { return next_query; }
  SQLStmtType get_stmt_type() const { return stmt_type; }
  bool get_is_unionall() const { return is_unionall; }
  int get_result_table_id() const { return result_table_id; }
  const std::list<int>& get_result_col_list() const { return result_col_list; }
  void set_result_col_list(const std::list<int>& col_list) { result_col_list = col_list; }
  void set_result_table_id(int id) { result_table_id = id; }
  void set_is_distinct(bool d) { is_distinct = d; }
  void set_where_predicate(std::shared_ptr<Analyzer::Expr> p) { where_predicate = p; }
  void set_group_by(std::list<std::shared_ptr<Analyzer::Expr>>& g) { group_by = g; }
  void set_having_predicate(std::shared_ptr<Analyzer::Expr> p) { having_predicate = p; }
  void set_order_by(std::list<OrderEntry>* o) { order_by = o; }
  void set_next_query(Query* q) { next_query = q; }
  void set_is_unionall(bool u) { is_unionall = u; }
  void set_stmt_type(SQLStmtType t) { stmt_type = t; }
  void set_num_aggs(int a) { num_aggs = a; }
  int get_rte_idx(const std::string& range_var_name) const;
  RangeTblEntry* get_rte(int rte_idx) const { return rangetable[rte_idx]; }
  void add_rte(RangeTblEntry* rte);
  void add_tle(TargetEntry* tle) { targetlist.push_back(tle); }
  int64_t get_limit() const { return limit; }
  void set_limit(int64_t l) { limit = l; }
  int64_t get_offset() const { return offset; }
  void set_offset(int64_t o) { offset = o; }

 private:
  bool is_distinct;                                     // true only if SELECT DISTINCT
  std::vector<TargetEntry*> targetlist;                 // represents the SELECT clause
  std::vector<RangeTblEntry*> rangetable;               // represents the FROM clause for SELECT.  For INSERT,
                                                        // DELETE, UPDATE the result table is always the first
                                                        // entry in rangetable.
  std::shared_ptr<Analyzer::Expr> where_predicate;      // represents the WHERE clause
  std::list<std::shared_ptr<Analyzer::Expr>> group_by;  // represents the GROUP BY clause
  std::shared_ptr<Analyzer::Expr> having_predicate;     // represents the HAVING clause
  std::list<OrderEntry>* order_by;                      // represents the ORDER BY clause
  Query* next_query;                                    // the next query to UNION
  bool is_unionall;                                     // true only if it is UNION ALL
  SQLStmtType stmt_type;
  int num_aggs;                    // number of aggregate functions in query
  int result_table_id;             // for INSERT statements only
  std::list<int> result_col_list;  // for INSERT statement only
  int64_t limit;                   // row count for LIMIT clause.  0 means ALL
  int64_t offset;                  // offset in OFFSET clause.  0 means no offset.
};
}

#endif  // ANALYZER_H
