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
#ifndef __ACT_SIM__H__
#define __ACT_SIM__H__

#include <common/bitset.h>
#include <common/simdes.h>
#include <act/tracelib.h>
#include <string.h>
#include <act/act.h>
#include <act/passes.h>
#include <regex.h>
#include <stdlib.h>
#include <math.h>
#include <common/int.h>
#include "actsim_ext.h"
#include "state.h"
#include "channel.h"

#define E_CHP_VARBOOL  (E_NEWEND + 1)
#define E_CHP_VARINT   (E_NEWEND + 2)
#define E_CHP_VARCHAN  (E_NEWEND + 3)

#define E_CHP_VARBOOL_DEREF  (E_NEWEND + 4)
#define E_CHP_VARINT_DEREF   (E_NEWEND + 5)
//#define E_CHP_VARCHAN_DEREF  (E_NEWEND + 6)

#define E_PROBEIN  (E_NEWEND + 7)
#define E_PROBEOUT  (E_NEWEND + 8)

#define E_CHP_BITFIELD (E_NEWEND + 9)

#define E_CHP_VARSTRUCT_DEREF (E_NEWEND + 10)
#define E_CHP_VARSTRUCT       (E_NEWEND + 11)

/*
 *
 * Core simulation library
 *
 */

class ActSimCore;
class OnePrsSim;


#define MAX_LOCAL_PCS SIM_EV_MAX


/*
 * Maximum number of formats that are simultaneously supported in a
 * session.
 */
#define TRACE_NUM_FORMATS 3

class ActSimState {
public:
  ActSimState (int bools, int ints, int chans);
  ~ActSimState ();

  BigInt *getInt (int x);
  void setInt (int x, BigInt &v);
  int getBool (int x);
  inline bool isSpecialBool (int x) { return bitset_tst (bits, 3*x+2); }
  void mkSpecialBool (int x) { bitset_set (bits, 3*x+2); }
  bool setBool (int x, int v); // success == true
  act_channel_state *getChan (int x);
  int numChans () { return nchans; }

  void *allocState (int sz);

  void mkHazard (int v) {
    if (!hazards && nbools > 0) {
      hazards = bitset_new (nbools);
      bitset_set (hazards, v);
    }
  }
  bool isHazard (int v) {
    if (!hazards) return false;
    if (bitset_tst (hazards, v)) {
      return true;
    }
    else {
      return false;
    }
  }

private:
  bitset_t *hazards;		/* hazard information */
  bitset_t *bits;		/* Booleans */
  int nbools;			/* # of Booleans */
  
  BigInt *ival;			/* integers */
  int nints;			/* number of integers */


  act_channel_state *chans;	/* channel state */
  int nchans;			/* numchannels */

  list_t *extra_state;		/* any extra state needed */
};


class ActSimDES : public SimDES {
public:
  virtual ~ActSimDES() { };
  virtual void propagate () { };
};

class ActSimObj;

struct ActInstTable {
  struct Hashtable *H;	// sub-instances (optional)
  ActSimObj *obj;	// simulation object
};


class ActSimObj : public ActSimDES {
public:
  ActSimObj (ActSimCore *sim, Process *p);
  virtual ~ActSimObj();

  int getGlobalOffset (int loc, int type); // 0 = bool, 1 = int,
                                           // 2 = chan

  void setOffsets (state_counts *x) { _o = *x; }
  void setPorts (int *_bool, int *_int, int *_chan) {
    _abs_port_bool = _bool;
    _abs_port_int = _int;
    _abs_port_chan = _chan;
  }

  virtual void zeroInit () { }

  void setNameAlias (ActId *id) { name = id; }
  void setName (ActId *id) { if (id) { name = id->Clone(); } else { name = NULL; } }
  ActId *getName () { return name; }
  Process *getProc () { return _proc; }

  virtual void dumpState (FILE *fp) { };
  virtual unsigned long getEnergy () { return 0; }
  virtual double getLeakage () { return 0.0; }
  virtual unsigned long getArea () { return 0; }
  virtual void printStatus (int val, bool io_glob = false) { }

  virtual void propagate ();
  virtual void computeFanout() { printf ("should not be here\n"); }

  /* manipulate object watchpoint, using local index values */
  void addWatchPoint (int type, int idx, const char *name);
  void toggleBreakPt (int type, int idx, const char *name);
  void delWatchPoint (int type, int idx);

  void msgPrefix (FILE *fp = NULL);

