/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2012-2013 Armin Biere.
 *  Copyright (C) 2013-2016 Mathias Preiner.
 *  Copyright (C) 2014-2016 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorsort.h"

#include "btorabort.h"
#include "btorcore.h"
#include "btorexp.h"
#include "utils/btorutil.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#define BTOR_SORT_UNIQUE_TABLE_LIMIT 30

#define BTOR_FULL_SORT_UNIQUE_TABLE(table) \
  ((table)->num_elements >= (table)->size  \
   && btor_log_2_util ((table)->size) < BTOR_SORT_UNIQUE_TABLE_LIMIT)

static void
inc_sort_ref_counter (BtorSort *sort)
{
  assert (sort);
  BTOR_ABORT (sort->refs == INT_MAX, "Sort reference counter overflow");
  sort->refs++;
}

static unsigned
compute_hash_sort (const BtorSort *sort, int table_size)
{
  assert (sort);
  assert (table_size);
  assert (btor_is_power_of_2_util (table_size));

  unsigned i, res, tmp;

  tmp = 0;

  switch (sort->kind)
  {
    default:
    case BTOR_BOOL_SORT:
      assert (sort->kind == BTOR_BOOL_SORT);
      res = 0;
      break;

    case BTOR_BITVEC_SORT: res = (unsigned int) sort->bitvec.width; break;

    case BTOR_ARRAY_SORT:
      res = (unsigned int) sort->array.index->id;
      tmp = (unsigned int) sort->array.element->id;
      break;

    case BTOR_LST_SORT:
      res = (unsigned int) sort->lst.head->id;
      tmp = (unsigned int) sort->lst.tail->id;
      break;

    case BTOR_FUN_SORT:
      res = (unsigned int) sort->fun.domain->id;
      tmp = (unsigned int) sort->fun.codomain->id;
      break;

    case BTOR_TUPLE_SORT:
      res = 0;
      for (i = 0; i < sort->tuple.num_elements; i++)
      {
        if ((i & 1) == 0)
          res += (unsigned int) sort->tuple.elements[i]->id;
        else
          tmp += (unsigned int) sort->tuple.elements[i]->id;
      }
      break;
  }

  res *= 444555667u;

  if (tmp)
  {
    res += tmp;
    res *= 123123137u;
  }

  res &= table_size - 1;

  return res;
}

static void
remove_from_sorts_unique_table_sort (BtorSortUniqueTable *table, BtorSort *sort)
{
  assert (table);
  assert (sort);
  assert (!sort->refs);
  assert (table->num_elements > 0);

  unsigned int hash;
  BtorSort *prev, *cur;

  hash = compute_hash_sort (sort, table->size);
  prev = 0;
  cur  = table->chains[hash];

  while (cur != sort)
  {
    assert (cur);
    prev = cur;
    cur  = cur->next;
  }

  assert (cur);
  if (!prev)
    table->chains[hash] = cur->next;
  else
    prev->next = cur->next;

  table->num_elements--;
}

static int
equal_sort (const BtorSort *a, const BtorSort *b)
{
  assert (a);
  assert (b);

  unsigned i;

  if (a->kind != b->kind) return 0;

  switch (a->kind)
  {
    case BTOR_BOOL_SORT:
    default: assert (a->kind == BTOR_BOOL_SORT); break;

    case BTOR_BITVEC_SORT:
      if (a->bitvec.width != b->bitvec.width) return 0;
      break;

    case BTOR_ARRAY_SORT:
      if (a->array.index->id != b->array.index->id) return 0;
      if (a->array.element->id != b->array.element->id) return 0;
      break;

    case BTOR_LST_SORT:
      if (a->lst.head->id != b->lst.head->id) return 0;
      if (a->lst.tail->id != b->lst.tail->id) return 0;
      break;

    case BTOR_FUN_SORT:
      if (a->fun.domain->id != b->fun.domain->id) return 0;
      if (a->fun.codomain->id != b->fun.codomain->id) return 0;
      break;

    case BTOR_TUPLE_SORT:
      if (a->tuple.num_elements != b->tuple.num_elements) return 0;
      for (i = 0; i < a->tuple.num_elements; i++)
        if (a->tuple.elements[i]->id != b->tuple.elements[i]->id) return 0;
      break;
  }

  return 1;
}

