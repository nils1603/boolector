/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2012-2017 Mathias Preiner.
 *  Copyright (C) 2012-2017 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorslvfun.h"

#include "btorabort.h"
#include "btorbeta.h"
#include "btorclone.h"
#include "btorcore.h"
#include "btordbg.h"
#include "btordcr.h"
#include "btorexp.h"
#include "btorlog.h"
#include "btormodel.h"
#include "btoropt.h"
#include "btorslvprop.h"
#include "btorslvsls.h"
#include "utils/btorhashint.h"
#include "utils/btorhashptr.h"
#include "utils/btornodeiter.h"
#include "utils/btorstack.h"
#include "utils/btorutil.h"

/*------------------------------------------------------------------------*/

static BtorFunSolver *
clone_fun_solver (Btor *clone, BtorFunSolver *slv, BtorNodeMap *exp_map)
{
  assert (clone);
  assert (slv);
  assert (slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (exp_map);

  uint32_t h;
  Btor *btor;
  BtorFunSolver *res;

  btor = slv->btor;

  BTOR_NEW (clone->mm, res);
  memcpy (res, slv, sizeof (BtorFunSolver));

  res->btor   = clone;
  res->lemmas = btor_hashptr_table_clone (
      clone->mm, slv->lemmas, btor_clone_key_as_node, 0, exp_map, 0);

  btor_clone_node_ptr_stack (
      clone->mm, &slv->cur_lemmas, &res->cur_lemmas, exp_map);

  if (slv->score)
  {
    h = btor_opt_get (btor, BTOR_OPT_FUN_JUST_HEURISTIC);
    if (h == BTOR_JUST_HEUR_BRANCH_MIN_APP)
    {
      res->score = btor_hashptr_table_clone (clone->mm,
                                             slv->score,
                                             btor_clone_key_as_node,
                                             btor_clone_data_as_ptr_htable,
                                             exp_map,
                                             exp_map);
    }
    else
    {
      assert (h == BTOR_JUST_HEUR_BRANCH_MIN_DEP);
      res->score = btor_hashptr_table_clone (clone->mm,
                                             slv->score,
                                             btor_clone_key_as_node,
                                             btor_clone_data_as_int,
                                             exp_map,
                                             0);
    }
  }

  BTOR_INIT_STACK (clone->mm, res->stats.lemmas_size);
  if (BTOR_SIZE_STACK (slv->stats.lemmas_size) > 0)
  {
    BTOR_CNEWN (clone->mm,
                res->stats.lemmas_size.start,
                BTOR_SIZE_STACK (slv->stats.lemmas_size));

    res->stats.lemmas_size.end =
        res->stats.lemmas_size.start + BTOR_SIZE_STACK (slv->stats.lemmas_size);
    res->stats.lemmas_size.top = res->stats.lemmas_size.start
                                 + BTOR_COUNT_STACK (slv->stats.lemmas_size);
    memcpy (res->stats.lemmas_size.start,
            slv->stats.lemmas_size.start,
            BTOR_SIZE_STACK (slv->stats.lemmas_size) * sizeof (uint32_t));
  }

  return res;
}

static void
delete_fun_solver (BtorFunSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  BtorPtrHashTable *t;
  BtorPtrHashTableIterator it, iit;
  BtorNode *exp;
  Btor *btor;

  btor = slv->btor;

  btor_iter_hashptr_init (&it, slv->lemmas);
  while (btor_iter_hashptr_has_next (&it))
    btor_node_release (btor, btor_iter_hashptr_next (&it));
  btor_hashptr_table_delete (slv->lemmas);

  if (slv->score)
  {
    btor_iter_hashptr_init (&it, slv->score);
    while (btor_iter_hashptr_has_next (&it))
    {
      if (btor_opt_get (btor, BTOR_OPT_FUN_JUST_HEURISTIC)
          == BTOR_JUST_HEUR_BRANCH_MIN_APP)
      {
        t   = (BtorPtrHashTable *) it.bucket->data.as_ptr;
        exp = btor_iter_hashptr_next (&it);
        btor_node_release (btor, exp);

        btor_iter_hashptr_init (&iit, t);
        while (btor_iter_hashptr_has_next (&iit))
          btor_node_release (btor, btor_iter_hashptr_next (&iit));
        btor_hashptr_table_delete (t);
      }
      else
      {
        assert (btor_opt_get (btor, BTOR_OPT_FUN_JUST_HEURISTIC)
                == BTOR_JUST_HEUR_BRANCH_MIN_DEP);
        btor_node_release (btor, btor_iter_hashptr_next (&it));
      }
    }
    btor_hashptr_table_delete (slv->score);
  }

  BTOR_RELEASE_STACK (slv->cur_lemmas);
  BTOR_RELEASE_STACK (slv->stats.lemmas_size);
  BTOR_DELETE (btor->mm, slv);
  btor->slv = 0;
}

/*------------------------------------------------------------------------*/

static void
configure_sat_mgr (Btor *btor)
{
  BtorSATMgr *smgr;

  smgr = btor_get_sat_mgr (btor);
  if (btor_sat_is_initialized (smgr)) return;
  btor_sat_enable_solver (smgr);
  btor_sat_init (smgr);

  /* reset SAT solver to non-incremental if all functions have been
   * eliminated */
  if (!btor_opt_get (btor, BTOR_OPT_INCREMENTAL) && smgr->inc_required
      && btor->lambdas->count == 0 && btor->ufs->count == 0)
  {
    smgr->inc_required = false;
    BTOR_MSG (btor->msg,
              1,
              "no functions found, resetting SAT solver to non-incremental");
  }

  BTOR_ABORT (
      smgr->inc_required && !btor_sat_mgr_has_incremental_support (smgr),
      "selected SAT solver '%s' does not support incremental mode",
      smgr->name);
}

static BtorSolverResult
timed_sat_sat (Btor *btor, int32_t limit)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);

  double start, delta;
  BtorSolverResult res;
  BtorSATMgr *smgr;
  BtorAIGMgr *amgr;

  amgr = btor_get_aig_mgr (btor);
  BTOR_MSG (btor->msg,
            1,
            "%u AIG vars, %u AIG ands, %u CNF vars, %u CNF clauses",
            amgr->cur_num_aig_vars,
            amgr->cur_num_aigs,
            amgr->num_cnf_vars,
            amgr->num_cnf_clauses);
  smgr  = btor_get_sat_mgr (btor);
  start = btor_util_time_stamp ();
  res   = btor_sat_check_sat (smgr, limit);
  delta = btor_util_time_stamp () - start;
  BTOR_FUN_SOLVER (btor)->time.sat += delta;

  BTOR_MSG (
      btor->msg, 2, "SAT solver returns %d after %.1f seconds", res, delta);

  return res;
}

static bool
has_bv_assignment (Btor *btor, BtorNode *exp)
{
  exp = BTOR_REAL_ADDR_NODE (exp);
  return (btor->bv_model && btor_hashint_map_contains (btor->bv_model, exp->id))
         || BTOR_IS_SYNTH_NODE (exp) || btor_node_is_bv_const (exp);
}

static BtorBitVector *
get_bv_assignment (Btor *btor, BtorNode *exp)
{
  assert (btor->bv_model);
  assert (!BTOR_REAL_ADDR_NODE (exp)->parameterized);

  BtorNode *real_exp;
  BtorBitVector *bv, *result;
  BtorHashTableData *d;

  real_exp = BTOR_REAL_ADDR_NODE (exp);
  if ((d = btor_hashint_map_get (btor->bv_model, real_exp->id)))
    bv = btor_bv_copy (btor->mm, d->as_ptr);
  else /* cache assignment to avoid querying the sat solver multiple times */
  {
    /* synthesized nodes are always encoded and have an assignment */
    if (BTOR_IS_SYNTH_NODE (real_exp))
      bv = btor_bv_get_assignment (btor->mm, real_exp, false);
    else if (btor_node_is_bv_const (real_exp))
      bv = btor_bv_copy (btor->mm, btor_node_const_get_bits (real_exp));
    /* initialize var, apply, and feq nodes if they are not yet synthesized
     * and encoded (not in the BV skeleton yet, and thus unconstrained). */
    else if (btor_node_is_bv_var (real_exp) || btor_node_is_apply (real_exp)
             || btor_node_is_fun_eq (real_exp))
    {
      if (!BTOR_IS_SYNTH_NODE (real_exp))
        BTORLOG (1, "zero-initialize: %s", btor_util_node2string (real_exp));
      bv = btor_bv_get_assignment (btor->mm, real_exp, true);
    }
    else
      bv = btor_eval_exp (btor, real_exp);
    btor_model_add_to_bv (btor, btor->bv_model, real_exp, bv);
  }

  if (BTOR_IS_INVERTED_NODE (exp))
  {
    result = btor_bv_not (btor->mm, bv);
    btor_bv_free (btor->mm, bv);
  }
  else
    result = bv;

  return result;
}

/*------------------------------------------------------------------------*/

static Btor *
new_exp_layer_clone_for_dual_prop (Btor *btor,
                                   BtorNodeMap **exp_map,
                                   BtorNode **root)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (exp_map);
  assert (root);

  double start;
  Btor *clone;
  BtorNode *cur, *and;
  BtorPtrHashTableIterator it;

  /* empty formula */
  if (btor->unsynthesized_constraints->count == 0) return 0;

  start = btor_util_time_stamp ();
  clone = btor_clone_exp_layer (btor, exp_map);
  assert (!clone->synthesized_constraints->count);
  assert (clone->unsynthesized_constraints->count);

  btor_opt_set (clone, BTOR_OPT_MODEL_GEN, 0);
  btor_opt_set (clone, BTOR_OPT_INCREMENTAL, 1);
#ifndef NBTORLOG
  btor_opt_set (clone, BTOR_OPT_LOGLEVEL, 0);
#endif
  btor_opt_set (clone, BTOR_OPT_VERBOSITY, 0);
  btor_opt_set (clone, BTOR_OPT_FUN_DUAL_PROP, 0);

  assert (!btor_sat_is_initialized (btor_get_sat_mgr (clone)));
  btor_opt_set_str (clone, BTOR_OPT_SAT_ENGINE, "plain=1");
  configure_sat_mgr (clone);

  btor_iter_hashptr_init (&it, clone->unsynthesized_constraints);
  btor_iter_hashptr_queue (&it, clone->assumptions);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur                                   = btor_iter_hashptr_next (&it);
    BTOR_REAL_ADDR_NODE (cur)->constraint = 0;
    if (!*root)
    {
      *root = btor_node_copy (clone, cur);
    }
    else
    {
      and = btor_exp_and (clone, *root, cur);
      btor_node_release (clone, *root);
      *root = and;
    }
  }

  btor_iter_hashptr_init (&it, clone->unsynthesized_constraints);
  btor_iter_hashptr_queue (&it, clone->assumptions);
  while (btor_iter_hashptr_has_next (&it))
    btor_node_release (clone, btor_iter_hashptr_next (&it));
  btor_hashptr_table_delete (clone->unsynthesized_constraints);
  btor_hashptr_table_delete (clone->assumptions);
  clone->unsynthesized_constraints =
      btor_hashptr_table_new (clone->mm,
                              (BtorHashPtr) btor_node_hash_by_id,
                              (BtorCmpPtr) btor_node_compare_by_id);
  clone->assumptions =
      btor_hashptr_table_new (clone->mm,
                              (BtorHashPtr) btor_node_hash_by_id,
                              (BtorCmpPtr) btor_node_compare_by_id);

  BTOR_FUN_SOLVER (btor)->time.search_init_apps_cloning +=
      btor_util_time_stamp () - start;
  return clone;
}

