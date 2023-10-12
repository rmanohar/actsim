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
#include <common/hash.h>


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
#define CHPSIM_NOP    7  /* dummy needed for turning off simulator opt */
#define CHPSIM_CONDARB 8

struct chpsimderef {
  Array *range;			// if NULL, then offset is the id
  Expr **chp_idx;
  int *idx;			// for structures, we use this
				// array. Length is 3x the # of items
				// in the struct. Format: offset,
				// width, type
  Data *d;			// used for structures

  int offset;			// offset / offseti for struct
  int stride;			// stride for struct arrays
  unsigned int isbool:1;	// is this a bool type?
  unsigned int isenum:1;	// is this an enumeration?
  int enum_sz;			// used for enumerations
  int width;
  act_connection *cx;
};

struct chpsimstmt {
  int type;
  int delay_cost;
  int bw_cost;			// can be used for channels
  int energy_cost;
  union {
    int fork;			/* # of forks */
    struct {
      chpsimcond c;		/* conditional */
      unsigned int is_shared:1;	// shared variable in guards for CHPSIM_COND
      unsigned int is_probe:1;	// probe in guards for CHPSIM_COND
      int stats;
    } cond;
    struct {
      const char *name;		/* function name */
      list_t *l;		/* arguments */
    } fn;
    struct {
      short isint;		/* 0 = bool, otherwise bitwidth of int
				   */
      unsigned int is_struct:1;	// 1 if structure, 0 otherwise
      Expr *e;
      struct chpsimderef d;	/* variable deref */
    } assign;			/* var := e */
    struct {
      int chvar;
      act_connection *vc;
      unsigned int d_type:3;	// data type (0, 1, 2)
      unsigned int flavor:2;	// flavor: 0, 1 (+), 2 (-)
      unsigned int is_struct:1;	// 1 if structure, 0 otherwise
      unsigned int is_structx:2; // 0 if not bidir, 1 if bidir, no
				 // 2 if bidir and struct
      int width;		 // channel width
      Expr *e;			// outgoing expression, if any
      struct chpsimderef *d;	// variable, if any
    } sendrecv;
  } u;
};


/* --- Each unique process has a CHP sim graph associated with it --- */

/**
 * The overall information about the chpsim graph
 *  - g : the entry point into the graph
 *  - e : ?
 *  - max_count : number of slots needed to keep track of internal joins
 *  - max_stats : number of slots needed to keep track of any run-time
 *  - labels : map from label to chpsimgraph pointer
 * statistics.
 */
class ChpSimGraph;

struct chpsimgraph_info {
  chpsimgraph_info() {
    g = NULL; labels = NULL; e = NULL; max_count = 0; max_stats = 0;
  }
  ~chpsimgraph_info() { }
  ChpSimGraph *g;
  Expr *e;			/* for probes, if needed */
  int max_count;
  int max_stats;
  struct Hashtable *labels;
};


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

  static chpsimgraph_info *buildChpSimGraph (ActSimCore *, act_chp_lang_t *);
  static int max_pending_count;
  static int max_stats;
  static struct Hashtable *labels;

  static void checkFragmentation (ActSimCore *, ChpSim *, act_chp_lang_t *);
  static void checkFragmentation (ActSimCore *, ChpSim *, Expr *);
  static void checkFragmentation (ActSimCore *, ChpSim *, ActId *, int);
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
  ChpSim (chpsimgraph_info *, act_chp_lang_t *, ActSimCore *sim, Process *p);
     /* initialize simulation, and create initial event */
  ~ChpSim ();

  int Step (Event *ev);		/* run a step of the simulation */

  void reStart (ChpSimGraph *g, int maxcnt);
  
  int jumpTo (const char *s);

  void computeFanout ();

  int computeOffset (const struct chpsimderef *d);

  virtual void propagate (void);

  void zeroInit ();

  void dumpState (FILE *fp);
  unsigned long getEnergy (void);
  double getLeakage (void);
  unsigned long getArea (void);

  void dumpStats (FILE *fp);
  
  int getBool (int glob_off) { return _sc->getBool (glob_off); }
  bool setBool (int glob_off, int val) { return _sc->setBool (glob_off, val); }
  void boolProp (int glob_off);
  void intProp (int glob_off);
  void setFrag (act_channel_state *f) { _frag_ch = f; }

  void awakenDeadlockedGC ();
  void skipChannelAction (int is_send, int offset);

  BigInt exprEval (Expr *e);

  void setHseMode() { _hse_mode = 1; }
  int isHseMode() { return _hse_mode; }
  

 private:
  int _npc;			/* # of program counters */
  int _pcused;			/* # of _pc[] slots currently being
				   used */
  
  struct Hashtable *_labels;	/* label map */
  
  ChpSimGraph **_pc;		/* current PC state of simulation */
  int *_holes;			/* available slots in the _pc array */
  int *_tot;			/* current pending concurrent count */

  list_t *_deadlock_pc;
  list_t *_stalled_pc;
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
  Scope *_cureval;
  act_channel_state *_frag_ch;	// fragmented channel


  unsigned long *_stats;
  int _maxstats;
  int _hse_mode;		// is this a HSE?

  BigInt funcEval (Function *, int, void **);
  BigInt varEval (int id, int type);
  expr_multires varChanEvalStruct (int id, int type);
  
  expr_multires exprStruct (Expr *e);
  expr_multires funcStruct (Function *, int, void **);
  expr_multires varStruct (struct chpsimderef *);

  int _structure_assign (struct chpsimderef *, expr_multires *);
  
  void _run_chp (Function *fn, act_chp_lang_t *);
  /* type == 3 : probe */

  int varSend (int pc, int wakeup, int id, int off, int flavor,
	       expr_multires &v, int bidir, expr_multires *xchg, int *frag,
	       int *skipwrite);
  int varRecv (int pc, int wakeup, int id, int off, int flavor,
	       expr_multires *v, int bidir, expr_multires &xchg, int *frag,
	       int *skipwrite);

  int chkWatchBreakPt (int type, int loff, int goff, const BigInt &v, int flag = 0);
  

  int _updatepc (int pc);
  int _add_waitcond (chpsimcond *gc, int pc, int undo = 0);
  int _collect_sharedvars (Expr *e, int pc, int undo);
  void _remove_me (int pc);

  int _nextEvent (int pc, int bw_delay);
  void _initEvent ();
  void _zeroAllIntsChans (ChpSimGraph *g);
  void _zeroStructure (struct chpsimderef *d);

};


#endif /* __ACT_CHP_SIM_H__ */