static BtorSort **
find_sort (BtorSortUniqueTable *table, const BtorSort *pattern)
{
  assert (table);
  assert (pattern);

  BtorSort **res, *sort;
  unsigned int hash;
  hash = compute_hash_sort (pattern, table->size);
  assert (hash < (unsigned) table->size);
  for (res = table->chains + hash; (sort = *res) && !equal_sort (sort, pattern);
       res = &sort->next)
    assert (sort->refs > 0);
  return res;
}

static void
enlarge_sorts_unique_table (BtorSortUniqueTable *table)
{
  assert (table);

  BtorSort *cur, *temp, **new_chains;
  int size, new_size, i;
  unsigned int hash;
  BtorMemMgr *mm;

  mm       = table->mm;
  size     = table->size;
  new_size = size << 1;
  assert (new_size / size == 2);
  BTOR_CNEWN (mm, new_chains, new_size);
  for (i = 0; i < size; i++)
  {
    cur = table->chains[i];
    while (cur)
    {
      temp             = cur->next;
      hash             = compute_hash_sort (cur, new_size);
      cur->next        = new_chains[hash];
      new_chains[hash] = cur;
      cur              = temp;
    }
  }
  BTOR_DELETEN (mm, table->chains, size);
  table->size   = new_size;
  table->chains = new_chains;
}

static void
release_sort (BtorSortUniqueTable *table, BtorSort *sort)
{
  assert (table);
  assert (sort);
  assert (sort->refs > 0);

  unsigned i;

  if (--sort->refs > 0) return;

  remove_from_sorts_unique_table_sort (table, sort);

  switch (sort->kind)
  {
    default: break;

    case BTOR_LST_SORT:
#ifndef NDEBUG
      sort->lst.head->parents--;
      sort->lst.tail->parents--;
#endif
      release_sort (table, sort->lst.head);
      release_sort (table, sort->lst.tail);
      break;

    case BTOR_ARRAY_SORT:
#ifndef NDEBUG
      sort->array.index->parents--;
      sort->array.element->parents--;
#endif
      release_sort (table, sort->array.index);
      release_sort (table, sort->array.element);
      break;

    case BTOR_FUN_SORT:
#ifndef NDEBUG
      sort->fun.domain->parents--;
      sort->fun.codomain->parents--;
#endif
      release_sort (table, sort->fun.domain);
      release_sort (table, sort->fun.codomain);
      break;

    case BTOR_TUPLE_SORT:
      for (i = 0; i < sort->tuple.num_elements; i++)
      {
#ifndef NDEBUG
        sort->tuple.elements[i]->parents--;
#endif
        release_sort (table, sort->tuple.elements[i]);
      }
      BTOR_DELETEN (table->mm, sort->tuple.elements, sort->tuple.num_elements);
      break;
  }

  assert (BTOR_PEEK_STACK (table->id2sort, sort->id) == sort);
  BTOR_POKE_STACK (table->id2sort, sort->id, 0);
  BTOR_DELETE (table->mm, sort);
}

BtorSort *
btor_get_sort_by_id (Btor *btor, BtorSortId id)
{
  assert (btor);
  assert (id < BTOR_COUNT_STACK (btor->sorts_unique_table.id2sort));
  return BTOR_PEEK_STACK (btor->sorts_unique_table.id2sort, id);
}

BtorSortId
btor_copy_sort (Btor *btor, BtorSortId id)
{
  assert (btor);
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  inc_sort_ref_counter (sort);
  return id;
}

void
btor_release_sort (Btor *btor, BtorSortId id)
{
  assert (btor);

  BtorSort *sort;

  sort = btor_get_sort_by_id (btor, id);
  assert (sort);
  assert (sort->refs > 0);
  release_sort (&btor->sorts_unique_table, sort);
}

static BtorSort *
copy_sort (BtorSort *sort)
{
  inc_sort_ref_counter (sort);
  return sort;
}