static void
assume_inputs (Btor *btor,
               Btor *clone,
               BtorNodePtrStack *inputs,
               BtorNodeMap *exp_map,
               BtorNodeMap *key_map,
               BtorNodeMap *assumptions)
{
  assert (btor);
  assert (clone);
  assert (inputs);
  assert (exp_map);
  assert (key_map);
  assert (key_map->table->count == 0);
  assert (assumptions);

  uint32_t i;
  BtorNode *cur_btor, *cur_clone, *bv_const, *bv_eq;
  BtorBitVector *bv;

  for (i = 0; i < BTOR_COUNT_STACK (*inputs); i++)
  {
    cur_btor  = BTOR_PEEK_STACK (*inputs, i);
    cur_clone = btor_nodemap_mapped (exp_map, cur_btor);
    assert (cur_clone);
    assert (BTOR_IS_REGULAR_NODE (cur_clone));
    assert (!btor_nodemap_mapped (key_map, cur_clone));
    btor_nodemap_map (key_map, cur_clone, cur_btor);

    assert (BTOR_IS_REGULAR_NODE (cur_btor));
    bv       = get_bv_assignment (btor, cur_btor);
    bv_const = btor_exp_const (clone, bv);
    btor_bv_free (btor->mm, bv);
    bv_eq = btor_exp_eq (clone, cur_clone, bv_const);
    BTORLOG (1,
             "assume input: %s (%s)",
             btor_util_node2string (cur_btor),
             btor_util_node2string (bv_const));
    btor_assume_exp (clone, bv_eq);
    btor_nodemap_map (assumptions, bv_eq, cur_clone);
    btor_node_release (clone, bv_const);
    btor_node_release (clone, bv_eq);
  }
}

static BtorNode *
create_function_inequality (Btor *btor, BtorNode *feq)
{
  assert (BTOR_IS_REGULAR_NODE (feq));
  assert (btor_node_is_fun_eq (feq));

  BtorMemMgr *mm;
  BtorNode *var, *app0, *app1, *eq, *arg;
  BtorSortId funsort, sort;
  BtorNodePtrStack args;
  BtorTupleSortIterator it;

  mm = btor->mm;
  BTOR_INIT_STACK (mm, args);
  funsort = btor_sort_fun_get_domain (btor, btor_node_get_sort_id (feq->e[0]));

  btor_iter_tuple_sort_init (&it, btor, funsort);
  while (btor_iter_tuple_sort_has_next (&it))
  {
    sort = btor_iter_tuple_sort_next (&it);
    assert (btor_sort_is_bitvec (btor, sort));
    var = btor_exp_var (btor, sort, 0);
    BTOR_PUSH_STACK (args, var);
  }

  arg  = btor_exp_args (btor, args.start, BTOR_COUNT_STACK (args));
  app0 = btor_node_create_apply (btor, feq->e[0], arg);
  app1 = btor_node_create_apply (btor, feq->e[1], arg);
  eq   = btor_exp_eq (btor, app0, app1);

  btor_node_release (btor, arg);
  btor_node_release (btor, app0);
  btor_node_release (btor, app1);
  while (!BTOR_EMPTY_STACK (args))
    btor_node_release (btor, BTOR_POP_STACK (args));
  BTOR_RELEASE_STACK (args);

  return BTOR_INVERT_NODE (eq);
}

/* for every function equality f = g, add
 * f != g -> f(a) != g(a) */
static void
add_function_inequality_constraints (Btor *btor)
{
  uint32_t i;
  BtorNode *cur, *neq, *con;
  BtorNodePtrStack feqs, visit;
  BtorPtrHashTableIterator it;
  BtorPtrHashBucket *b;
  BtorMemMgr *mm;
  BtorIntHashTable *cache;

  mm = btor->mm;
  BTOR_INIT_STACK (mm, visit);
  /* we have to add inequality constraints for every function equality
   * in the formula (var_rhs and fun_rhs are still part of the formula). */
  btor_iter_hashptr_init (&it, btor->var_rhs);
  btor_iter_hashptr_queue (&it, btor->fun_rhs);
  btor_iter_hashptr_queue (&it, btor->unsynthesized_constraints);
  btor_iter_hashptr_queue (&it, btor->assumptions);
  assert (btor->embedded_constraints->count == 0);
  assert (btor->varsubst_constraints->count == 0);
  /* we don't have to traverse synthesized_constraints as we already created
   * inequality constraints for them in a previous sat call */
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    cur = btor_pointer_chase_simplified_exp (btor, cur);
    BTOR_PUSH_STACK (visit, cur);
  }

  /* collect all reachable function equalities */
  cache = btor_hashint_table_new (mm);
  BTOR_INIT_STACK (mm, feqs);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_hashint_table_contains (cache, cur->id)) continue;

    btor_hashint_table_add (cache, cur->id);
    if (btor_node_is_fun_eq (cur))
    {
      b = btor_hashptr_table_get (btor->feqs, cur);
      /* already visited and created inequality constraint in a previous
       * sat call */
      if (b->data.as_int) continue;
      BTOR_PUSH_STACK (feqs, cur);
      /* if the lambdas are not arrays, we cannot handle equalities */
      BTOR_ABORT (
          (btor_node_is_lambda (cur->e[0]) && !cur->e[0]->is_array)
              || (btor_node_is_lambda (cur->e[1]) && !cur->e[1]->is_array),
          "equality over non-array lambdas not supported yet");
    }
    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (visit, cur->e[i]);
  }

  /* add inequality constraint for every reachable function equality */
  while (!BTOR_EMPTY_STACK (feqs))
  {
    cur = BTOR_POP_STACK (feqs);
    assert (btor_node_is_fun_eq (cur));
    assert (!cur->parameterized);
    b = btor_hashptr_table_get (btor->feqs, cur);
    assert (b);
    assert (b->data.as_int == 0);
    b->data.as_int = 1;
    neq            = create_function_inequality (btor, cur);
    con            = btor_exp_implies (btor, BTOR_INVERT_NODE (cur), neq);
    btor_assert_exp (btor, con);
    btor_node_release (btor, con);
    btor_node_release (btor, neq);
    BTORLOG (
        2, "add inequality constraint for %s", btor_util_node2string (cur));
  }
  BTOR_RELEASE_STACK (visit);
  BTOR_RELEASE_STACK (feqs);
  btor_hashint_table_delete (cache);
}

static int32_t
sat_aux_btor_dual_prop (Btor *btor)
{
  assert (btor);

  BtorSolverResult result;

  if (btor->inconsistent) goto DONE;

  BTOR_MSG (btor->msg, 1, "calling SAT");
  configure_sat_mgr (btor);

  if (btor->valid_assignments == 1) btor_reset_incremental_usage (btor);

  if (btor->feqs->count > 0) add_function_inequality_constraints (btor);

  assert (btor->synthesized_constraints->count == 0);
  assert (btor->unsynthesized_constraints->count == 0);
  assert (btor->embedded_constraints->count == 0);
  assert (btor_dbg_check_all_hash_tables_proxy_free (btor));
  assert (btor_dbg_check_all_hash_tables_simp_free (btor));
  assert (btor_dbg_check_assumptions_simp_free (btor));

  btor_add_again_assumptions (btor);

  result = timed_sat_sat (btor, -1);

  assert (result == BTOR_RESULT_UNSAT
          || (btor_terminate (btor) && result == BTOR_RESULT_UNKNOWN));

DONE:
  result                  = BTOR_RESULT_UNSAT;
  btor->valid_assignments = 1;

  btor->last_sat_result = result;
  return result;
}

static void
collect_applies (Btor *btor,
                 Btor *clone,
                 BtorNodeMap *key_map,
                 BtorNodeMap *assumptions,
                 BtorIntHashTable *top_applies,
                 BtorNodePtrStack *top_applies_feq)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (clone);
  assert (key_map);
  assert (assumptions);
  assert (top_applies);
  assert (top_applies_feq);

  double start;
  uint32_t i;
  BtorMemMgr *mm;
  BtorFunSolver *slv;
  BtorNode *cur_btor, *cur_clone, *bv_eq;
  BtorNodePtrStack failed_eqs;
  BtorNodeMapIterator it;
  BtorIntHashTable *mark;

  start = btor_util_time_stamp ();

  mm   = btor->mm;
  slv  = BTOR_FUN_SOLVER (btor);
  mark = btor_hashint_table_new (mm);

  BTOR_INIT_STACK (mm, failed_eqs);

  btor_iter_nodemap_init (&it, assumptions);
  while (btor_iter_nodemap_has_next (&it))
  {
    bv_eq     = btor_iter_nodemap_next (&it);
    cur_clone = btor_nodemap_mapped (assumptions, bv_eq);
    assert (cur_clone);
    /* Note: node mapping is normalized, revert */
    if (BTOR_IS_INVERTED_NODE (cur_clone))
    {
      bv_eq     = BTOR_INVERT_NODE (bv_eq);
      cur_clone = BTOR_INVERT_NODE (cur_clone);
    }
    cur_btor = btor_nodemap_mapped (key_map, cur_clone);
    assert (cur_btor);
    assert (BTOR_IS_REGULAR_NODE (cur_btor));
    assert (btor_node_is_bv_var (cur_btor) || btor_node_is_apply (cur_btor)
            || btor_node_is_fun_eq (cur_btor));

    if (btor_node_is_bv_var (cur_btor))
      slv->stats.dp_assumed_vars += 1;
    else if (btor_node_is_fun_eq (cur_btor))
      slv->stats.dp_assumed_eqs += 1;
    else
    {
      assert (btor_node_is_apply (cur_btor));
      slv->stats.dp_assumed_applies += 1;
    }

    if (btor_failed_exp (clone, bv_eq))
    {
      BTORLOG (1, "failed: %s", btor_util_node2string (cur_btor));
      if (btor_node_is_bv_var (cur_btor))
        slv->stats.dp_failed_vars += 1;
      else if (btor_node_is_fun_eq (cur_btor))
      {
        slv->stats.dp_failed_eqs += 1;
        BTOR_PUSH_STACK (failed_eqs, cur_btor);
      }
      else
      {
        assert (btor_node_is_apply (cur_btor));
        if (btor_hashint_table_contains (mark, cur_btor->id)) continue;
        slv->stats.dp_failed_applies += 1;
        btor_hashint_table_add (mark, cur_btor->id);
        btor_hashint_table_add (top_applies, cur_btor->id);
      }
    }
  }

  btor_hashint_table_delete (mark);
  mark = btor_hashint_table_new (mm);

  /* collect applies below failed function equalities */
  while (!BTOR_EMPTY_STACK (failed_eqs))
  {
    cur_btor = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (failed_eqs));

    if (!cur_btor->apply_below
        || btor_hashint_table_contains (mark, cur_btor->id))
      continue;

    btor_hashint_table_add (mark, cur_btor->id);

    /* we only need the "top applies" below a failed function equality */
    if (!cur_btor->parameterized && btor_node_is_apply (cur_btor))
    {
      BTORLOG (1, "apply below eq: %s", btor_util_node2string (cur_btor));
      if (!btor_hashint_table_contains (top_applies, cur_btor->id))
      {
        BTOR_PUSH_STACK (*top_applies_feq, cur_btor);
        btor_hashint_table_add (top_applies, cur_btor->id);
      }
      continue;
    }

    for (i = 0; i < cur_btor->arity; i++)
      BTOR_PUSH_STACK (failed_eqs, cur_btor->e[i]);
  }
  BTOR_RELEASE_STACK (failed_eqs);
  btor_hashint_table_delete (mark);
  slv->time.search_init_apps_collect_fa += btor_util_time_stamp () - start;
}

