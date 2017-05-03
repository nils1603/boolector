/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2007-2009 Robert Daniel Brummayer.
 *  Copyright (C) 2007-2014 Armin Biere.
 *  Copyright (C) 2013-2017 Aina Niemetz.
 *  Copyright (C) 2012-2017 Mathias Preiner.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "sat/btorlgl.h"

/*------------------------------------------------------------------------*/
#ifdef BTOR_USE_LINGELING
/*------------------------------------------------------------------------*/

#include <ctype.h>
#include <limits.h>
#include "btorabort.h"
#include "btorcore.h"
#include "btoropt.h"

#define BTOR_LGL_SIMP_DELAY 10000
#define BTOR_LGL_MIN_BLIMIT 50000
#define BTOR_LGL_MAX_BLIMIT 200000

/*------------------------------------------------------------------------*/

static void *
init (BtorSATMgr *smgr)
{
  BtorLGL *res;

  if (btor_opt_get (smgr->btor, BTOR_OPT_VERBOSITY) >= 1)
  {
    lglbnr ("Lingeling", "[lingeling] ", stdout);
    fflush (stdout);
  }

  BTOR_CNEW (smgr->btor->mm, res);
  res->lgl = lglminit (smgr->btor->mm,
                       (lglalloc) btor_mem_sat_malloc,
                       (lglrealloc) btor_mem_sat_realloc,
                       (lgldealloc) btor_mem_sat_free);

  res->blimit = BTOR_LGL_MIN_BLIMIT;

  return res;
}

static void
add (BtorSATMgr *smgr, int lit)
{
  BtorLGL *blgl = smgr->solver;
  lgladd (blgl->lgl, lit);
}

static int
sat (BtorSATMgr *smgr, int limit)
{
  BtorLGL *blgl = smgr->solver;
  LGL *lgl      = blgl->lgl, *clone;
  const char *str;
  int res, bfres;
  char name[80];

  assert (smgr->satcalls >= 1);

  lglsetopt (lgl, "simplify", 1);

#ifdef BTOR_PRINT_DIMACS_FOR_LINGELING
  {
    static int count = 0;
    char name[80];
    FILE *file;
    sprintf (name, "/tmp/btor_sat_%05d_%08d.cnf", getpid (), count++);
    file = fopen (name, "w");
    lglprint (lgl, file);
    fclose (file);
    BTOR_MSG (smgr->btor->msg, 0, "wrote %s", name);
  }
#endif

  if (smgr->inc_required
      && (smgr->satcalls == 1 || (smgr->satcalls & (smgr->satcalls - 1))))
    lglsetopt (lgl, "simpdelay", BTOR_LGL_SIMP_DELAY);
  else
    lglsetopt (lgl, "simpdelay", 0);

  if (smgr->inc_required)
  {
    lglsetopt (lgl, "flipping", 0);
    lglsetopt (lgl, "locs", 0);
  }
  else
  {
    lglsetopt (lgl, "clim", -1);
    res = lglsat (lgl);
    return res;
  }

  if (!smgr->fork || (0 <= limit && limit < blgl->blimit))
  {
    if (limit < INT_MAX) lglsetopt (lgl, "clim", limit);
    res = lglsat (lgl);
  }
  else
  {
    BTOR_MSG (smgr->btor->msg, 2, "blimit = %d", blgl->blimit);
    lglsetopt (lgl, "clim", blgl->blimit);
    if (!(res = lglsat (lgl)))
    {
      blgl->blimit *= 2;
      if (blgl->blimit > BTOR_LGL_MAX_BLIMIT)
        blgl->blimit = BTOR_LGL_MAX_BLIMIT;

      blgl->nforked++;
      clone = lglclone (lgl);
      lglfixate (clone);
      lglmeltall (clone);
      str = "clone";
      lglsetopt (clone, "clim", limit);
      /* callbacks are not cloned in Lingeling */
      if (smgr->term.fun) lglseterm (clone, smgr->term.fun, smgr->term.state);
      sprintf (
          name, "[%s lgl%s%d] ", smgr->btor->msg->prefix, str, blgl->nforked);
      lglsetprefix (clone, name);
      lglsetout (clone, smgr->output);

#ifndef NDEBUG
      res =
#else
      (void)
#endif
          lglsat (clone);
      if (btor_opt_get (smgr->btor, BTOR_OPT_VERBOSITY) > 0) lglstats (clone);
      bfres = lglunclone (lgl, clone);
      lglrelease (clone);
      assert (!res || bfres == res);
      res = bfres;
    }
    else
    {
      blgl->blimit = 9 * (blgl->blimit / 10);
      if (blgl->blimit < BTOR_LGL_MIN_BLIMIT)
        blgl->blimit = BTOR_LGL_MIN_BLIMIT;
    }
  }
  return res;
}