  void sWakeup() { _shared->Notify (MAX_LOCAL_PCS); }
  void sStall () { _shared->AddObject (this); }
  void sRemove() { _shared->DelObject (this); }
  int  sWaiting() { return _shared->isWaiting (this); }

protected:
  state_counts _o;		/* my state offsets for all local
				   state */
  ActSimCore *_sc;

  ActId *name;
  Process *_proc;

  int *_abs_port_bool;		/* index of ports: absolute scale */
  int *_abs_port_chan;		/* these arrays are reversed! */
  int *_abs_port_int;

  WaitForOne *_shared;
};

class ActSimState;

class ActExclConstraint {
private:
  int sz;
  int *n;			// nodes
  ActExclConstraint **nxt;
  OnePrsSim **objs;

  static iHashtable *eHashHi, *eHashLo;	// map from bool id to root of
					// the constraint list

public:
  ActExclConstraint (int *nodes, int sz, int dir);  

  int illegal () { return sz > 0 ? 0 : 1; }

  void addObject (int id, OnePrsSim *obj);

  ActExclConstraint *getNext (int nid);

  static void Init ();
  static ActExclConstraint *findHi (int n);
  static ActExclConstraint *findLo (int n);
  static int safeChange (ActSimState *, int n, int v);
  static ActSimCore *_sc;

};

#define ACT_TIMING_INACTIVE     0x0
#define ACT_TIMING_START        0x1
#define ACT_TIMING_PENDING      0x2
#define ACT_TIMING_PENDINGDELAY 0x3

class ActTimingConstraint {
private:
  ActSimObj *obj;		// the instance that registered this constraint
  int n[3];			// n[0] : n[1] < n[2]
  act_connection *c[3];
  ActTimingConstraint *nxt[3];	// next pointers
  int margin;			// constraint margin
  
  unsigned long ts;		// time stamp
  struct _internal_ {
    unsigned int up:1;
    unsigned int dn:1;
  } f[3];
  unsigned int state:2;		// constraint state machine

  static iHashtable *THash;	// map from bool id to root of the
				// constraint list

public:
  static void Init ();

  static ActTimingConstraint *findBool (int n);
  
  ActTimingConstraint (ActSimObj *_obj, int root, int a, int b, int margin, int *extra);
  ~ActTimingConstraint ();

  ActTimingConstraint *getNext (int sig);
  void update (int sig, int v);

  void Print (FILE *fp);
  
  void setConn (int idx, act_connection *_c) { c[idx] = _c; }

  int isDup() { return n[0] >= 0 ? 0 : 1; }
  int isEqual (ActTimingConstraint *);
};


class ChpSimGraph;
class ChpSim;
class PrsSim;
class XyceSim;

/*
 * Core simulation engine. 
 *
 *   Used to maintain state and compute pending events/etc.
 *
 */
class ActSimCore {
 public:
  ActSimCore (Process *root = NULL); /* create simulation engine */
  ~ActSimCore ();

  void addStdEnv ();
     /* wrap the top-level process with its standard environment */

  Event *pendingEvents ();
     /* Returns the list of pending events */

     /* get/set the current state */
  ActSimState *getState () { return state; }
  void setState (ActSimState *);

  BigInt *getInt (int x) { return state->getInt (x); }
  void setInt (int x, BigInt &v) { state->setInt (x, v); }
  int getBool (int x) { return state->getBool (x); }
  bool setBool (int x, int v) { return state->setBool (x, v); }
  int isSpecialBool (int x)  { return state->isSpecialBool (x); }
  bool isHazard (int x) { return state->isHazard (x); }
  
  act_channel_state *getChan (int x) { return state->getChan (x); }

  Scope *CurScope() { return _curproc ? _curproc->CurScope() : root_scope; }
  stateinfo_t *cursi() { return _cursi; }
  void setsi(stateinfo_t *si) { _cursi = si; }
  stateinfo_t *getsi(Process *p) { return sp->getStateInfo (p); }
  act_boolean_netlist_t *getbnl (Process *p) { return bp->getBNL (p); }
  ActId *curinst() { return _curinst; }
  list_t *sistack() { return _si_stack; }
  list_t *objstack() { return _obj_stack; }

  void checkFragmentation (ActId *id, ActSimObj *obj, stateinfo_t *si, int read_only);
  void checkFragmentation (act_connection *idc, ActId *reqid, ActSimObj *obj, stateinfo_t *si, int read_only);
  