static void
set_up_dual_and_collect (Btor *btor,
                         Btor *clone,
                         BtorNode *clone_root,
                         BtorNodeMap *exp_map,
                         BtorNodePtrStack *inputs,
                         BtorNodePtrStack *top_applies)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (clone);
  assert (clone_root);
  assert (exp_map);
  assert (inputs);
  assert (top_applies);

  double delta;
  uint32_t i;
  BtorNode *cur;
  BtorFunSolver *slv;
  BtorNodeMap *assumptions, *key_map;
  BtorNodePtrStack sorted, topapps_feq;
  BtorIntHashTable *topapps;

  delta = btor_util_time_stamp ();
  slv   = BTOR_FUN_SOLVER (btor);

  assumptions = btor_nodemap_new (btor);
  key_map     = btor_nodemap_new (btor);

  BTOR_INIT_STACK (btor->mm, sorted);
  BTOR_FIT_STACK (sorted, BTOR_COUNT_STACK (*inputs));
  memcpy (sorted.start,
          inputs->start,
          sizeof (BtorNode *) * BTOR_COUNT_STACK (*inputs));
  sorted.top = sorted.start + BTOR_COUNT_STACK (*inputs);

  BTOR_INIT_STACK (btor->mm, topapps_feq);
  topapps = btor_hashint_table_new (btor->mm);

  /* assume root */
  btor_assume_exp (clone, BTOR_INVERT_NODE (clone_root));

  /* assume assignments of bv vars and applies, partial assignments are
   * assumed as partial assignment (as slice on resp. var/apply) */
  switch (btor_opt_get (btor, BTOR_OPT_FUN_DUAL_PROP_QSORT))
  {
    case BTOR_DP_QSORT_ASC:
      qsort (sorted.start,
             BTOR_COUNT_STACK (sorted),
             sizeof (BtorNode *),
             btor_node_compare_by_id_qsort_asc);
      break;
    case BTOR_DP_QSORT_DESC:
      qsort (sorted.start,
             BTOR_COUNT_STACK (sorted),
             sizeof (BtorNode *),
             btor_node_compare_by_id_qsort_desc);
      break;
    default:
      assert (btor_opt_get (btor, BTOR_OPT_FUN_DUAL_PROP_QSORT)
              == BTOR_DP_QSORT_JUST);
      btor_dcr_compute_scores_dual_prop (btor);
      qsort (sorted.start,
             BTOR_COUNT_STACK (sorted),
             sizeof (BtorNode *),
             btor_dcr_compare_scores_qsort);
  }
  assume_inputs (btor, clone, &sorted, exp_map, key_map, assumptions);
  slv->time.search_init_apps_collect_var_apps +=
      btor_util_time_stamp () - delta;

  /* let solver determine failed assumptions */
  delta = btor_util_time_stamp ();
  sat_aux_btor_dual_prop (clone);
  assert (clone->last_sat_result == BTOR_RESULT_UNSAT);
  slv->time.search_init_apps_sat += btor_util_time_stamp () - delta;

  /* extract partial model via failed assumptions */
  collect_applies (btor, clone, key_map, assumptions, topapps, &topapps_feq);

  for (i = 0; i < BTOR_COUNT_STACK (*inputs); i++)
  {
    cur = BTOR_PEEK_STACK (*inputs, i);
    if (btor_hashint_table_contains (topapps, BTOR_REAL_ADDR_NODE (cur)->id))
      BTOR_PUSH_STACK (*top_applies, cur);
  }
  for (i = 0; i < BTOR_COUNT_STACK (topapps_feq); i++)
    BTOR_PUSH_STACK (*top_applies, BTOR_PEEK_STACK (topapps_feq, i));

  BTOR_RELEASE_STACK (sorted);
  BTOR_RELEASE_STACK (topapps_feq);
  btor_hashint_table_delete (topapps);
  btor_nodemap_delete (assumptions);
  btor_nodemap_delete (key_map);
}

static void
search_initial_applies_dual_prop (Btor *btor,
                                  Btor *clone,
                                  BtorNode *clone_root,
                                  BtorNodeMap *exp_map,
                                  BtorNodePtrStack *top_applies)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (clone);
  assert (clone_root);
  assert (exp_map);
  assert (top_applies);

  double start;
  uint32_t i;
  BtorNode *cur;
  BtorNodePtrStack stack, inputs;
  BtorPtrHashTableIterator it;
  BtorSATMgr *smgr;
  BtorFunSolver *slv;
  BtorIntHashTable *mark;
  BtorMemMgr *mm;

  start = btor_util_time_stamp ();

  BTORLOG (1, "");
  BTORLOG (1, "*** search initial applies");

  mm                            = btor->mm;
  slv                           = BTOR_FUN_SOLVER (btor);
  slv->stats.dp_failed_vars     = 0;
  slv->stats.dp_assumed_vars    = 0;
  slv->stats.dp_failed_applies  = 0;
  slv->stats.dp_assumed_applies = 0;

  smgr = btor_get_sat_mgr (btor);
  if (!smgr->inc_required) return;

  mark = btor_hashint_table_new (mm);
  BTOR_INIT_STACK (mm, stack);
  BTOR_INIT_STACK (mm, inputs);

  btor_iter_hashptr_init (&it, btor->synthesized_constraints);
  btor_iter_hashptr_queue (&it, btor->assumptions);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    BTOR_PUSH_STACK (stack, cur);

    while (!BTOR_EMPTY_STACK (stack))
    {
      cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (stack));

      if (btor_hashint_table_contains (mark, cur->id)) continue;

      btor_hashint_table_add (mark, cur->id);
      if (btor_node_is_bv_var (cur) || btor_node_is_fun_eq (cur)
          || btor_node_is_apply (cur))
      {
        assert (BTOR_IS_SYNTH_NODE (cur));
        BTOR_PUSH_STACK (inputs, cur);
        continue;
      }

      for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (stack, cur->e[i]);
    }
  }

  (void) btor_node_compare_by_id_qsort_asc;

  set_up_dual_and_collect (
      btor, clone, clone_root, exp_map, &inputs, top_applies);

  BTOR_RELEASE_STACK (stack);
  BTOR_RELEASE_STACK (inputs);
  btor_hashint_table_delete (mark);

  slv->time.search_init_apps += btor_util_time_stamp () - start;
}

static void
add_lemma_to_dual_prop_clone (Btor *btor,
                              Btor *clone,
                              BtorNode **root,
                              BtorNode *lemma,
                              BtorNodeMap *exp_map)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (clone);
  assert (lemma);

  BtorNode *clemma, *and;

  /* clone and rebuild lemma with rewrite level 0 (as we want the exact
   * expression) */
  clemma = btor_clone_recursively_rebuild_exp (btor, clone, lemma, exp_map, 0);
  assert (clemma);
  and = btor_exp_and (clone, *root, clemma);
  btor_node_release (clone, clemma);
  btor_node_release (clone, *root);
  *root = and;
}

/*------------------------------------------------------------------------*/

static void
search_initial_applies_bv_skeleton (Btor *btor,
                                    BtorNodePtrStack *applies,
                                    BtorIntHashTable *cache)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (applies);

  double start;
  uint32_t i;
  BtorNode *cur;
  BtorNodePtrStack stack;
  BtorPtrHashTableIterator it;
  BtorMemMgr *mm;

  start = btor_util_time_stamp ();

  BTORLOG (1, "");
  BTORLOG (1, "*** search initial applies");

  mm = btor->mm;
  BTOR_INIT_STACK (mm, stack);

  btor_iter_hashptr_init (&it, btor->synthesized_constraints);
  btor_iter_hashptr_queue (&it, btor->assumptions);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    BTOR_PUSH_STACK (stack, cur);

    while (!BTOR_EMPTY_STACK (stack))
    {
      cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (stack));

      if (btor_hashint_table_contains (cache, cur->id)) continue;

      btor_hashint_table_add (cache, cur->id);

      if (btor_node_is_apply (cur) && !cur->parameterized)
      {
        //	      assert (BTOR_IS_SYNTH_NODE (cur));
        BTORLOG (1, "initial apply: %s", btor_util_node2string (cur));
        BTOR_PUSH_STACK (*applies, cur);
        continue;
      }

      for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (stack, cur->e[i]);
    }
  }

  BTOR_RELEASE_STACK (stack);
  BTOR_FUN_SOLVER (btor)->time.search_init_apps +=
      btor_util_time_stamp () - start;
}

static void
search_initial_applies_just (Btor *btor, BtorNodePtrStack *top_applies)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (top_applies);
  assert (btor->unsynthesized_constraints->count == 0);
  assert (btor->embedded_constraints->count == 0);

  uint32_t i, h;
  int32_t a, a0, a1;
  double start;
  BtorNode *cur, *e0, *e1;
  BtorPtrHashTableIterator it;
  BtorNodePtrStack stack;
  BtorAIGMgr *amgr;
  BtorIntHashTable *mark;
  BtorMemMgr *mm;

  start = btor_util_time_stamp ();

  BTORLOG (1, "");
  BTORLOG (1, "*** search initial applies");

  mm   = btor->mm;
  amgr = btor_get_aig_mgr (btor);
  h    = btor_opt_get (btor, BTOR_OPT_FUN_JUST_HEURISTIC);
  mark = btor_hashint_table_new (mm);

  BTOR_INIT_STACK (mm, stack);

  btor_dcr_compute_scores (btor);

  btor_iter_hashptr_init (&it, btor->synthesized_constraints);
  btor_iter_hashptr_queue (&it, btor->assumptions);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    BTOR_PUSH_STACK (stack, cur);

    while (!BTOR_EMPTY_STACK (stack))
    {
      cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (stack));

      if (btor_hashint_table_contains (mark, cur->id)) continue;

      btor_hashint_table_add (mark, cur->id);

      if (btor_node_is_apply (cur) && !cur->parameterized)
      {
        BTORLOG (1, "initial apply: %s", btor_util_node2string (cur));
        BTOR_PUSH_STACK (*top_applies, cur);
        continue;
      }

      if (!cur->parameterized && !btor_node_is_fun (cur)
          && btor_node_get_width (btor, cur) == 1)
      {
        switch (cur->kind)
        {
          case BTOR_FUN_EQ_NODE:
            a = BTOR_IS_SYNTH_NODE (cur)
                    ? btor_aig_get_assignment (amgr, cur->av->aigs[0])
                    : 0;  // 'x';

            if (a == 1 || a == 0) goto PUSH_CHILDREN;
            /* if equality is false (-1), we do not need to check
             * applies below for consistency as it is sufficient to
             * check the witnesses of inequality */
            break;

          case BTOR_AND_NODE:

            a = BTOR_IS_SYNTH_NODE (cur)
                    ? btor_aig_get_assignment (amgr, cur->av->aigs[0])
                    : 0;  // 'x'

            e0 = BTOR_REAL_ADDR_NODE (cur->e[0]);
            e1 = BTOR_REAL_ADDR_NODE (cur->e[1]);

            a0 = BTOR_IS_SYNTH_NODE (e0)
                     ? btor_aig_get_assignment (amgr, e0->av->aigs[0])
                     : 0;  // 'x'
            if (a0 && BTOR_IS_INVERTED_NODE (cur->e[0])) a0 *= -1;

            a1 = BTOR_IS_SYNTH_NODE (e1)
                     ? btor_aig_get_assignment (amgr, e1->av->aigs[0])
                     : 0;  // 'x'
            if (a1 && BTOR_IS_INVERTED_NODE (cur->e[1])) a1 *= -1;

            if (a != -1)  // and = 1 or x
            {
              BTOR_PUSH_STACK (stack, cur->e[0]);
              BTOR_PUSH_STACK (stack, cur->e[1]);
            }
            else  // and = 0
            {
              if (a0 == -1 && a1 == -1)  // both inputs 0
              {
                /* branch selection w.r.t selected heuristic */
                if (h == BTOR_JUST_HEUR_BRANCH_MIN_APP
                    || h == BTOR_JUST_HEUR_BRANCH_MIN_DEP)
                {
                  if (btor_dcr_compare_scores (btor, cur->e[0], cur->e[1]))
                    BTOR_PUSH_STACK (stack, cur->e[0]);
                  else
                    BTOR_PUSH_STACK (stack, cur->e[1]);
                }
                else
                {
                  assert (h == BTOR_JUST_HEUR_LEFT);
                  BTOR_PUSH_STACK (stack, cur->e[0]);
                }
              }
              else if (a0 == -1)  // only one input 0
                BTOR_PUSH_STACK (stack, cur->e[0]);
              else if (a1 == -1)  // only one input 0
                BTOR_PUSH_STACK (stack, cur->e[1]);
              else if (a0 == 0 && a1 == 1)  // first input x, second 0
                BTOR_PUSH_STACK (stack, cur->e[0]);
              else if (a0 == 1 && a1 == 0)  // first input 0, second x
                BTOR_PUSH_STACK (stack, cur->e[1]);
              else  // both inputs x
              {
                assert (a0 == 0);
                assert (a1 == 0);
                BTOR_PUSH_STACK (stack, cur->e[0]);
                BTOR_PUSH_STACK (stack, cur->e[1]);
              }
            }
            break;

#if 0
		  case BTOR_BCOND_NODE:
		    BTOR_PUSH_STACK (stack, cur->e[0]);
		    a = BTOR_IS_SYNTH_NODE (BTOR_REAL_ADDR_NODE (cur->e[0]))
			? btor_aig_get_assignment (
			    amgr, BTOR_REAL_ADDR_NODE (cur->e[0])->av->aigs[0])
			: 0;  // 'x';
		    if (BTOR_IS_INVERTED_NODE (cur->e[0])) a *= -1;
		    if (a == 1)  // then
		      BTOR_PUSH_STACK (stack, cur->e[1]);
		    else if (a == -1)
		      BTOR_PUSH_STACK (stack, cur->e[2]);
		    else                   // else
		      {
			BTOR_PUSH_STACK (stack, cur->e[1]);
			BTOR_PUSH_STACK (stack, cur->e[2]);
		      }
		    break;
#endif

          default: goto PUSH_CHILDREN;
        }
      }
      else
      {
      PUSH_CHILDREN:
        for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (stack, cur->e[i]);
      }
    }
  }

  BTOR_RELEASE_STACK (stack);
  btor_hashint_table_delete (mark);

  BTOR_FUN_SOLVER (btor)->time.search_init_apps +=
      btor_util_time_stamp () - start;
}