static BtorSort *
create_sort (Btor *btor, BtorSortUniqueTable *table, BtorSort *pattern)
{
  assert (table);
  assert (pattern);

  unsigned i;
  BtorSort *res;

  BTOR_CNEW (table->mm, res);

#ifndef NDEBUG
  res->btor = btor;
#endif

  switch (pattern->kind)
  {
    case BTOR_BOOL_SORT: res->kind = BTOR_BOOL_SORT; break;

    case BTOR_BITVEC_SORT:
      res->kind         = BTOR_BITVEC_SORT;
      res->bitvec.width = pattern->bitvec.width;
      break;

    case BTOR_ARRAY_SORT:
      res->kind          = BTOR_ARRAY_SORT;
      res->array.index   = copy_sort (pattern->array.index);
      res->array.element = copy_sort (pattern->array.element);
#ifndef NDEBUG
      res->array.index->parents++;
      res->array.element->parents++;
#endif
      break;

    case BTOR_LST_SORT:
      res->kind     = BTOR_LST_SORT;
      res->lst.head = copy_sort (pattern->lst.head);
      res->lst.tail = copy_sort (pattern->lst.tail);
#ifndef NDEBUG
      res->lst.head->parents++;
      res->lst.tail->parents++;
#endif
      break;

    case BTOR_FUN_SORT:
      res->kind         = BTOR_FUN_SORT;
      res->fun.domain   = copy_sort (pattern->fun.domain);
      res->fun.codomain = copy_sort (pattern->fun.codomain);
#ifndef NDEBUG
      res->fun.domain->parents++;
      res->fun.codomain->parents++;
#endif
      break;

    case BTOR_TUPLE_SORT:
      res->kind               = BTOR_TUPLE_SORT;
      res->tuple.num_elements = pattern->tuple.num_elements;
      BTOR_NEWN (table->mm, res->tuple.elements, res->tuple.num_elements);
      for (i = 0; i < res->tuple.num_elements; i++)
      {
        res->tuple.elements[i] = copy_sort (pattern->tuple.elements[i]);
#ifndef NDEBUG
        res->tuple.elements[i]->parents++;
#endif
      }
      break;

    default: break;
  }
  assert (res->kind);
  res->id = BTOR_COUNT_STACK (table->id2sort);
  BTOR_PUSH_STACK (table->id2sort, res);
  assert (BTOR_COUNT_STACK (table->id2sort) == res->id + 1);
  assert (BTOR_PEEK_STACK (table->id2sort, res->id) == res);

  table->num_elements++;
  res->table = table;

  return res;
}

BtorSortId
btor_bool_sort (Btor *btor)
{
  return btor_bitvec_sort (btor, 1);
}

BtorSortId
btor_bitvec_sort (Btor *btor, unsigned width)
{
  assert (btor);
  assert (width > 0);

  BtorSort *res, **pos, pattern;
  BtorSortUniqueTable *table;

  table = &btor->sorts_unique_table;

  BTOR_CLR (&pattern);
  pattern.kind         = BTOR_BITVEC_SORT;
  pattern.bitvec.width = width;
  pos                  = find_sort (table, &pattern);
  assert (pos);
  res = *pos;
  if (!res)
  {
    if (BTOR_FULL_SORT_UNIQUE_TABLE (table))
    {
      enlarge_sorts_unique_table (table);
      pos = find_sort (table, &pattern);
      assert (pos);
      res = *pos;
      assert (!res);
    }
    res  = create_sort (btor, table, &pattern);
    *pos = res;
  }
  inc_sort_ref_counter (res);
  return res->id;
}