static int
deref (BtorSATMgr *smgr, int lit)
{
  BtorLGL *blgl = smgr->solver;
  return lglderef (blgl->lgl, lit);
}

static int
repr (BtorSATMgr *smgr, int lit)
{
  BtorLGL *blgl = smgr->solver;
  return lglrepr (blgl->lgl, lit);
}

static void
reset (BtorSATMgr *smgr)
{
  BtorLGL *blgl = smgr->solver;
  lglrelease (blgl->lgl);
  BTOR_DELETE (smgr->btor->mm, blgl);
}

static void
set_output (BtorSATMgr *smgr, FILE *output)
{
  BtorLGL *blgl = smgr->solver;
  lglsetout (blgl->lgl, output);
}

static void
set_prefix (BtorSATMgr *smgr, const char *prefix)
{
  BtorLGL *blgl = smgr->solver;
  lglsetprefix (blgl->lgl, prefix);
}

static void
enable_verbosity (BtorSATMgr *smgr, int level)
{
  BtorLGL *blgl = smgr->solver;
  if (level <= 0)
    lglsetopt (blgl->lgl, "verb", -1);
  else if (level >= 2)
    lglsetopt (blgl->lgl, "verb", level - 1);
}

static int
inc_max_var (BtorSATMgr *smgr)
{
  BtorLGL *blgl = smgr->solver;
  int res       = lglincvar (blgl->lgl);
  if (smgr->inc_required) lglfreeze (blgl->lgl, res);
  return res;
}

static void
stats (BtorSATMgr *smgr)
{
  BtorLGL *blgl = smgr->solver;
  lglstats (blgl->lgl);
  BTOR_MSG (smgr->btor->msg, 1, "%d forked", blgl->nforked);
}

/*------------------------------------------------------------------------*/

static void
assume (BtorSATMgr *smgr, int lit)
{
  BtorLGL *blgl = smgr->solver;
  lglassume (blgl->lgl, lit);
}

static void
melt (BtorSATMgr *smgr, int lit)
{
  BtorLGL *blgl = smgr->solver;
  if (smgr->inc_required) lglmelt (blgl->lgl, lit);
}

static int
failed (BtorSATMgr *smgr, int lit)
{
  BtorLGL *blgl = smgr->solver;
  return lglfailed (blgl->lgl, lit);
}

static int
fixed (BtorSATMgr *smgr, int lit)
{
  BtorLGL *blgl = smgr->solver;
  return lglfixed (blgl->lgl, lit);
}

static void *
clone (BtorSATMgr *smgr, BtorMemMgr *mm)
{
  assert (smgr);

  BtorLGL *res, *blgl;

  blgl = smgr->solver;

  /* not initialized yet */
  if (!blgl) return 0;

  BTOR_CNEW (mm, res);
  res->nforked = blgl->nforked;
  res->blimit  = blgl->blimit;
  res->lgl     = lglmclone (blgl->lgl,
                        mm,
                        (lglalloc) btor_mem_sat_malloc,
                        (lglrealloc) btor_mem_sat_realloc,
                        (lgldealloc) btor_mem_sat_free);
  return res;
}

static void
setterm (BtorSATMgr *smgr)
{
  assert (smgr);

  BtorLGL *blgl;
  blgl = smgr->solver;
  lglseterm (blgl->lgl, smgr->term.fun, smgr->term.state);
}

/*------------------------------------------------------------------------*/

bool
btor_sat_enable_lingeling (BtorSATMgr *smgr)
{
  assert (smgr != NULL);

  BTOR_ABORT (smgr->initialized,
              "'btor_sat_init' called before 'btor_sat_enable_lingeling'");

  smgr->name = "Lingeling";
  smgr->fork = btor_opt_get (smgr->btor, BTOR_OPT_SAT_ENGINE_LGL_FORK);

  BTOR_CLR (&smgr->api);
  smgr->api.add              = add;
  smgr->api.assume           = assume;
  smgr->api.deref            = deref;
  smgr->api.enable_verbosity = enable_verbosity;
  smgr->api.failed           = failed;
  smgr->api.fixed            = fixed;
  smgr->api.inc_max_var      = inc_max_var;
  smgr->api.init             = init;
  smgr->api.melt             = melt;
  smgr->api.repr             = repr;
  smgr->api.reset            = reset;
  smgr->api.sat              = sat;
  smgr->api.set_output       = set_output;
  smgr->api.set_prefix       = set_prefix;
  smgr->api.stats            = stats;
  smgr->api.clone            = clone;
  smgr->api.setterm          = setterm;
  return true;
}

/*------------------------------------------------------------------------*/
#endif
/*------------------------------------------------------------------------*/
