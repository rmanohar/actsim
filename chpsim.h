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

#include <common/simdes.h>
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
#define CHPSIM_LOOP   6
#define CHPSIM_SEND_STRUCT 7
#define CHPSIM_RECV_STRUCT 8
#define CHPSIM_ASSIGN_STRUCT 9

struct chpsimderef {
  Array *range;			// if NULL, then offset is the id
  Expr **chp_idx;
  int *idx;			// for structures, we use this
				// array. Length is 3x the # of items
				// in the struct. Format: offset,
				// width, type
  Data *d;			// used for structures

  int offset;			// offset / offseti for struct
  int width;			// for all vars / offsetb for struct
  act_connection *cx;
};

struct chpsimstmt {
  int type;
  int delay_cost;
  int energy_cost;
  union {
    int fork;			/* # of forks */
    chpsimcond c;		/* conditional */
    struct {
      const char *name;		/* function name */
      list_t *l;		/* arguments */
    } fn;
    struct {
      int isint;		/* 0 = bool, otherwise bitwidth of int
				   */
      Expr *e;
      struct chpsimderef d;	/* variable deref */
    } assign;			/* var := e */
    struct {
      int chvar;
      act_connection *vc;
      int d_type;
      Expr *e;
      struct chpsimderef *d;
    } sendrecv;
  } u;
};


/* --- Each unique process has a CHP sim graph associated with it --- */

class ChpSimGraph {
 public:
  ChpSimGraph (ActSimCore *);
  ~ChpSimGraph ();
  
  ActSimCore *state;
  chpsimstmt *stmt;		/* object to simulate */
  int wait;			/* for concurrency */
  int totidx;			/* index into pending count */
  ChpSimGraph *next;
  ChpSimGraph **all;		/* successors, if multiple.
				   used by comma and selections 
				*/
  ChpSimGraph *completed (int pc, int *tot, int *done);
  void printStmt (FILE *fp, Process *p);

  static ChpSimGraph *buildChpSimGraph (ActSimCore *,
					act_chp_lang_t *, ChpSimGraph **stop);
  static int max_pending_count;

  static void checkFragmentation (ActSimCore *, ChpSim *, act_chp_lang_t *);
  static void checkFragmentation (ActSimCore *, ChpSim *, Expr *);
  static void checkFragmentation (ActSimCore *, ChpSim *, ActId *);
  static void recordChannel (ActSimCore *, ChpSim *, ActId *);
  static void recordChannel (ActSimCore *, ChpSim *, act_chp_lang_t *);
private:
  static ChpSimGraph *_buildChpSimGraph (ActSimCore *,
					 act_chp_lang_t *, ChpSimGraph **stop);
  static int cur_pending_count;

};


/* --- each unique instance has a ChpSim object associated with it --- */

class ChpSim : public ActSimObj {
 public:
  ChpSim (ChpSimGraph *, int maxcnt, act_chp_lang_t *, ActSimCore *sim,
	  Process *p);
     /* initialize simulation, and create initial event */
  ~ChpSim ();

  void Step (int ev_type);	/* run a step of the simulation */

  void reStart (ChpSimGraph *g, int maxcnt);

  void computeFanout ();

  int computeOffset (struct chpsimderef *d);

  void dumpState (FILE *fp);
  unsigned long getEnergy (void);
  double getLeakage (void);
  unsigned long getArea (void);

 private:
  int _npc;			/* # of program counters */
  ChpSimGraph **_pc;		/* current PC state of simulation */
  int *_tot;			/* current pending concurrent count */

  int _pcused;			/* # of _pc[] slots currently being
				   used */
  int _stalled_pc;
  act_chp_lang_t *_savedc;

  unsigned long _energy_cost;
  double _leakage_cost;
  unsigned long _area_cost;

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
  
  expr_multires exprStruct (Expr *e);
  expr_multires funcStruct (Function *, int, expr_res *);
  expr_multires varStruct (struct chpsimderef *);
  
  void _run_chp (act_chp_lang_t *);
  /* type == 3 : probe */

  int varSend (int pc, int wakeup, int id, expr_res v);
  int varRecv (int pc, int wakeup, int id, expr_res *v);

  int _updatepc (int pc);
  int _add_waitcond (chpsimcond *gc, int pc, int undo = 0);
  int _collect_sharedvars (Expr *e, int pc, int undo);

  int _nextEvent (int pc);
};



#endif /* __ACT_CHP_SIM_H__ */
