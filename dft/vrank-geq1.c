/*
 * Copyright (c) 2002 Matteo Frigo
 * Copyright (c) 2002 Steven G. Johnson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: vrank-geq1.c,v 1.15 2002-08-26 02:06:52 stevenj Exp $ */


/* Plans for handling vector transform loops.  These are *just* the
   loops, and rely on child plans for the actual DFTs.
 
   They form a wrapper around solvers that don't have apply functions
   for non-null vectors.
 
   vrank-geq1 plans also recursively handle the case of multi-dimensional
   vectors, obviating the need for most solvers to deal with this.  We
   can also play games here, such as reordering the vector loops.
 
   Each vrank-geq1 plan reduces the vector rank by 1, picking out a
   dimension determined by the vecloop_dim field of the solver. */

#include "dft.h"

typedef struct {
     solver super;
     int vecloop_dim;
     const int *buddies;
     uint nbuddies;
} S;

typedef struct {
     plan_dft super;

     plan *cld;
     uint vl;
     int ivs, ovs;
     const S *solver;
} P;

static void apply(plan *ego_, R *ri, R *ii, R *ro, R *io)
{
     P *ego = (P *) ego_;
     uint i, vl = ego->vl;
     int ivs = ego->ivs, ovs = ego->ovs;
     dftapply cldapply = ((plan_dft *) ego->cld)->apply;

     for (i = 0; i < vl; ++i) {
          cldapply(ego->cld,
                   ri + i * ivs, ii + i * ivs, ro + i * ovs, io + i * ovs);
     }
}

static void awake(plan *ego_, int flg)
{
     P *ego = (P *) ego_;
     AWAKE(ego->cld, flg);
}

static void destroy(plan *ego_)
{
     P *ego = (P *) ego_;
     X(plan_destroy)(ego->cld);
     X(free)(ego);
}

static void print(plan *ego_, printer *p)
{
     P *ego = (P *) ego_;
     const S *s = ego->solver;
     p->print(p, "(dft-vrank>=1-x%u/%d%(%p%))",
	      ego->vl, s->vecloop_dim, ego->cld);
}

static int pickdim(const S *ego, tensor vecsz, int oop, uint *dp)
{
     return X(pickdim)(ego->vecloop_dim, ego->buddies, ego->nbuddies,
		       vecsz, oop, dp);
}

static int applicable(const solver *ego_, const problem *p_, uint *dp)
{
     if (DFTP(p_)) {
          const S *ego = (const S *) ego_;
          const problem_dft *p = (const problem_dft *) p_;

          return (1
                  && FINITE_RNK(p->vecsz.rnk)
                  && p->vecsz.rnk > 0
                  && pickdim(ego, p->vecsz, p->ri != p->ro, dp)
	       );
     }

     return 0;
}

static int score(const solver *ego_, const problem *p_, const planner *plnr)
{
     const S *ego = (const S *)ego_;
     const problem_dft *p;
     uint vdim;

     if (!applicable(ego_, p_, &vdim))
          return BAD;

     /* fftw2 behavior */
     if ((plnr->flags & IMPATIENT) && (ego->vecloop_dim != ego->buddies[0]))
	  return BAD;

     p = (const problem_dft *) p_;

     /* fftw2-like heuristic: once we've started vector-recursing,
	don't stop (unless we have to) */
     if ((plnr->flags & FORCE_VRECURSE) && p->vecsz.rnk == 1)
	  return UGLY;

     /* Heuristic: if the transform is multi-dimensional, and the
        vector stride is less than the transform size, then we
        probably want to use a rank>=2 plan first in order to combine
        this vector with the transform-dimension vectors. */
     {
	  iodim *d = p->vecsz.dims + vdim;
	  if (1
	      && p->sz.rnk > 1 
	      && X(imin)(d->is, d->os) < X(tensor_max_index)(p->sz)
	       )
          return UGLY;
     }

     /* Heuristic: don't use a vrank-geq1 for rank-0 vrank-1
	transforms, since this case is better handled by rank-0
	solvers. */
     if (p->sz.rnk == 0 && p->vecsz.rnk == 1)
	  return UGLY;

     return GOOD;
}

static plan *mkplan(const solver *ego_, const problem *p_, planner *plnr)
{
     const S *ego = (const S *) ego_;
     const problem_dft *p;
     P *pln;
     plan *cld;
     problem *cldp;
     uint vdim;
     iodim *d;

     static const plan_adt padt = {
	  X(dft_solve), awake, print, destroy
     };

     if (!applicable(ego_, p_, &vdim))
          return (plan *) 0;
     p = (const problem_dft *) p_;

     /* fftw2 vector recursion: use it or lose it */
     if (p->vecsz.rnk == 1 && (plnr->flags & CLASSIC_VRECURSE))
	  plnr->flags &= ~CLASSIC_VRECURSE & ~FORCE_VRECURSE;

     /* record whether the vector loop would cause either the input or
        the output to become unaligned. */
     d = p->vecsz.dims + vdim;
     if (d->n > 0)
	  if (X(alignment_of)(p->ri + d->is) ||
	      X(alignment_of)(p->ii + d->is) ||
	      X(alignment_of)(p->ro + d->os) || 
	      X(alignment_of)(p->io + d->os))
	       plnr->flags |= POSSIBLY_UNALIGNED;

     cldp = X(mkproblem_dft_d)(X(tensor_copy)(p->sz),
			       X(tensor_copy_except)(p->vecsz, vdim),
			       p->ri, p->ii, p->ro, p->io);
     cld = MKPLAN(plnr, cldp);
     X(problem_destroy)(cldp);
     if (!cld)
          return (plan *) 0;

     pln = MKPLAN_DFT(P, &padt, apply);

     pln->cld = cld;
     pln->vl = d->n;
     pln->ivs = d->is;
     pln->ovs = d->os;

     pln->solver = ego;
     pln->super.super.ops = X(ops_mul)(pln->vl, cld->ops);
     pln->super.super.pcost = pln->vl * cld->pcost;

     return &(pln->super.super);
}

static solver *mksolver(int vecloop_dim, const int *buddies, uint nbuddies)
{
     static const solver_adt sadt = { mkplan, score };
     S *slv = MKSOLVER(S, &sadt);
     slv->vecloop_dim = vecloop_dim;
     slv->buddies = buddies;
     slv->nbuddies = nbuddies;
     return &(slv->super);
}

void X(dft_vrank_geq1_register)(planner *p)
{
     uint i;

     /* FIXME: Should we try other vecloop_dim values? */
     static const int buddies[] = { 1, -1 };

     const uint nbuddies = sizeof(buddies) / sizeof(buddies[0]);

     for (i = 0; i < nbuddies; ++i)
          REGISTER_SOLVER(p, mksolver(buddies[i], buddies, nbuddies));
}