BtorSortId
btor_array_sort (Btor *btor, BtorSortId index_id, BtorSortId element_id)
{
  assert (btor);
  assert (index_id < BTOR_COUNT_STACK (btor->sorts_unique_table.id2sort));
  assert (element_id < BTOR_COUNT_STACK (btor->sorts_unique_table.id2sort));

  BtorSortId tup, res;
  BtorSort *s;

  tup = btor_tuple_sort (btor, &index_id, 1);
  res = btor_fun_sort (btor, tup, element_id);
  btor_release_sort (btor, tup);
  s               = btor_get_sort_by_id (btor, res);
  s->fun.is_array = true;
  return res;
#if 0
  BtorSort * res, ** pos, pattern, *index, *element;
  BtorSortUniqueTable *table;

  table = &btor->sorts_unique_table;

  index = btor_get_sort_by_id (btor, index_id);
  assert (index);
  assert (index->refs > 0);
  assert (index->table == table);
  element = btor_get_sort_by_id (btor, element_id);
  assert (element);
  assert (element->refs > 0);
  assert (element->table == table);

  BTOR_CLR (&pattern);
  pattern.kind = BTOR_ARRAY_SORT;
  pattern.array.index = index;
  pattern.array.element = element;
  pos = find_sort (table, &pattern);
  assert (pos);
  res = *pos;
  if (!res) 
    {
      if (BTOR_FULL_SORT_UNIQUE_TABLE (table))
	{
	  enlarge_sorts_unique_table (table);
	  pos = find_sort (table, &pattern);
	  assert (pos);
	  res = *pos;
	  assert (!res);
	}
      res = create_sort (btor, table, &pattern);
      *pos = res;
    }
  inc_sort_ref_counter (res);
  return res->id;
#endif
}

BtorSortId
btor_lst_sort (Btor *btor, BtorSortId head_id, BtorSortId tail_id)
{
  assert (btor);
  assert (head_id < BTOR_COUNT_STACK (btor->sorts_unique_table.id2sort));
  assert (tail_id < BTOR_COUNT_STACK (btor->sorts_unique_table.id2sort));

  BtorSort *res, **pos, pattern, *head, *tail;
  BtorSortUniqueTable *table;

  table = &btor->sorts_unique_table;

  head = btor_get_sort_by_id (btor, head_id);
  assert (head);
  assert (head->refs > 0);
  assert (head->table == table);
  tail = btor_get_sort_by_id (btor, tail_id);
  assert (tail);
  assert (tail->refs > 0);
  assert (tail->table == table);

  BTOR_CLR (&pattern);
  pattern.kind     = BTOR_LST_SORT;
  pattern.lst.head = head;
  pattern.lst.tail = tail;
  pos              = find_sort (table, &pattern);
  assert (pos);
  res = *pos;
  if (!res)
  {
    if (BTOR_FULL_SORT_UNIQUE_TABLE (table))
    {
      enlarge_sorts_unique_table (table);
      pos = find_sort (table, &pattern);
      assert (pos);
      res = *pos;
      assert (!res);
    }
    res  = create_sort (btor, table, &pattern);
    *pos = res;
  }
  inc_sort_ref_counter (res);
  return res->id;
}

BtorSortId
btor_fun_sort (Btor *btor, BtorSortId domain_id, BtorSortId codomain_id)
{
  assert (btor);
  assert (domain_id);

  BtorSort *domain, *codomain, *res, **pos, pattern;
  BtorSortUniqueTable *table;

  table = &btor->sorts_unique_table;

  domain = btor_get_sort_by_id (btor, domain_id);
  assert (domain);
  assert (domain->refs > 0);
  assert (domain->table == table);
  assert (domain->kind == BTOR_TUPLE_SORT);
  codomain = btor_get_sort_by_id (btor, codomain_id);
  assert (codomain);
  assert (codomain->refs > 0);
  assert (codomain->table == table);

  BTOR_CLR (&pattern);
  pattern.kind         = BTOR_FUN_SORT;
  pattern.fun.domain   = domain;
  pattern.fun.codomain = codomain;
  pos                  = find_sort (table, &pattern);
  assert (pos);
  res = *pos;
  if (!res)
  {
    if (BTOR_FULL_SORT_UNIQUE_TABLE (table))
    {
      enlarge_sorts_unique_table (table);
      pos = find_sort (table, &pattern);
      assert (pos);
      res = *pos;
      assert (!res);
    }
    res            = create_sort (btor, table, &pattern);
    res->fun.arity = domain->tuple.num_elements;
    *pos           = res;
  }
  inc_sort_ref_counter (res);

  return res->id;
}