  int hasLocalOffset (ActId *id, stateinfo_t *si);
  int hasLocalOffset (act_connection *c, stateinfo_t *si);
  int getLocalOffset (ActId *id, stateinfo_t *si, int *type, int *width = NULL);
  int getLocalOffset (act_connection *c, stateinfo_t *si, int *type, int *width = NULL);
  /* encoding: >= 0 = local state. Add to process offset to get global
     offset.

     for negative:
        -2x : global negative "x"; add "x" to total globals to get abs offset
   (-2x + 1): port offset. add "x" to total ports to get port
              offset

      sets type to 0, 1, 2, 3 for bool, int, chan(in), chan(out)
  */

  int getLocalDynamicStructOffset (act_connection *c,
				   stateinfo_t *si,
				   int *offset_i, int *offset_b);

  act_connection *getConnFromOffset (Process *p, int off, int type, int *dy);

#if 0  
  void gStall (SimDES *s) { state->gStall (s); }
  void gRemove (SimDES *s) { state->gRemove (s); }
  void gWakeup () { state->gWakeup(); }
#endif

  void incFanout (int off, int type, SimDES *who);
  int numFanout (int off, int type) { if (type == 0) return nfo[off]; else return nfo[off+nint_start];
}
  SimDES **getFO (int off, int type) { if (type == 0) { return fo[off]; } else { return fo[off+nint_start]; } }
    
  void logFilter (const char *s);
  int isFiltered (const char *s);

  void setMode (int mode) { _prs_sim_mode = mode; }
  void setRandom () { _sim_rand = 1; }
  void setNoRandom() { _sim_rand = 0; }
  void setRandom (int min, int max) {
    _sim_rand = 2; _rand_min = min; _rand_max = max;
  }
  void setRandomSeed (unsigned seed) { _seed = seed; }
  void setRandomChoice (int v) { _sim_rand_excl = v; }
  int isRandomChoice() { return _sim_rand_excl; }
  int isResetMode() { return _prs_sim_mode; }
  void setWarning (int v) { _on_warning = v; }
  inline int onWarning() { return _on_warning; }

  void registerFragmented (Channel *c);
  ChanMethods *getFragmented (Channel *c);

  phash_bucket_t *exprWidth (Expr *e) { return phash_lookup (ewidths, e); }
  phash_bucket_t *exprAddWidth (Expr *e) { return phash_add (ewidths, e); }


#define LN_MAX_VAL 11.0903548889591  /* log(1 << 16) */

  inline int getRandom (int range) {
    return rand_r (&_seed) % range;
  }

  inline int getDelay (int delay) {
    double d;
    unsigned long val;

    if (_sim_rand == 0 || delay == 0) {
      return delay;
    }
    else if (_sim_rand == 1) {
      d = (0.0 + rand_r (&_seed))/RAND_MAX;
      val = exp(d*LN_MAX_VAL)-1;
    }
    else if (_sim_rand == 2) {
      d = (0.0 + rand_r (&_seed))/RAND_MAX;
      val = _rand_min + d*(_rand_max - _rand_min);
    }
    else {
      val = 0;
    }
    if (val == 0) { val = 1; }
    return val;
  }

  int infLoopOpt() { return _inf_loop_opt; }

  void computeFanout (ActInstTable *inst);

  struct watchpt_bucket {
    char *s;
    unsigned int ignore_fmt;
    void *node[TRACE_NUM_FORMATS];
  };

  inline void addWatchPt (int type, unsigned long off, const char *name) {
    ihash_bucket_t *b;
    watchpt_bucket *w;
    if (type == 3) { type = 2; }
    b = ihash_lookup (_W, ((unsigned long)type) | (off << 2));
    if (b) {
      w = (watchpt_bucket *) b->v;
      FREE (w->s);
      FREE (w);
    }
    else {
      b = ihash_add (_W, ((unsigned long)type) | (off << 2));
    }
    NEW (w, watchpt_bucket);
    b->v = w;
    w->s = Strdup (name);
    w->ignore_fmt = ~0U;
    for (int i=0; i < TRACE_NUM_FORMATS; i++) {
      w->node[i] = NULL;
    }
  }

  inline const watchpt_bucket *chkWatchPt (int type, unsigned long off) {
    ihash_bucket_t *b;
    watchpt_bucket *w;
    if (type == 3) { type = 2; }
    b = ihash_lookup (_W, ((unsigned long)type) | (off << 2));
    if (b) {
      w = (watchpt_bucket *) b->v;
      return w;
    }
    else {
      return nullptr;
    }
  }