static bool
equal_bv_assignments (BtorNode *exp0, BtorNode *exp1)
{
  bool equal;
  Btor *btor;
  BtorBitVector *bv0, *bv1;

  btor  = BTOR_REAL_ADDR_NODE (exp0)->btor;
  bv0   = get_bv_assignment (btor, exp0);
  bv1   = get_bv_assignment (btor, exp1);
  equal = btor_bv_compare (bv0, bv1) == 0;
  btor_bv_free (btor->mm, bv0);
  btor_bv_free (btor->mm, bv1);
  return equal;
}

static int32_t
compare_args_assignments (BtorNode *e0, BtorNode *e1)
{
  assert (BTOR_IS_REGULAR_NODE (e0));
  assert (BTOR_IS_REGULAR_NODE (e1));
  assert (btor_node_is_args (e0));
  assert (btor_node_is_args (e1));

  bool equal;
  BtorBitVector *bv0, *bv1;
  BtorNode *arg0, *arg1;
  Btor *btor;
  BtorArgsIterator it0, it1;
  btor = e0->btor;

  if (btor_node_get_sort_id (e0) != btor_node_get_sort_id (e1)) return 1;

  btor_iter_args_init (&it0, e0);
  btor_iter_args_init (&it1, e1);

  while (btor_iter_args_has_next (&it0))
  {
    assert (btor_iter_args_has_next (&it1));
    arg0 = btor_iter_args_next (&it0);
    arg1 = btor_iter_args_next (&it1);

    bv0 = get_bv_assignment (btor, arg0);
    bv1 = get_bv_assignment (btor, arg1);

    equal = btor_bv_compare (bv0, bv1) == 0;
    btor_bv_free (btor->mm, bv0);
    btor_bv_free (btor->mm, bv1);

    if (!equal) return 1;
  }

  return 0;
}

static uint32_t
hash_args_assignment (BtorNode *exp)
{
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor_node_is_args (exp));

  uint32_t hash;
  Btor *btor;
  BtorNode *arg;
  BtorArgsIterator it;
  BtorBitVector *bv;

  btor = exp->btor;
  hash = 0;
  btor_iter_args_init (&it, exp);
  while (btor_iter_args_has_next (&it))
  {
    arg = btor_iter_args_next (&it);
    bv  = get_bv_assignment (btor, arg);
    hash += btor_bv_hash (bv);
    btor_bv_free (btor->mm, bv);
  }
  return hash;
}

static void
collect_premisses (Btor *btor,
                   BtorNode *from,
                   BtorNode *to,
                   BtorNode *args,
                   BtorPtrHashTable *cond_sel_if,
                   BtorPtrHashTable *cond_sel_else)
{
  assert (btor);
  assert (from);
  assert (to);
  assert (cond_sel_if);
  assert (cond_sel_else);
  assert (BTOR_IS_REGULAR_NODE (from));
  assert (BTOR_IS_REGULAR_NODE (args));
  assert (btor_node_is_args (args));
  assert (BTOR_IS_REGULAR_NODE (to));

  BTORLOG (1,
           "%s: %s, %s, %s",
           __FUNCTION__,
           btor_util_node2string (from),
           btor_util_node2string (to),
           btor_util_node2string (args));

  BtorMemMgr *mm;
  BtorNode *fun, *result;
  BtorPtrHashTable *t;
  BtorBitVector *bv_assignment;

  mm = btor->mm;

  /* follow propagation path and collect all conditions that have been
   * evaluated during propagation */
  if (btor_node_is_apply (from))
  {
    assert (BTOR_IS_REGULAR_NODE (to));
    assert (btor_node_is_fun (to));

    fun = from->e[0];

    for (;;)
    {
      assert (BTOR_IS_REGULAR_NODE (fun));
      assert (btor_node_is_fun (fun));

      if (fun == to) break;

      if (btor_node_is_fun_cond (fun))
      {
        bv_assignment = get_bv_assignment (btor, fun->e[0]);

        /* propagate over function ite */
        if (btor_bv_is_true (bv_assignment))
        {
          result = fun->e[1];
          t      = cond_sel_if;
        }
        else
        {
          result = fun->e[2];
          t      = cond_sel_else;
        }
        btor_bv_free (mm, bv_assignment);
        if (!btor_hashptr_table_get (t, fun->e[0]))
          btor_hashptr_table_add (t, btor_node_copy (btor, fun->e[0]));
        fun = result;
        continue;
      }

      assert (btor_node_is_lambda (fun));

      btor_beta_assign_args (btor, fun, args);
      result = btor_beta_reduce_partial_collect (
          btor, fun, cond_sel_if, cond_sel_else);
      btor_beta_unassign_params (btor, fun);
      result = BTOR_REAL_ADDR_NODE (result);

      assert (btor_node_is_apply (result));
      assert (result->e[1] == args);

      fun = result->e[0];
      btor_node_release (btor, result);
    }
  }
  else
  {
    assert (btor_node_is_lambda (from));
    fun = from;

    btor_beta_assign_args (btor, fun, args);
    result = btor_beta_reduce_partial_collect (
        btor, fun, cond_sel_if, cond_sel_else);
    btor_beta_unassign_params (btor, fun);
    assert (BTOR_REAL_ADDR_NODE (result) == to);
    btor_node_release (btor, result);
  }
}

static void
add_symbolic_lemma (Btor *btor,
                    BtorPtrHashTable *bconds_sel1,
                    BtorPtrHashTable *bconds_sel2,
                    BtorNode *a,
                    BtorNode *b,
                    BtorNode *args0,
                    BtorNode *args1)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (bconds_sel1);
  assert (bconds_sel2);
  assert (a);
  assert (b);
  assert (BTOR_IS_REGULAR_NODE (a));
  assert (btor_node_is_apply (a));
  assert (BTOR_IS_REGULAR_NODE (args0));
  assert (btor_node_is_args (args0));
  assert (!args1 || BTOR_IS_REGULAR_NODE (b));
  assert (!args1 || btor_node_is_apply (b));
  assert (!args1 || BTOR_IS_REGULAR_NODE (args1));
  assert (!args1 || btor_node_is_args (args1));
  assert (!a->parameterized);
  assert (!BTOR_REAL_ADDR_NODE (b)->parameterized);

  uint32_t lemma_size = 0;
  BtorFunSolver *slv;
  BtorNode *cond, *eq, *and, *arg0, *arg1;
  BtorNode *premise = 0, *conclusion = 0, *lemma;
  BtorArgsIterator it0, it1;
  BtorPtrHashTableIterator it;

  slv = BTOR_FUN_SOLVER (btor);

  BTORLOG (2, "lemma:");
  /* function congruence axiom conflict:
   *   apply arguments: a_0,...,a_n, b_0,...,b_n
   *   encode premisses: \forall i <= n . /\ a_i = b_i */
  if (args1)
  {
    assert (btor_node_get_sort_id (args0) == btor_node_get_sort_id (args1));

    btor_iter_args_init (&it0, args0);
    btor_iter_args_init (&it1, args1);

    while (btor_iter_args_has_next (&it0))
    {
      assert (btor_iter_args_has_next (&it1));
      arg0 = btor_iter_args_next (&it0);
      arg1 = btor_iter_args_next (&it1);
      BTORLOG (2,
               "  p %s = %s",
               btor_util_node2string (arg0),
               btor_util_node2string (arg1));
      eq = btor_exp_eq (btor, arg0, arg1);
      if (premise)
      {
        and = btor_exp_and (btor, premise, eq);
        btor_node_release (btor, premise);
        btor_node_release (btor, eq);
        premise = and;
      }
      else
        premise = eq;

      lemma_size += 1;
    }
  }
  /* else beta reduction conflict */

  /* encode conclusion a = b */
  conclusion = btor_exp_eq (btor, a, b);

  lemma_size += 1; /* a == b */
  lemma_size += bconds_sel1->count;
  lemma_size += bconds_sel2->count;

  /* premisses bv conditions:
   *   true conditions: c_0, ..., c_k
   *   encode premisses: \forall i <= k. /\ c_i */
  btor_iter_hashptr_init (&it, bconds_sel1);
  while (btor_iter_hashptr_has_next (&it))
  {
    cond = btor_iter_hashptr_next (&it);
    BTORLOG (1, "  p %s", btor_util_node2string (cond));
    assert (btor_node_get_width (btor, cond) == 1);
    assert (!BTOR_REAL_ADDR_NODE (cond)->parameterized);
    if (premise)
    {
      and = btor_exp_and (btor, premise, cond);
      btor_node_release (btor, premise);
      premise = and;
    }
    else
      premise = btor_node_copy (btor, cond);
    btor_node_release (btor, cond);
  }

  /* premisses bv conditions:
   *   false conditions: c_0, ..., c_l
   *   encode premisses: \forall i <= l. /\ \not c_i */
  btor_iter_hashptr_init (&it, bconds_sel2);
  while (btor_iter_hashptr_has_next (&it))
  {
    cond = btor_iter_hashptr_next (&it);
    BTORLOG (2, "  p %s", btor_util_node2string (BTOR_INVERT_NODE (cond)));
    assert (btor_node_get_width (btor, cond) == 1);
    assert (!BTOR_REAL_ADDR_NODE (cond)->parameterized);
    if (premise)
    {
      and = btor_exp_and (btor, premise, BTOR_INVERT_NODE (cond));
      btor_node_release (btor, premise);
      premise = and;
    }
    else
      premise = btor_node_copy (btor, BTOR_INVERT_NODE (cond));
    btor_node_release (btor, cond);
  }
  BTORLOG (
      2, "  c %s = %s", btor_util_node2string (a), btor_util_node2string (b));

  assert (conclusion);
  if (premise)
  {
    lemma = btor_exp_implies (btor, premise, conclusion);
    btor_node_release (btor, premise);
  }
  else
    lemma = btor_node_copy (btor, conclusion);

  /* delaying lemmas may in some cases produce the same lemmas with different *
   * conflicts */
  if (!btor_hashptr_table_get (slv->lemmas, lemma))
  {
    BTORLOG (2, "  lemma: %s", btor_util_node2string (lemma));
    btor_hashptr_table_add (slv->lemmas, btor_node_copy (btor, lemma));
    BTOR_PUSH_STACK (slv->cur_lemmas, lemma);
    slv->stats.lod_refinements++;
    slv->stats.lemmas_size_sum += lemma_size;
    if (lemma_size >= BTOR_SIZE_STACK (slv->stats.lemmas_size))
      BTOR_FIT_STACK (slv->stats.lemmas_size, lemma_size);
    slv->stats.lemmas_size.start[lemma_size] += 1;
  }
  btor_node_release (btor, lemma);
  btor_node_release (btor, conclusion);
}