BtorSortId
btor_tuple_sort (Btor *btor, BtorSortId *element_ids, size_t num_elements)
{
  assert (btor);
  assert (element_ids);
  assert (num_elements > 0);

  size_t i;
  BtorSort *elements[num_elements], *res, **pos, pattern;
  BtorSortUniqueTable *table;

  table = &btor->sorts_unique_table;

  for (i = 0; i < num_elements; i++)
  {
    elements[i] = btor_get_sort_by_id (btor, element_ids[i]);
    assert (elements[i]);
    assert (elements[i]->table == table);
  }

  BTOR_CLR (&pattern);
  pattern.kind               = BTOR_TUPLE_SORT;
  pattern.tuple.num_elements = num_elements;
  pattern.tuple.elements     = elements;
  pos                        = find_sort (table, &pattern);
  assert (pos);
  res = *pos;
  if (!res)
  {
    if (BTOR_FULL_SORT_UNIQUE_TABLE (table))
    {
      enlarge_sorts_unique_table (table);
      pos = find_sort (table, &pattern);
      assert (pos);
      res = *pos;
      assert (!res);
    }
    res  = create_sort (btor, table, &pattern);
    *pos = res;
  }
  inc_sort_ref_counter (res);
  return res->id;
}

unsigned
btor_get_width_bitvec_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  /* special case for Boolector as boolean are treated as bv of width 1 */
  if (sort->kind == BTOR_BOOL_SORT) return 1;
  assert (sort->kind == BTOR_BITVEC_SORT);
  return sort->bitvec.width;
}

unsigned
btor_get_arity_tuple_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort->kind == BTOR_TUPLE_SORT);
  return sort->tuple.num_elements;
}

BtorSortId
btor_get_codomain_fun_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort->kind == BTOR_FUN_SORT);
  return sort->fun.codomain->id;
}

BtorSortId
btor_get_domain_fun_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort->kind == BTOR_FUN_SORT);
  return sort->fun.domain->id;
}

unsigned
btor_get_arity_fun_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort->kind == BTOR_FUN_SORT);
  assert (sort->fun.domain->kind == BTOR_TUPLE_SORT);
  return sort->fun.domain->tuple.num_elements;
}

BtorSortId
btor_get_index_array_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  if (sort->kind == BTOR_ARRAY_SORT) return sort->array.index->id;
  assert (sort->kind == BTOR_FUN_SORT);
  assert (sort->fun.domain->tuple.num_elements == 1);
  return sort->fun.domain->tuple.elements[0]->id;
}

BtorSortId
btor_get_element_array_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  if (sort->kind == BTOR_ARRAY_SORT) return sort->array.element->id;
  assert (sort->kind == BTOR_FUN_SORT);
  return sort->fun.codomain->id;
}

bool
btor_is_valid_sort (Btor *btor, BtorSortId id)
{
  return id < BTOR_COUNT_STACK (btor->sorts_unique_table.id2sort)
         && BTOR_PEEK_STACK (btor->sorts_unique_table.id2sort, id) != 0;
}

bool
btor_is_bool_sort (Btor *btor, BtorSortId id)
{
  return btor_is_bitvec_sort (btor, id)
         && btor_get_width_bitvec_sort (btor, id) == 1;
}

bool
btor_is_bitvec_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort);
  return sort->kind == BTOR_BITVEC_SORT;
}

bool
btor_is_array_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort);
  return btor_is_fun_sort (btor, id) && sort->fun.is_array;
}

bool
btor_is_tuple_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort);
  return sort->kind == BTOR_TUPLE_SORT;
}

bool
btor_is_fun_sort (Btor *btor, BtorSortId id)
{
  BtorSort *sort;
  sort = btor_get_sort_by_id (btor, id);
  assert (sort);
  return sort->kind == BTOR_FUN_SORT;
}

void
btor_init_tuple_sort_iterator (BtorTupleSortIterator *it,
                               Btor *btor,
                               BtorSortId id)
{
  assert (it);
  assert (btor);
  assert (btor_is_tuple_sort (btor, id));
  it->pos   = 0;
  it->tuple = btor_get_sort_by_id (btor, id);
}

bool
btor_has_next_tuple_sort_iterator (const BtorTupleSortIterator *it)
{
  assert (it);
  return it->pos < it->tuple->tuple.num_elements;
}

BtorSortId
btor_next_tuple_sort_iterator (BtorTupleSortIterator *it)
{
  assert (it);
  assert (it->pos < it->tuple->tuple.num_elements);

  BtorSortId result;
  result = it->tuple->tuple.elements[it->pos]->id;
  it->pos += 1;
  return result;
}