  inline void delWatchPt (int type, unsigned long off) {
    ihash_bucket_t *b;
    watchpt_bucket *w;
    if (type == 3) { type = 2; }
    b = ihash_lookup (_W, ((unsigned long)type) | (off << 2));
    if (b) {
      w = (watchpt_bucket *) b->v;
      ihash_delete (_W, ((unsigned long)type) | (off << 2));
      FREE (w->s);
      FREE (w);
    }
  }

  inline const char *chkBreakPt (int type, unsigned long off) {
    ihash_bucket_t *b;
    if (type == 3) { type = 2; }
    b = ihash_lookup (_B, ((unsigned long)type) | (off << 2));
    if (b) {
      return (char *)b->v;
    }
    else {
      return nullptr;
    }
  }

  inline void toggleBreakPt (int type, unsigned long off, const char *name) {
    ihash_bucket_t *b;
    if (type == 3) { type = 2; }
    b = ihash_lookup (_B, ((unsigned long)type) | (off << 2));
    if (b) {
      FREE (b->v);
      ihash_delete (_B, ((unsigned long)type) | (off << 2));
    }
    else {
      b = ihash_add (_B, ((unsigned long)type) | (off << 2));
      b->v = Strdup (name);
    }
  }

  int initTrace (int fmt, const  char *name); // clear when it is NULL
  act_trace_t *getTrace (int fmt) { return _tr[fmt]; }
  void recordTrace (const watchpt_bucket *w, int type,
		    act_chan_state_t chan_state, const BigInt &val);

  void setTimescale (float tm) { _int_to_float_timescale = tm*1e-12; }
  float getTimescale() { return _int_to_float_timescale; }

  double curTimeMetricUnits() {
    double f;
    BigInt tm = SimDES::CurTime();
    if (tm.getLen() <= 1) {
      f = tm.getVal (0)*(double) _int_to_float_timescale;
    }
    else {
      f = 0;
      for (int i=0; i < tm.getLen(); i++) {
	f *= (1UL << 32);
	f *= (1UL << 32);
	f += tm.getVal (i)* (double) _int_to_float_timescale;
      }
    }
    return f;
  }


  int isInternalParallel() { return _is_internal_parallel; }
  void setInternalParallel (int v) { _is_internal_parallel = v; }

  int trIndex (const char *s) {
    for (int i=0; i < TRACE_NUM_FORMATS; i++) {
      if (_trname[i] && strcmp (s, _trname[i]) == 0) {
	return i;
      }
    }
    return -1;
  }

  int useOrAllocTrIndex (const char *s) {
    /* have we loaded this format already? */
    int i = trIndex (s);
    if (i != -1) {
      return i;
    }

    /* is there an open slot to load the format? */
    for (i=0; i < TRACE_NUM_FORMATS; i++) {
      if (!_trname[i]) {
	_trfn[i] = act_trace_load_format (s, NULL);
	if (!_trfn[i]) {
	  return -1;
	}
	_trname[i] = Strdup (s);
	return i;
      }
    }
    
    /* can we free an earlier loaded format and replace it with this
       one? */
    for (i=0; i < TRACE_NUM_FORMATS; i++) {
      if (!_tr[i]) {
	act_extern_trace_func_t *tmp = act_trace_load_format (s, NULL);
	if (!tmp) {
	  return -1;
	}
	FREE (_trname[i]);
	act_trace_close_format (_trfn[i]);
	_trfn[i] = tmp;
	_trname[i] = Strdup (s);
	return i;
      }
    }

    /* all attempts failed */
    return -1;
  }

  /*
   * Release the trace file 
   */
  void releaseTrIndex (const char *s) {
    int i = trIndex (s);
    /* close the trace file if it is open */
    initTrace (i, NULL);
    FREE (_trname[i]);
    _trname[i] = NULL;
  }

protected:
  Act *a;

  int nfo_len;
  int nint_start;
  int *nfo;			// nbools + nint length (=nfo_len), contains
				// fanout count for each variable
  
  SimDES ***fo;			// fanout destinations
  struct iHashtable *hfo;	// for high fanout nets

  struct iHashtable *map;	/* map from process pointer to
				   process_info */

  struct pHashtable *chan;	// compiled channel methods table

  struct pHashtable *ewidths;	// expression width table map for a - b

  ActInstTable I;		/* instance map */

  unsigned int root_is_ns:1;	/* root is the global namespace? */
  Process *simroot;		/* set if root is not the global ns */
  Scope *root_scope;		/* root scope */
  act_languages *root_lang;	/* languages in the root scope */