static void
add_lemma (Btor *btor, BtorNode *fun, BtorNode *app0, BtorNode *app1)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (fun);
  assert (app0);
  assert (BTOR_IS_REGULAR_NODE (fun));
  assert (btor_node_is_fun (fun));
  assert (!fun->parameterized);
  assert (BTOR_IS_REGULAR_NODE (app0));
  assert (btor_node_is_apply (app0));
  assert (!app1 || BTOR_IS_REGULAR_NODE (app1));
  assert (!app1 || btor_node_is_apply (app1));

  double start;
  BtorPtrHashTable *cond_sel_if, *cond_sel_else;
  BtorNode *args, *value, *exp;
  BtorMemMgr *mm;

  mm    = btor->mm;
  start = btor_util_time_stamp ();

  /* collect intermediate conditions of bit vector conditionals */
  cond_sel_if   = btor_hashptr_table_new (mm,
                                        (BtorHashPtr) btor_node_hash_by_id,
                                        (BtorCmpPtr) btor_node_compare_by_id);
  cond_sel_else = btor_hashptr_table_new (mm,
                                          (BtorHashPtr) btor_node_hash_by_id,
                                          (BtorCmpPtr) btor_node_compare_by_id);

  /* function congruence axiom conflict */
  if (app1)
  {
    for (exp = app0; exp; exp = exp == app0 ? app1 : 0)
    {
      assert (exp);
      assert (btor_node_is_apply (exp));
      args = exp->e[1];
      /* path from exp to conflicting fun */
      collect_premisses (btor, exp, fun, args, cond_sel_if, cond_sel_else);
    }
    add_symbolic_lemma (
        btor, cond_sel_if, cond_sel_else, app0, app1, app0->e[1], app1->e[1]);
  }
  /* beta reduction conflict */
  else
  {
    args = app0->e[1];
    btor_beta_assign_args (btor, fun, args);
    value = btor_beta_reduce_partial (btor, fun, 0);
    btor_beta_unassign_params (btor, fun);
    assert (!btor_node_is_lambda (value));

    /* path from app0 to conflicting fun */
    collect_premisses (btor, app0, fun, args, cond_sel_if, cond_sel_else);

    /* path from conflicting fun to value */
    collect_premisses (btor,
                       fun,
                       BTOR_REAL_ADDR_NODE (value),
                       args,
                       cond_sel_if,
                       cond_sel_else);

    add_symbolic_lemma (
        btor, cond_sel_if, cond_sel_else, app0, value, app0->e[1], 0);
    btor_node_release (btor, value);
  }

  btor_hashptr_table_delete (cond_sel_if);
  btor_hashptr_table_delete (cond_sel_else);
  BTOR_FUN_SOLVER (btor)->time.lemma_gen += btor_util_time_stamp () - start;
}

static void
push_applies_for_propagation (Btor *btor,
                              BtorNode *exp,
                              BtorNodePtrStack *prop_stack,
                              BtorIntHashTable *apply_search_cache)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (exp);
  assert (prop_stack);

  uint32_t i;
  double start;
  BtorFunSolver *slv;
  BtorNode *cur;
  BtorNodePtrStack visit;
  BtorMemMgr *mm;

  start = btor_util_time_stamp ();
  slv   = BTOR_FUN_SOLVER (btor);
  mm    = btor->mm;

  BTOR_INIT_STACK (mm, visit);
  BTOR_PUSH_STACK (visit, exp);
  do
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));
    assert (!cur->parameterized);
    assert (!btor_node_is_fun (cur));

    if (!cur->apply_below
        || btor_hashint_table_contains (apply_search_cache, cur->id)
        || btor_node_is_fun_eq (cur))
      continue;

    btor_hashint_table_add (apply_search_cache, cur->id);

    if (btor_node_is_apply (cur))
    {
      BTOR_PUSH_STACK (*prop_stack, cur);
      BTOR_PUSH_STACK (*prop_stack, cur->e[0]);
      continue;
    }

    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (visit, cur->e[i]);
  } while (!BTOR_EMPTY_STACK (visit));
  BTOR_RELEASE_STACK (visit);
  slv->time.find_prop_app += btor_util_time_stamp () - start;
}

static bool
find_conflict_app (Btor *btor, BtorNode *app, BtorIntHashTable *conf_apps)
{
  double start;
  bool res = false;
  uint32_t i;
  BtorIntHashTable *cache;
  BtorMemMgr *mm;
  BtorNodePtrStack visit;
  BtorNode *cur;

  start = btor_util_time_stamp ();
  mm    = btor->mm;
  cache = btor_hashint_table_new (mm);
  BTOR_INIT_STACK (mm, visit);
  BTOR_PUSH_STACK (visit, app->e[1]);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (!cur->apply_below || btor_node_is_fun (cur)
        || btor_hashint_table_contains (cache, cur->id))
      continue;
    btor_hashint_table_add (cache, cur->id);
    if (btor_hashint_table_contains (conf_apps, cur->id))
    {
      res = true;
      break;
    }
    if (btor_node_is_apply (cur)) continue;

    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (visit, cur->e[i]);
  }
  btor_hashint_table_delete (cache);
  BTOR_RELEASE_STACK (visit);
  BTOR_FUN_SOLVER (btor)->time.find_conf_app += btor_util_time_stamp () - start;
  return res;
}

static void
propagate (Btor *btor,
           BtorNodePtrStack *prop_stack,
           BtorPtrHashTable *cleanup_table,
           BtorIntHashTable *apply_search_cache)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (prop_stack);
  assert (cleanup_table);
  assert (apply_search_cache);

  double start;
  uint32_t opt_eager_lemmas;
  bool prop_down, conflict, restart;
  BtorBitVector *bv;
  BtorMemMgr *mm;
  BtorFunSolver *slv;
  BtorNode *fun, *app, *args, *fun_value, *cur;
  BtorNode *hashed_app;
  BtorPtrHashBucket *b;
  BtorPtrHashTableIterator it;
  BtorPtrHashTable *conds;
  BtorIntHashTable *conf_apps;

  start            = btor_util_time_stamp ();
  mm               = btor->mm;
  slv              = BTOR_FUN_SOLVER (btor);
  conf_apps        = btor_hashint_table_new (mm);
  opt_eager_lemmas = btor_opt_get (btor, BTOR_OPT_FUN_EAGER_LEMMAS);

  BTORLOG (1, "");
  BTORLOG (1, "*** %s", __FUNCTION__);
  while (!BTOR_EMPTY_STACK (*prop_stack))
  {
    fun = BTOR_POP_STACK (*prop_stack);
    assert (BTOR_IS_REGULAR_NODE (fun));
    assert (btor_node_is_fun (fun));
    assert (!fun->simplified);
    assert (!BTOR_EMPTY_STACK (*prop_stack));
    app = BTOR_POP_STACK (*prop_stack);
    assert (BTOR_IS_REGULAR_NODE (app));
    assert (btor_node_is_apply (app));
    assert (app->refs - app->ext_refs > 0);

    conflict = false;
    restart  = true;

    if (app->propagated) continue;

    app->propagated = 1;
    if (!btor_hashptr_table_get (cleanup_table, app))
      btor_hashptr_table_add (cleanup_table, app);
    slv->stats.propagations++;

    BTORLOG (1, "propagate");
    BTORLOG (1, "  app: %s", btor_util_node2string (app));
    BTORLOG (1, "  fun: %s", btor_util_node2string (fun));

    args = app->e[1];
    assert (BTOR_IS_REGULAR_NODE (args));
    assert (btor_node_is_args (args));

    push_applies_for_propagation (btor, args, prop_stack, apply_search_cache);

    if (!fun->rho)
    {
      fun->rho = btor_hashptr_table_new (mm,
                                         (BtorHashPtr) hash_args_assignment,
                                         (BtorCmpPtr) compare_args_assignments);
      if (!btor_hashptr_table_get (cleanup_table, fun))
        btor_hashptr_table_add (cleanup_table, fun);
    }
    else
    {
      b = btor_hashptr_table_get (fun->rho, args);
      if (b)
      {
        hashed_app = (BtorNode *) b->data.as_ptr;
        assert (BTOR_IS_REGULAR_NODE (hashed_app));
        assert (btor_node_is_apply (hashed_app));

        /* function congruence conflict */
        if (!equal_bv_assignments (hashed_app, app))
        {
          BTORLOG (1, "\e[1;31m");
          BTORLOG (1, "FC conflict at: %s", btor_util_node2string (fun));
          BTORLOG (1, "add_lemma:");
          BTORLOG (1, "  fun: %s", btor_util_node2string (fun));
          BTORLOG (1, "  app1: %s", btor_util_node2string (hashed_app));
          BTORLOG (1, "  app2: %s", btor_util_node2string (app));
          BTORLOG (1, "\e[0;39m");
          if (opt_eager_lemmas == BTOR_FUN_EAGER_LEMMAS_CONF)
          {
            btor_hashint_table_add (conf_apps, app->id);
            restart = find_conflict_app (btor, app, conf_apps);
          }
          else if (opt_eager_lemmas == BTOR_FUN_EAGER_LEMMAS_ALL)
            restart = false;
          slv->stats.function_congruence_conflicts++;
          add_lemma (btor, fun, hashed_app, app);
          conflict = true;
          /* stop at first conflict */
          if (restart) break;
        }
        continue;
      }
    }
    assert (fun->rho);
    assert (!btor_hashptr_table_get (fun->rho, args));
    btor_hashptr_table_add (fun->rho, args)->data.as_ptr = app;
    BTORLOG (1,
             "  save app: %s (%s)",
             btor_util_node2string (args),
             btor_util_node2string (app));

    /* skip array vars/uf */
    if (btor_node_is_uf (fun)) continue;

    if (btor_node_is_fun_cond (fun))
    {
      push_applies_for_propagation (
          btor, fun->e[0], prop_stack, apply_search_cache);
      bv = get_bv_assignment (btor, fun->e[0]);

      /* propagate over function ite */
      BTORLOG (1, "  propagate down: %s", btor_util_node2string (app));
      app->propagated = 0;
      BTOR_PUSH_STACK (*prop_stack, app);
      if (btor_bv_is_true (bv))
        BTOR_PUSH_STACK (*prop_stack, fun->e[1]);
      else
        BTOR_PUSH_STACK (*prop_stack, fun->e[2]);
      btor_bv_free (mm, bv);
      continue;
    }

    assert (btor_node_is_lambda (fun));
    conds = btor_hashptr_table_new (mm,
                                    (BtorHashPtr) btor_node_hash_by_id,
                                    (BtorCmpPtr) btor_node_compare_by_id);
    btor_beta_assign_args (btor, fun, args);
    fun_value = btor_beta_reduce_partial (btor, fun, conds);
    assert (!btor_node_is_fun (fun_value));
    btor_beta_unassign_params (btor, fun);

    prop_down = false;
    if (!BTOR_IS_INVERTED_NODE (fun_value) && btor_node_is_apply (fun_value))
      prop_down = fun_value->e[1] == args;

    if (prop_down)
    {
      assert (btor_node_is_apply (fun_value));
      BTOR_PUSH_STACK (*prop_stack, app);
      BTOR_PUSH_STACK (*prop_stack, BTOR_REAL_ADDR_NODE (fun_value)->e[0]);
      slv->stats.propagations_down++;
      app->propagated = 0;
      BTORLOG (1, "  propagate down: %s", btor_util_node2string (app));
    }
    else if (!equal_bv_assignments (app, fun_value))
    {
      BTORLOG (1, "\e[1;31m");
      BTORLOG (1, "BR conflict at: %s", btor_util_node2string (fun));
      BTORLOG (1, "add_lemma:");
      BTORLOG (1, "  fun: %s", btor_util_node2string (fun));
      BTORLOG (1, "  app: %s", btor_util_node2string (app));
      BTORLOG (1, "\e[0;39m");
      if (opt_eager_lemmas == BTOR_FUN_EAGER_LEMMAS_CONF)
      {
        btor_hashint_table_add (conf_apps, app->id);
        restart = find_conflict_app (btor, app, conf_apps);
      }
      else if (opt_eager_lemmas == BTOR_FUN_EAGER_LEMMAS_ALL)
        restart = false;
      slv->stats.beta_reduction_conflicts++;
      add_lemma (btor, fun, app, 0);
      conflict = true;
    }

    /* we have a conflict and the values are inconsistent, we do not have
     * to push applies onto 'prop_stack' that produce this inconsistent
     * value */
    if (conflict)
    {
      btor_iter_hashptr_init (&it, conds);
      while (btor_iter_hashptr_has_next (&it))
        btor_node_release (btor, btor_iter_hashptr_next (&it));
    }
    /* push applies onto 'prop_stack' that are necesary to derive 'fun_value'
     */
    else
    {
      /* in case of down propagation 'fun_value' is a function application
       * and we can propagate 'app' instead. hence, we to not have to
       * push 'fun_value' onto 'prop_stack'. */
      if (!prop_down)
        push_applies_for_propagation (
            btor, fun_value, prop_stack, apply_search_cache);

      /* push applies in evaluated conditions */
      btor_iter_hashptr_init (&it, conds);
      while (btor_iter_hashptr_has_next (&it))
      {
        cur = btor_iter_hashptr_next (&it);
        push_applies_for_propagation (
            btor, cur, prop_stack, apply_search_cache);
        btor_node_release (btor, cur);
      }
    }
    btor_hashptr_table_delete (conds);
    btor_node_release (btor, fun_value);

    /* stop at first conflict */
    if (restart && conflict) break;
  }
  btor_hashint_table_delete (conf_apps);
  slv->time.prop += btor_util_time_stamp () - start;
}

