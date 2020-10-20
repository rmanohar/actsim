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
#ifndef __ACT_CHP_SIM_H__
#define __ACT_CHP_SIM_H__

#include <simdes.h>
#include "actsim.h"


/*--- CHP simulation data structures ---*/

struct chpsimcond {
  Expr *g;
  struct chpsimcond *next;
};

#define CHPSIM_COND   0
#define CHPSIM_ASSIGN 1
#define CHPSIM_SEND   2
#define CHPSIM_RECV   3
#define CHPSIM_FUNC   4  /* built-in functions */
#define CHPSIM_FORK   5

struct chpsimderef {
  Array *range;			// if NULL, then offset is the id
  Expr **chp_idx;
  int *idx;
  int offset;
  act_connection *cx;
};

struct chpsimstmt {
  int type;
  union {
    int fork;			/* # of forks */
    chpsimcond c;		/* conditional */
    struct {
      const char *name;		/* function name */
      list_t *l;		/* arguments */
    } fn;
    struct {
      int isbool;
      struct chpsimderef d;	/* variable deref */
      Expr *e;
    } assign;			/* var := e */
    struct {
      int chvar;
      act_connection *vc;
      list_t *el;		/* list of expressions */
    } send;
    struct {
      int chvar;
      act_connection *vc;
      list_t *vl;		/* list of vars */
    } recv;
  } u;
};


/* --- Each unique process has a CHP sim graph associated with it --- */

class ChpSimGraph {
 public:
  ChpSimGraph (ActSimCore *);
  ActSimCore *state;
  chpsimstmt *stmt;		/* object to simulate */
  int wait;			/* for concurrency */
  int tot;
  ChpSimGraph *next;
  ChpSimGraph **all;		/* successors, if multiple.
				   used by comma and selections 
				*/
  ChpSimGraph *completed (int pc, int *done);

  static ChpSimGraph *buildChpSimGraph (ActSimCore *,
					act_chp_lang_t *, ChpSimGraph **stop);
  
};


/* --- each unique instance has a ChpSim object associated with it --- */

class ChpSim : public ActSimObj {
 public:
  ChpSim (ChpSimGraph *, act_chp_lang_t *, ActSimCore *sim);
     /* initialize simulation, and create initial event */

  void Step (int ev_type);	/* run a step of the simulation */

  void computeFanout ();

  int computeOffset (struct chpsimderef *d);

 private:
  int _npc;			/* # of program counters */
  ChpSimGraph **_pc;		/* current PC state of simulation */

  int _pcused;			/* # of _pc[] slots currently being
				   used */
  int _stalled_pc;
  act_chp_lang_t *_savedc;

  WaitForOne *_probe;
  
  int _max_program_counters (act_chp_lang_t *c);
  void _compute_used_variables (act_chp_lang_t *c);
  void _compute_used_variables_helper (act_chp_lang_t *c);
  void _compute_used_variables_helper (Expr *e);
  struct iHashtable *_tmpused;

  list_t *_statestk;
  expr_res exprEval (Expr *e);
  expr_res funcEval (Function *, int, expr_res *);
  expr_res varEval (int id, int type);
  void _run_chp (act_chp_lang_t *);
  /* type == 3 : probe */

  void varSet (int id, int type, expr_res v);
  int varSend (int pc, int wakeup, int id, expr_res v);
  int varRecv (int pc, int wakeup, int id, expr_res *v);

  int _updatepc (int pc);
  int _add_waitcond (chpsimcond *gc, int pc, int undo = 0);
  int _collect_sharedvars (Expr *e, int pc, int undo);
};



#endif /* __ACT_CHP_SIM_H__ */