  ActSimState *state;		/* the state vector */
  ActStatePass *sp;		/* the information about states */
  ActBooleanizePass *bp;	/* Booleanize pass */
  
  Process *_curproc;		/* current process, if any */
  ActId *_curinst;		/* current instance path, if any */
  ActId *_cursuffix;
  state_counts _curoffset;	/* offset of parent process */
  stateinfo_t *_cursi;		/* current state info */
  ActInstTable *_curI;		/* current inst table */

  list_t *_si_stack;		// stack of state info
  list_t *_obj_stack;		// object stack

  int *_cur_abs_port_bool;	/* index of ports: absolute scale */
  int *_cur_abs_port_chan;
  int *_cur_abs_port_int;

  list_t *_chp_sim_objects;	/* used for reset sync wakeup */
  
  
  stateinfo_t *_rootsi;		/* root stateinfo; needed for globals
				   */

  int _is_internal_parallel;    /* used to let the graph construction
				   phase know that there is internal
				   parallelism */

  struct iHashtable *_W;		/* watchpoints */
  struct iHashtable *_B;		/* breakpoints */

  act_extern_trace_func_t *_trfn[TRACE_NUM_FORMATS];
  act_trace_t *_tr[TRACE_NUM_FORMATS];
  static char *_trname[TRACE_NUM_FORMATS];
  float _int_to_float_timescale; // units to convert integer units
				 // to time
  /*-- timing forks --*/
  
  
  /*-- add parts to the simulator --*/
  void _add_language (int lev, act_languages *l);
  void _add_all_inst (Scope *sc);

  ChpSimGraph *_build_chp_graph (act_chp_lang_t *c, ChpSimGraph **stop);

  /*- add specific language -*/
  ChpSim *_add_chp (act_chp *c);
  ChpSim *_add_hse (act_chp *c);
  ActSimObj *_add_dflow (act_dataflow *c);
  PrsSim *_add_prs (act_prs *c);
  XyceSim *_add_xyce ();
  void _add_spec (ActSimObj *, act_spec *);
  void _check_add_spec (const char *name, InstType *it, ActSimObj *obj);
  void _check_fragmentation (ChpSim *);
  void _check_fragmentation (PrsSim *);
  void _check_fragmentation (XyceSim *);

  void _add_timing_fork (ActSimObj *obj, stateinfo_t *si, 
			 int root, int a, int b, Expr *, int *extra);
  void _add_excl (int type, int *ids, int sz);
  void _add_rand_init (int *ids, int sz);
  void _add_hazard (int *ids, int sz);

  void _register_prssim_with_excl (ActInstTable *);

  /*-- returns the current level selected --*/
  int _getlevel ();

  void _initSim ();	      /* create simulation */

  int _have_filter;
  regex_t match;

  unsigned int _prs_sim_mode:1;	 /* 0 = normal, 1 = reset */
  unsigned int _sim_rand:2;	 /* 0 = normal, 1 = random, 2 = rand
				    range */
  
  unsigned int _sim_rand_excl:1; /* 0 = normal, 1 = random excl */

  unsigned int _on_warning:2;	/* 0 = nothing, 1 = break, 2 = exit */

  unsigned int _inf_loop_opt:1;	/* turn on infinite loop optimization */

  unsigned int _rand_min, _rand_max;
  
  unsigned _seed;		 /* random seed, if used */

  int _black_box_mode;

  A_DECL (int, _rand_init);
};


class ActSim : public ActSimCore {
public:
  ActSim (Process *root = NULL); // root of the simulation
  ~ActSim ();

  void setBp (act_connection *c); // breakpoint if this changes
  void clrBp (act_connection *c); // clear breakpoint

  act_connection *runSim (act_connection **cause);
  act_connection *Step (long nsteps);
  act_connection *Advance (long delay);

  void runInit (void);		// run initialization block. This runs
				// all the actions upto the final
				// one. The final block is simply
				// scheduled.

   

  void saveSim (FILE *);
  void restoreSim (FILE *);

  ActInstTable *getInstTable () { return &I; }

  
private:
  list_t *_init_simobjs;
};

void sim_recordChannel (ActSimCore *sc, ActSimObj *c, ActId *id);
void actsim_close_log (void);
void actsim_set_log (FILE *fp);
void actsim_log (const char *s, ...);
void actsim_log_flush (void);
FILE *actsim_log_fp (void);

extern int debug_metrics;

Act *actsim_Act();
Process *actsim_top();
int is_rand_excl ();
bool _match_hseprs (Event *);
void runPending (bool verbose);

#endif /* __ACT_SIM_H__ */