/* generate hash table for function 'fun' consisting of all rho and static_rho
 * hash tables. */
static BtorPtrHashTable *
generate_table (Btor *btor, BtorNode *fun)
{
  uint32_t i;
  BtorMemMgr *mm;
  BtorNode *cur, *value, *args;
  BtorPtrHashTable *table, *rho, *static_rho;
  BtorNodePtrStack visit;
  BtorIntHashTable *cache;
  BtorPtrHashTableIterator it;
  BtorBitVector *evalbv;

  mm    = btor->mm;
  table = btor_hashptr_table_new (mm,
                                  (BtorHashPtr) hash_args_assignment,
                                  (BtorCmpPtr) compare_args_assignments);
  cache = btor_hashint_table_new (mm);

  BTOR_INIT_STACK (mm, visit);
  BTOR_PUSH_STACK (visit, fun);

  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_hashint_table_contains (cache, cur->id)
        || (!btor_node_is_fun (cur) && !cur->parameterized))
      continue;

    btor_hashint_table_add (cache, cur->id);

    /* NOTE: all encountered lambda nodes need to be arrays,
     *       in any other case we fully support equality over UFs and
     *       conditionals. */
    if (btor_node_is_fun (cur))
    {
      rho        = cur->rho;
      static_rho = 0;

      if (btor_node_is_lambda (cur))
      {
        assert (cur->is_array);
        static_rho = btor_node_lambda_get_static_rho (cur);
        assert (static_rho);
      }
      else if (btor_node_is_fun_cond (cur))
      {
        evalbv = get_bv_assignment (btor, cur->e[0]);
        if (btor_bv_is_true (evalbv))
          BTOR_PUSH_STACK (visit, cur->e[1]);
        else
          BTOR_PUSH_STACK (visit, cur->e[2]);
        btor_bv_free (mm, evalbv);
      }

      if (rho)
      {
        btor_iter_hashptr_init (&it, rho);
        if (static_rho) btor_iter_hashptr_queue (&it, static_rho);
      }
      else if (static_rho)
        btor_iter_hashptr_init (&it, static_rho);

      if (rho || static_rho)
      {
        while (btor_iter_hashptr_has_next (&it))
        {
          value = it.bucket->data.as_ptr;
          assert (!btor_node_is_proxy (value));
          args = btor_iter_hashptr_next (&it);
          assert (!btor_node_is_proxy (args));

          if (!btor_hashptr_table_get (table, args))
            btor_hashptr_table_add (table, args)->data.as_ptr = value;
        }
      }

      /* child already pushed w.r.t. evaluation of condition */
      if (btor_node_is_fun_cond (cur)) continue;
    }

    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (visit, cur->e[i]);
  }

  BTOR_RELEASE_STACK (visit);
  btor_hashint_table_delete (cache);

  return table;
}

static void
add_extensionality_lemmas (Btor *btor)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);

  double start, delta;
  bool skip;
  BtorBitVector *evalbv;
  unsigned num_lemmas = 0;
  BtorNode *cur, *cur_args, *app0, *app1, *eq, *con, *value;
  BtorPtrHashTableIterator it;
  BtorPtrHashTable *table0, *table1, *conflicts;
  BtorPtrHashTableIterator hit;
  BtorNodePtrStack feqs;
  BtorMemMgr *mm;
  BtorPtrHashBucket *b;
  BtorFunSolver *slv;

  start = btor_util_time_stamp ();

  BTORLOG (1, "");
  BTORLOG (1, "*** %s", __FUNCTION__);

  slv = BTOR_FUN_SOLVER (btor);
  mm  = btor->mm;
  BTOR_INIT_STACK (mm, feqs);

  /* collect all reachable function equalities */
  btor_iter_hashptr_init (&it, btor->feqs);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    assert (btor_node_is_fun_eq (cur));
    BTOR_PUSH_STACK (feqs, cur);
  }

  while (!BTOR_EMPTY_STACK (feqs))
  {
    cur = BTOR_POP_STACK (feqs);
    assert (btor_node_is_fun_eq (cur));

    evalbv = get_bv_assignment (btor, cur);
    assert (evalbv);
    skip = btor_bv_is_false (evalbv);
    btor_bv_free (btor->mm, evalbv);

    if (skip) continue;

    table0 = generate_table (btor, cur->e[0]);
    table1 = generate_table (btor, cur->e[1]);

    conflicts = btor_hashptr_table_new (mm,
                                        (BtorHashPtr) hash_args_assignment,
                                        (BtorCmpPtr) compare_args_assignments);

    btor_iter_hashptr_init (&hit, table0);
    while (btor_iter_hashptr_has_next (&hit))
    {
      value    = hit.bucket->data.as_ptr;
      cur_args = btor_iter_hashptr_next (&hit);
      b        = btor_hashptr_table_get (table1, cur_args);

      if (btor_hashptr_table_get (conflicts, cur_args)) continue;

      if (!b || !equal_bv_assignments (value, b->data.as_ptr))
        btor_hashptr_table_add (conflicts, cur_args);
    }

    btor_iter_hashptr_init (&hit, table1);
    while (btor_iter_hashptr_has_next (&hit))
    {
      value    = hit.bucket->data.as_ptr;
      cur_args = btor_iter_hashptr_next (&hit);
      b        = btor_hashptr_table_get (table0, cur_args);

      if (btor_hashptr_table_get (conflicts, cur_args)) continue;

      if (!b || !equal_bv_assignments (value, b->data.as_ptr))
        btor_hashptr_table_add (conflicts, cur_args);
    }

    BTORLOG (1, "  %s", btor_util_node2string (cur));
    btor_iter_hashptr_init (&hit, conflicts);
    while (btor_iter_hashptr_has_next (&hit))
    {
      cur_args = btor_iter_hashptr_next (&hit);
      app0     = btor_exp_apply (btor, cur->e[0], cur_args);
      app1     = btor_exp_apply (btor, cur->e[1], cur_args);
      eq       = btor_exp_eq (btor, app0, app1);
      con      = btor_exp_implies (btor, cur, eq);

      /* add instantiation of extensionality lemma */
      if (!btor_hashptr_table_get (slv->lemmas, con))
      {
        btor_hashptr_table_add (slv->lemmas, btor_node_copy (btor, con));
        BTOR_PUSH_STACK (slv->cur_lemmas, con);
        slv->stats.extensionality_lemmas++;
        slv->stats.lod_refinements++;
        num_lemmas++;
        BTORLOG (1,
                 "    %s, %s",
                 btor_util_node2string (app0),
                 btor_util_node2string (app1));
      }
      btor_node_release (btor, app0);
      btor_node_release (btor, app1);
      btor_node_release (btor, eq);
      btor_node_release (btor, con);
    }
    btor_hashptr_table_delete (conflicts);
    btor_hashptr_table_delete (table0);
    btor_hashptr_table_delete (table1);
  }
  BTOR_RELEASE_STACK (feqs);

  delta = btor_util_time_stamp () - start;
  BTORLOG (
      1, "  added %u extensionality lemma in %.2f seconds", num_lemmas, delta);
  slv->time.check_extensionality += delta;
}

