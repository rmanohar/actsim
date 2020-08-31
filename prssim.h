/*************************************************************************
 *
 *  Copyright (c) 2020 Rajit Manohar
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *
 **************************************************************************
 */
#ifndef __ACT_PRS_SIM_H__
#define __ACT_PRS_SIM_H__

#include <simdes.h>
#include "actsim.h"

/*
 * up, down, wup, wdn
 */
#define PRSSIM_EXPR_AND 0
#define PRSSIM_EXPR_OR 1
#define PRSSIM_EXPR_NOT 2
#define PRSSIM_EXPR_VAR 3
#define PRSSIM_EXPR_TRUE 4
#define PRSSIM_EXPR_FALSE 5

struct prssim_expr {
  unsigned int type:3;  /* AND, OR, NOT, VAR */
  union {
    struct {
      prssim_expr *l, *r;
    };
    int vid;
  };
};

#define PRSSIM_RULE  0
#define PRSSIM_PASSP 1
#define PRSSIM_PASSN 2
#define PRSSIM_TGATE 3

#define PRSSIM_NORM 0
#define PRSSIM_WEAK 1

struct prssim_stmt {
  unsigned int type:2; /* RULE, P, N, TRANSGATE */
  struct prssim_stmt *next;
  union {
    struct {
      prssim_expr *up[2], *dn[2];
      int vid;
    };
    struct {
      int t1, t2, g, _g;
    };
  };
};
  

class PrsSimGraph {
private:
  struct prssim_stmt *_rules, *_tail;
  struct Hashtable *_labels;

  void _add_one_rule (ActSimCore *, act_prs_lang_t *);
  void _add_one_gate (ActSimCore *, act_prs_lang_t *);
  
public:
  PrsSimGraph();
  
  void addPrs (ActSimCore *, act_prs_lang_t *);

  prssim_stmt *getRules () { return _rules; }


  static PrsSimGraph *buildPrsSimGraph (ActSimCore *, act_prs *, act_spec *);
  
};

class PrsSim : public ActSimObj {
 public:
  PrsSim (PrsSimGraph *, ActSimCore *sim);
     /* initialize simulation, and create initial event */

  void Step (int ev_type);	/* run a step of the simulation */

  void computeFanout ();

  int getBool (int lid) { int off = getGlobalOffset (lid, 0); return _sc->getBool (off); }
    
  void setBool (int lid, int v) { int off = getGlobalOffset (lid, 0); _sc->setBool (off, v); }
  
 private:
  void _computeFanout (prssim_expr *, SimDES *);
  
  void varSet (int id, int type, expr_res v);
  int varSend (int pc, int wakeup, int id, expr_res v);
  int varRecv (int pc, int wakeup, int id, expr_res *v);

  ActSimCore *_sc;
  PrsSimGraph *_g;
};


/*-- not actsimobj so that it can be lightweight --*/
class OnePrsSim : public SimDES {
private:
  PrsSim *_proc;		// process core
  struct prssim_stmt *_me;	// the rule
  void propagate ();
  int eval (prssim_expr *);
public:
  OnePrsSim (PrsSim *p, struct prssim_stmt *x) { _proc = p; _me = x; }
  void Step (int ev_type);
};


#endif /* __ACT_CHP_SIM_H__ */