static void
check_and_resolve_conflicts (Btor *btor,
                             Btor *clone,
                             BtorNode *clone_root,
                             BtorNodeMap *exp_map,
                             BtorNodePtrStack *init_apps,
                             BtorIntHashTable *init_apps_cache)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);

  double start, start_cleanup;
  bool found_conflicts;
  int32_t i;
  BtorMemMgr *mm;
  BtorFunSolver *slv;
  BtorNode *app, *cur;
  BtorNodePtrStack prop_stack;
  BtorNodePtrStack top_applies;
  BtorPtrHashTable *cleanup_table;
  BtorIntHashTable *apply_search_cache;
  BtorPtrHashTableIterator pit;
  BtorIntHashTableIterator iit;

  start           = btor_util_time_stamp ();
  slv             = BTOR_FUN_SOLVER (btor);
  found_conflicts = false;
  mm              = btor->mm;
  slv             = BTOR_FUN_SOLVER (btor);

  /* initialize new bit vector model, which will be constructed while
   * consistency checking. this also deletes the model from the previous run */
  btor_model_init_bv (btor, &btor->bv_model);

  assert (!found_conflicts);
  cleanup_table = btor_hashptr_table_new (mm,
                                          (BtorHashPtr) btor_node_hash_by_id,
                                          (BtorCmpPtr) btor_node_compare_by_id);
  BTOR_INIT_STACK (mm, prop_stack);
  BTOR_INIT_STACK (mm, top_applies);

  apply_search_cache = btor_hashint_table_new (mm);

  /* NOTE: terms in var_rhs are always part of the formula (due to the implicit
   * top level equality). if terms containing applies do not occur in the
   * formula anymore due to variable substitution, we still need to ensure that
   * the assignment computed for the substituted variable is correct. hence, we
   * need to check the applies for consistency and push them onto the
   * propagation stack.
   * this also applies for don't care reasoning.
   */
  btor_iter_hashptr_init (&pit, btor->var_rhs);
  while (btor_iter_hashptr_has_next (&pit))
  {
    cur = btor_simplify_exp (btor, btor_iter_hashptr_next (&pit));
    /* no parents -> is not reachable from the roots */
    if (BTOR_REAL_ADDR_NODE (cur)->parents > 0) continue;
    push_applies_for_propagation (btor, cur, &prop_stack, apply_search_cache);
  }

  if (clone)
  {
    search_initial_applies_dual_prop (
        btor, clone, clone_root, exp_map, &top_applies);
    init_apps = &top_applies;
  }
  else if (btor_opt_get (btor, BTOR_OPT_FUN_JUST))
  {
    search_initial_applies_just (btor, &top_applies);
    init_apps = &top_applies;
  }
  else
    search_initial_applies_bv_skeleton (btor, init_apps, init_apps_cache);

  for (i = BTOR_COUNT_STACK (*init_apps) - 1; i >= 0; i--)
  {
    app = BTOR_PEEK_STACK (*init_apps, i);
    assert (BTOR_IS_REGULAR_NODE (app));
    assert (btor_node_is_apply (app));
    assert (!app->parameterized);
    assert (!app->propagated);
    BTOR_PUSH_STACK (prop_stack, app);
    BTOR_PUSH_STACK (prop_stack, app->e[0]);
  }

  propagate (btor, &prop_stack, cleanup_table, apply_search_cache);
  found_conflicts = BTOR_COUNT_STACK (slv->cur_lemmas) > 0;

  /* check consistency of array/uf equalities */
  if (!found_conflicts && btor->feqs->count > 0)
  {
    assert (BTOR_EMPTY_STACK (prop_stack));
    add_extensionality_lemmas (btor);
    found_conflicts = BTOR_COUNT_STACK (slv->cur_lemmas) > 0;
  }

  /* applies may have assignments that were not checked for consistency, which
   * is the case when they are not required for deriving SAT (don't care
   * reasoning). hence, we remove those applies from the 'bv_model' as they do
   * not have a valid assignment. an assignment will be generated during
   * model construction */
  if (!found_conflicts)
  {
    btor_iter_hashint_init (&iit, btor->bv_model);
    while (btor_iter_hashint_has_next (&iit))
    {
      cur = btor_node_get_by_id (btor, btor_iter_hashint_next (&iit));
      if (btor_node_is_apply (cur) && !cur->propagated)
        btor_model_remove_from_bv (btor, btor->bv_model, cur);
    }
  }

  start_cleanup = btor_util_time_stamp ();
  btor_iter_hashptr_init (&pit, cleanup_table);
  while (btor_iter_hashptr_has_next (&pit))
  {
    cur = btor_iter_hashptr_next (&pit);
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (btor_node_is_apply (cur))
    {
      /* generate model for apply */
      if (!found_conflicts)
        btor_bv_free (btor->mm, get_bv_assignment (btor, cur));
      cur->propagated = 0;
    }
    else
    {
      assert (btor_node_is_fun (cur));
      assert (cur->rho);

      if (found_conflicts)
      {
        btor_hashptr_table_delete (cur->rho);
        cur->rho = 0;
      }
      else
      {
        /* remember functions for incremental usage (and prevent
         * premature release in case that function is released via API
         * call) */
        BTOR_PUSH_STACK (btor->functions_with_model,
                         btor_node_copy (btor, cur));
      }
    }
  }
  slv->time.prop_cleanup += btor_util_time_stamp () - start_cleanup;
  btor_hashptr_table_delete (cleanup_table);
  BTOR_RELEASE_STACK (prop_stack);
  BTOR_RELEASE_STACK (top_applies);
  btor_hashint_table_delete (apply_search_cache);
  slv->time.check_consistency += btor_util_time_stamp () - start;
}

static BtorSolverResult
sat_fun_solver (BtorFunSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  uint32_t i;
  bool done;
  BtorSolverResult result;
  Btor *btor, *clone;
  BtorNode *clone_root, *lemma;
  BtorNodeMap *exp_map;
  BtorIntHashTable *init_apps_cache;
  BtorNodePtrStack init_apps;

  btor = slv->btor;
  assert (!btor->inconsistent);

  /* make initial applies in bv skeleton global in order to prevent
   * traversing the whole formula every refinement round */
  BTOR_INIT_STACK (btor->mm, init_apps);
  init_apps_cache = btor_hashint_table_new (btor->mm);

  clone      = 0;
  clone_root = 0;
  exp_map    = 0;

  if ((btor_opt_get (btor, BTOR_OPT_FUN_PREPROP)
       || btor_opt_get (btor, BTOR_OPT_FUN_PRESLS))
      && btor->ufs->count == 0 && btor->feqs->count == 0)
  {
    BtorSolver *preslv;
    BtorOptEngine eopt;

    if (btor->lambdas->count) btor_opt_set (btor, BTOR_OPT_BETA_REDUCE_ALL, 1);

    if (btor_opt_get (btor, BTOR_OPT_FUN_PREPROP))
    {
      preslv = btor_new_prop_solver (btor);
      eopt   = BTOR_ENGINE_PROP;
    }
    else
    {
      preslv = btor_new_sls_solver (btor);
      eopt   = BTOR_ENGINE_SLS;
    }

    btor->slv = preslv;
    btor_opt_set (btor, BTOR_OPT_ENGINE, eopt);
    result = btor->slv->api.sat (btor->slv);
    done   = result == BTOR_RESULT_SAT || result == BTOR_RESULT_UNSAT;
    /* print prop/sls solver statistics */
    btor->slv->api.print_stats (btor->slv);
    btor->slv->api.print_time_stats (btor->slv);
    /* delete prop/sls solver */
    btor->slv->api.delet (btor->slv);
    /* reset */
    btor->slv = (BtorSolver *) slv;
    btor_opt_set (btor, BTOR_OPT_ENGINE, BTOR_ENGINE_FUN);
    if (done)
    {
      BTOR_MSG (btor->msg, 1, "");
      BTOR_MSG (btor->msg,
                1,
                "%s engine determined %s",
                eopt == BTOR_ENGINE_PROP ? "PROP" : "SLS",
                result == BTOR_RESULT_SAT ? "'sat'" : "'unsat'");
      goto DONE;
    }
    /* reset */
    btor_model_delete (btor);
  }

  if (btor_terminate (btor))
  {
  UNKNOWN:
    result = BTOR_RESULT_UNKNOWN;
    goto DONE;
  }

  configure_sat_mgr (btor);

  if (btor->feqs->count > 0) add_function_inequality_constraints (btor);

  /* initialize dual prop clone */
  if (btor_opt_get (btor, BTOR_OPT_FUN_DUAL_PROP))
    clone = new_exp_layer_clone_for_dual_prop (btor, &exp_map, &clone_root);

  while (true)
  {
    if (btor_terminate (btor)
        || (slv->lod_limit > -1
            && slv->stats.lod_refinements >= (uint32_t) slv->lod_limit))
    {
      goto UNKNOWN;
    }

    btor_process_unsynthesized_constraints (btor);
    if (btor->found_constraint_false)
    {
    UNSAT:
      result = BTOR_RESULT_UNSAT;
      goto DONE;
    }
    assert (btor->unsynthesized_constraints->count == 0);
    assert (btor_dbg_check_all_hash_tables_proxy_free (btor));
    assert (btor_dbg_check_all_hash_tables_simp_free (btor));

    /* make SAT call on bv skeleton */
    btor_add_again_assumptions (btor);
    result = timed_sat_sat (btor, slv->sat_limit);

    if (result == BTOR_RESULT_UNSAT)
      goto DONE;
    else if (result == BTOR_RESULT_UNKNOWN)
    {
      assert (slv->sat_limit > -1 || btor->cbs.term.done);
      goto DONE;
    }

    assert (result == BTOR_RESULT_SAT);

    if (btor->ufs->count == 0 && btor->lambdas->count == 0) break;

    check_and_resolve_conflicts (
        btor, clone, clone_root, exp_map, &init_apps, init_apps_cache);
    if (BTOR_EMPTY_STACK (slv->cur_lemmas)) break;
    slv->stats.refinement_iterations++;

    BTORLOG (1, "add %d lemma(s)", BTOR_COUNT_STACK (slv->cur_lemmas));
    /* add generated lemmas to formula */
    for (i = 0; i < BTOR_COUNT_STACK (slv->cur_lemmas); i++)
    {
      lemma = BTOR_PEEK_STACK (slv->cur_lemmas, i);
      assert (!BTOR_REAL_ADDR_NODE (lemma)->simplified);
      // TODO (ma): use btor_assert_exp?
      btor_insert_unsynthesized_constraint (btor, lemma);
      if (clone)
        add_lemma_to_dual_prop_clone (btor, clone, &clone_root, lemma, exp_map);
    }
    BTOR_RESET_STACK (slv->cur_lemmas);

    if (btor_opt_get (btor, BTOR_OPT_VERBOSITY))
    {
      printf (
          "[btorcore] %d iterations, %d lemmas, %d ext. lemmas, "
          "vars %d, applies %d\n",
          slv->stats.refinement_iterations,
          slv->stats.lod_refinements,
          slv->stats.extensionality_lemmas,
          btor->ops[BTOR_BV_VAR_NODE].cur,
          btor->ops[BTOR_APPLY_NODE].cur);
    }

    /* may be set via insert_unsythesized_constraint
     * in case generated lemma is false */
    if (btor->inconsistent) goto UNSAT;
  }

DONE:
  BTOR_RELEASE_STACK (init_apps);
  btor_hashint_table_delete (init_apps_cache);

  if (clone)
  {
    assert (exp_map);
    btor_nodemap_delete (exp_map);
    btor_node_release (clone, clone_root);
    btor_delete (clone);
  }
  return result;
}

/*------------------------------------------------------------------------*/

static void
generate_model_fun_solver (BtorFunSolver *slv,
                           bool model_for_all_nodes,
                           bool reset)
{
  assert (slv);
  assert (slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  (void) reset;

  /* already created during check_and_resolve_conflicts */
  if (!slv->btor->bv_model)
    btor_model_init_bv (slv->btor, &slv->btor->bv_model);
  btor_model_init_fun (slv->btor, &slv->btor->fun_model);

  btor_model_generate (slv->btor,
                       slv->btor->bv_model,
                       slv->btor->fun_model,
                       model_for_all_nodes);
}

static void
print_stats_fun_solver (BtorFunSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  uint32_t i;
  Btor *btor;

  btor = slv->btor;

  if (!(slv = BTOR_FUN_SOLVER (btor))) return;

  if (btor->ufs->count || btor->lambdas->count)
  {
    BTOR_MSG (btor->msg, 1, "");
    BTOR_MSG (btor->msg, 1, "lemmas on demand statistics:");
    BTOR_MSG (btor->msg,
              1,
              "%4d refinement iterations",
              slv->stats.refinement_iterations);
    BTOR_MSG (btor->msg, 1, "%4d LOD refinements", slv->stats.lod_refinements);
    if (slv->stats.lod_refinements)
    {
      BTOR_MSG (btor->msg,
                1,
                "  %4d function congruence conflicts",
                slv->stats.function_congruence_conflicts);
      BTOR_MSG (btor->msg,
                1,
                "  %4d beta reduction conflicts",
                slv->stats.beta_reduction_conflicts);
      BTOR_MSG (btor->msg,
                1,
                "  %4d extensionality lemmas",
                slv->stats.extensionality_lemmas);
      BTOR_MSG (btor->msg,
                1,
                "  %.1f average lemma size",
                BTOR_AVERAGE_UTIL (slv->stats.lemmas_size_sum,
                                   slv->stats.lod_refinements));
      for (i = 1; i < BTOR_SIZE_STACK (slv->stats.lemmas_size); i++)
      {
        if (!slv->stats.lemmas_size.start[i]) continue;
        BTOR_MSG (btor->msg,
                  1,
                  "    %4d lemmas of size %d",
                  slv->stats.lemmas_size.start[i],
                  i);
      }
    }
  }

  BTOR_MSG (btor->msg, 1, "");
  BTOR_MSG (
      btor->msg, 1, "%7lld expression evaluations", slv->stats.eval_exp_calls);
  BTOR_MSG (btor->msg,
            1,
            "%7lld partial beta reductions",
            btor->stats.betap_reduce_calls);
  BTOR_MSG (btor->msg, 1, "%7lld propagations", slv->stats.propagations);
  BTOR_MSG (
      btor->msg, 1, "%7lld propagations down", slv->stats.propagations_down);

  if (btor_opt_get (btor, BTOR_OPT_FUN_DUAL_PROP))
  {
    BTOR_MSG (btor->msg,
              1,
              "%d/%d dual prop. vars (failed/assumed)",
              slv->stats.dp_failed_vars,
              slv->stats.dp_assumed_vars);
    BTOR_MSG (btor->msg,
              1,
              "%d/%d dual prop. applies (failed/assumed)",
              slv->stats.dp_failed_applies,
              slv->stats.dp_assumed_applies);
  }
}

static void
print_time_stats_fun_solver (BtorFunSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  Btor *btor;

  btor = slv->btor;

  BTOR_MSG (btor->msg, 1, "");
  BTOR_MSG (btor->msg,
            1,
            "%.2f seconds consistency checking",
            slv->time.check_consistency);
  BTOR_MSG (btor->msg,
            1,
            "  %.2f seconds initial applies search",
            slv->time.search_init_apps);

  if (btor_opt_get (btor, BTOR_OPT_FUN_JUST)
      || btor_opt_get (btor, BTOR_OPT_FUN_DUAL_PROP))
  {
    BTOR_MSG (btor->msg,
              1,
              "    %.2f seconds compute scores",
              slv->time.search_init_apps_compute_scores);
    BTOR_MSG (btor->msg,
              1,
              "      %.2f seconds merge applies",
              slv->time.search_init_apps_compute_scores_merge_applies);
  }

  if (btor_opt_get (btor, BTOR_OPT_FUN_DUAL_PROP))
  {
    BTOR_MSG (btor->msg,
              1,
              "    %.2f seconds cloning",
              slv->time.search_init_apps_cloning);
    BTOR_MSG (btor->msg,
              1,
              "    %.2f seconds SAT solving",
              slv->time.search_init_apps_sat);
    BTOR_MSG (btor->msg,
              1,
              "    %.2f seconds collecting bv vars and apps",
              slv->time.search_init_apps_collect_var_apps);
    BTOR_MSG (btor->msg,
              1,
              "    %.2f seconds collecting initial applies (FA)",
              slv->time.search_init_apps_collect_fa);
    BTOR_MSG (btor->msg,
              1,
              "      %.2f seconds cone traversal",
              slv->time.search_init_apps_collect_fa_cone);
  }

  BTOR_MSG (btor->msg, 1, "  %.2f seconds propagation", slv->time.prop);
  BTOR_MSG (
      btor->msg, 1, "    %.2f seconds expression evaluation", slv->time.eval);
  BTOR_MSG (btor->msg,
            1,
            "    %.2f seconds partial beta reduction",
            btor->time.betap);
  BTOR_MSG (
      btor->msg, 1, "    %.2f seconds lemma generation", slv->time.lemma_gen);
  BTOR_MSG (btor->msg,
            1,
            "    %.2f seconds propagation apply search",
            slv->time.find_prop_app);
  BTOR_MSG (btor->msg,
            1,
            "    %.2f seconds conflict apply search",
            slv->time.find_conf_app);
  if (btor->feqs->count > 0)
    BTOR_MSG (btor->msg,
              1,
              "  %.2f seconds check extensionality",
              slv->time.check_extensionality);
  BTOR_MSG (btor->msg,
            1,
            "  %.2f seconds propagation cleanup",
            slv->time.prop_cleanup);

  BTOR_MSG (btor->msg, 1, "%.2f seconds in pure SAT solving", slv->time.sat);
  BTOR_MSG (btor->msg, 1, "");
}

BtorSolver *
btor_new_fun_solver (Btor *btor)
{
  assert (btor);

  BtorFunSolver *slv;

  BTOR_CNEW (btor->mm, slv);

  slv->kind = BTOR_FUN_SOLVER_KIND;
  slv->btor = btor;

  slv->api.clone          = (BtorSolverClone) clone_fun_solver;
  slv->api.delet          = (BtorSolverDelete) delete_fun_solver;
  slv->api.sat            = (BtorSolverSat) sat_fun_solver;
  slv->api.generate_model = (BtorSolverGenerateModel) generate_model_fun_solver;
  slv->api.print_stats    = (BtorSolverPrintStats) print_stats_fun_solver;
  slv->api.print_time_stats =
      (BtorSolverPrintTimeStats) print_time_stats_fun_solver;

  slv->lod_limit = -1;
  slv->sat_limit = -1;

  slv->lemmas = btor_hashptr_table_new (btor->mm,
                                        (BtorHashPtr) btor_node_hash_by_id,
                                        (BtorCmpPtr) btor_node_compare_by_id);
  BTOR_INIT_STACK (btor->mm, slv->cur_lemmas);

  BTOR_INIT_STACK (btor->mm, slv->stats.lemmas_size);

  BTOR_MSG (btor->msg, 1, "enabled core engine");

  return (BtorSolver *) slv;
}

// TODO (ma): this is just a fix for now, this should be moved elsewhere
BtorBitVector *
btor_eval_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (btor->slv);
  assert (btor->slv->kind == BTOR_FUN_SOLVER_KIND);
  assert (exp);
  assert (btor->bv_model);

  uint32_t i;
  double start;
  BtorMemMgr *mm;
  BtorNodePtrStack work_stack;
  BtorVoidPtrStack arg_stack;
  BtorNode *cur, *real_cur, *next;
  BtorPtrHashTable *cache;
  BtorPtrHashBucket *b;
  BtorPtrHashTableIterator it;
  BtorBitVector *result = 0, *inv_result, **e;
  BtorFunSolver *slv;
  BtorIntHashTable *mark;
  BtorHashTableData *d;

  start = btor_util_time_stamp ();
  mm    = btor->mm;
  slv   = BTOR_FUN_SOLVER (btor);
  slv->stats.eval_exp_calls++;

  BTOR_INIT_STACK (mm, work_stack);
  BTOR_INIT_STACK (mm, arg_stack);
  cache = btor_hashptr_table_new (mm,
                                  (BtorHashPtr) btor_node_hash_by_id,
                                  (BtorCmpPtr) btor_node_compare_by_id);
  mark  = btor_hashint_map_new (mm);

  BTOR_PUSH_STACK (work_stack, exp);
  while (!BTOR_EMPTY_STACK (work_stack))
  {
    cur      = BTOR_POP_STACK (work_stack);
    real_cur = BTOR_REAL_ADDR_NODE (cur);
    assert (!real_cur->simplified);

    d = btor_hashint_map_get (mark, real_cur->id);

    if (!d)
    {
      if (btor_node_is_bv_var (real_cur) || btor_node_is_apply (real_cur)
          || btor_node_is_fun_eq (real_cur)
          || has_bv_assignment (btor, real_cur))
      {
        result = get_bv_assignment (btor, real_cur);
        goto EVAL_EXP_PUSH_RESULT;
      }
      else if (btor_node_is_bv_const (real_cur))
      {
        result = btor_bv_copy (mm, btor_node_const_get_bits (real_cur));
        goto EVAL_EXP_PUSH_RESULT;
      }
      /* substitute param with its assignment */
      else if (btor_node_is_param (real_cur))
      {
        next = btor_node_param_get_assigned_exp (real_cur);
        assert (next);
        if (BTOR_IS_INVERTED_NODE (cur)) next = BTOR_INVERT_NODE (next);
        BTOR_PUSH_STACK (work_stack, next);
        continue;
      }

      BTOR_PUSH_STACK (work_stack, cur);
      btor_hashint_map_add (mark, real_cur->id);

      for (i = 0; i < real_cur->arity; i++)
        BTOR_PUSH_STACK (work_stack, real_cur->e[i]);
    }
    else if (d->as_int == 0)
    {
      assert (!btor_node_is_param (real_cur));
      assert (!btor_node_is_args (real_cur));
      assert (!btor_node_is_fun (real_cur));
      assert (real_cur->arity >= 1);
      assert (real_cur->arity <= 3);
      assert (real_cur->arity <= BTOR_COUNT_STACK (arg_stack));

      d->as_int = 1;
      arg_stack.top -= real_cur->arity;
      e = (BtorBitVector **) arg_stack.top; /* arguments in reverse order */

      switch (real_cur->kind)
      {
        case BTOR_SLICE_NODE:
          result = btor_bv_slice (mm,
                                  e[0],
                                  btor_node_slice_get_upper (real_cur),
                                  btor_node_slice_get_lower (real_cur));
          btor_bv_free (mm, e[0]);
          break;
        case BTOR_AND_NODE:
          result = btor_bv_and (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_BV_EQ_NODE:
          result = btor_bv_eq (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_ADD_NODE:
          result = btor_bv_add (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_MUL_NODE:
          result = btor_bv_mul (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_ULT_NODE:
          result = btor_bv_ult (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_SLL_NODE:
          result = btor_bv_sll (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_SRL_NODE:
          result = btor_bv_srl (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_UDIV_NODE:
          result = btor_bv_udiv (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_UREM_NODE:
          result = btor_bv_urem (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_CONCAT_NODE:
          result = btor_bv_concat (mm, e[1], e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          break;
        case BTOR_COND_NODE:
          if (btor_bv_is_true (e[2]))
            result = btor_bv_copy (mm, e[1]);
          else
            result = btor_bv_copy (mm, e[0]);
          btor_bv_free (mm, e[0]);
          btor_bv_free (mm, e[1]);
          btor_bv_free (mm, e[2]);
          break;
        default:
          BTORLOG (1, "  *** %s", btor_util_node2string (real_cur));
          /* should be unreachable */
          assert (0);
      }

      assert (!btor_hashptr_table_get (cache, real_cur));
      btor_hashptr_table_add (cache, real_cur)->data.as_ptr =
          btor_bv_copy (mm, result);

    EVAL_EXP_PUSH_RESULT:
      if (BTOR_IS_INVERTED_NODE (cur))
      {
        inv_result = btor_bv_not (mm, result);
        btor_bv_free (mm, result);
        result = inv_result;
      }

      BTOR_PUSH_STACK (arg_stack, result);
    }
    else
    {
      assert (d->as_int == 1);
      b = btor_hashptr_table_get (cache, real_cur);
      assert (b);
      result = btor_bv_copy (mm, (BtorBitVector *) b->data.as_ptr);
      goto EVAL_EXP_PUSH_RESULT;
    }
  }
  assert (BTOR_COUNT_STACK (arg_stack) == 1);
  result = BTOR_POP_STACK (arg_stack);
  assert (result);

  while (!BTOR_EMPTY_STACK (arg_stack))
  {
    inv_result = BTOR_POP_STACK (arg_stack);
    btor_bv_free (mm, inv_result);
  }

  btor_iter_hashptr_init (&it, cache);
  while (btor_iter_hashptr_has_next (&it))
  {
    btor_bv_free (mm, (BtorBitVector *) it.bucket->data.as_ptr);
    real_cur = btor_iter_hashptr_next (&it);
  }

  BTOR_RELEASE_STACK (work_stack);
  BTOR_RELEASE_STACK (arg_stack);
  btor_hashptr_table_delete (cache);
  btor_hashint_map_delete (mark);

  //  BTORLOG ("%s: %s '%s'", __FUNCTION__, btor_util_node2string (exp),
  //  result);
  slv->time.eval += btor_util_time_stamp () - start;

  return result;
}
