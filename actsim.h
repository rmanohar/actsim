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
#include <string.h>
#include <act/act.h>
#include <act/passes.h>
#include <regex.h>
#include <stdlib.h>
#include <math.h>
#include <common/int.h>
#include "actsim_ext.h"

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

struct act_channel_state {
  unsigned int send_here:7;	// if non-zero, this is the "pc" for
				// the sender to be used to wake-up
				// the sending process

  unsigned int sender_probe:1;	// 1 if the send_here wait is actually
				// due to a probe, and not a waiting
				// sender but a waiting probe
  
  unsigned int recv_here:7;	 // if non-zero, this is the "pc" for
				 // the receiver to be used to wake up
				 // the receiving process
  
  unsigned int receiver_probe:1; // receiver is probing and waiting as
				 // a result, but not blocked on a
				 // receive

  unsigned int fragmented:1;	// have to simulate this at a lower
				// level of abstraction since pieces
				// of the channel are accessed

  unsigned int frag_st:2;	// send/recv, send_up/recv_up, or
				// send_rest/recv_rest
  unsigned int ufrag_st:8;	// micro-state within frag state

  struct iHashtable *fH;	// fragment hash table
  Channel *ct;			// channel type
  ActId *inst_id;		// instance

  
  int len;
  expr_multires data, data2;  	// data: used when the receiver is
				// waiting for sender; data2 used when
				// sender arrives before receive is posted.
  WaitForOne *w;
  WaitForOne *probe;		// probe wake-up
};

struct extra_state_alloc {
  void *space;
  int sz;
};

#define MAX_LOCAL_PCS (SIM_EV_MAX+1)


class ActSimState {
public:
  ActSimState (int bools, int ints, int chans);
  ~ActSimState ();

  BigInt *getInt (int x);
  void setInt (int x, BigInt &v);
  int getBool (int x);
  void setBool (int x, int v);
  act_channel_state *getChan (int x);

  void gStall (SimDES *s) { gshared->AddObject (s); }
  void gRemove (SimDES *s) { gshared->DelObject (s); }
  void gWakeup () { gshared->Notify (MAX_LOCAL_PCS); }

  void *allocState (int sz);
  
private:
  bitset_t *bits;		/* Booleans */
  int nbools;			/* # of Booleans */
  
  BigInt *ival;			/* integers */
  int nints;			/* number of integers */


  act_channel_state *chans;	/* channel state */
  int nchans;			/* numchannels */

  list_t *extra_state;		/* any extra state needed */

  /*--- what about other bits of state?! ---*/
  WaitForOne *gshared;
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

  void setName (ActId *id) { if (id) { name = id->Clone(); } else { name = NULL; } }
  ActId *getName () { return name; }
  Process *getProc () { return _proc; }

  virtual void dumpState (FILE *fp) { };
  virtual unsigned long getEnergy () { return 0; }
  virtual double getLeakage () { return 0.0; }
  virtual unsigned long getArea () { return 0; }
  virtual void printStatus (int val) { }

  virtual void propagate ();
  virtual void computeFanout() { printf ("should not be here\n"); }

  void addWatchPoint (int type, int idx, const char *name);
  void delWatchPoint (int type, int idx);
  void toggleBreakPt (int type, int idx, const char *name);
  inline const char *isWatched (int type, int idx);
  inline const char *isBreakPt (int type, int idx);
  void msgPrefix (FILE *fp = NULL);

protected:
  state_counts _o;		/* my state offsets for all local
				   state */
  ActSimCore *_sc;

  ActId *name;
  Process *_proc;

  struct iHashtable *_W;		/* watchpoints */
  struct iHashtable *_B;		/* breakpoints */
  
  int *_abs_port_bool;		/* index of ports: absolute scale */
  int *_abs_port_chan;		/* these arrays are reversed! */
  int *_abs_port_int;
};

class ChpSimGraph;
class ChpSim;
class PrsSim;

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
  void setBool (int x, int v) { state->setBool (x, v); }
  act_channel_state *getChan (int x) { return state->getChan (x); }

  Scope *CurScope() { return _curproc ? _curproc->CurScope() : root_scope; }
  stateinfo_t *cursi() { return _cursi; }

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
  
  void gStall (SimDES *s) { state->gStall (s); }
  void gRemove (SimDES *s) { state->gRemove (s); }
  void gWakeup () { state->gWakeup(); }
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
  int isResetMode() { return _prs_sim_mode; }
  void setWarning (int v) { _on_warning = v; }
  inline int onWarning() { return _on_warning; }


#define LN_MAX_VAL 11.0903548889591  /* log(1 << 16) */

  inline int getDelay (int delay) {
    double d;
    unsigned long val;

    if (_sim_rand == 0) {
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
    if (val == 0) { val = 1; }
    return val;
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
				   chpsimgraph */
  struct iHashtable *pmap;	/* map from process pointer to
				   prssimgraph */

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
  state_counts _curoffset;	/* offset of parent process */
  stateinfo_t *_cursi;		/* current state info */
  ActInstTable *_curI;		/* current inst table */

  int *_cur_abs_port_bool;	/* index of ports: absolute scale */
  int *_cur_abs_port_chan;
  int *_cur_abs_port_int;
  
  
  stateinfo_t *_rootsi;		/* root stateinfo; needed for globals */
  
  /*-- add parts to the simulator --*/
  void _add_language (int lev, act_languages *l);
  void _add_all_inst (Scope *sc);

  ChpSimGraph *_build_chp_graph (act_chp_lang_t *c, ChpSimGraph **stop);

  /*- add specific language -*/
  ChpSim *_add_chp (act_chp *c);
  ChpSim *_add_hse (act_chp *c);
  ActSimObj *_add_dflow (act_dataflow *c);
  PrsSim *_add_prs (act_prs *c, act_spec *);
  void _check_fragmentation (ChpSim *);
  void _check_fragmentation (PrsSim *);

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

  unsigned int _rand_min, _rand_max;
  
  unsigned _seed;		 /* random seed, if used */
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

inline const char *ActSimObj::isWatched (int type, int offset)
{
  if (!_W) return 0;

  ihash_bucket_t *b;
  
  if (type == 3) {
    type = 2;
  }
  b = ihash_lookup (_W, (unsigned long)type | ((unsigned long)offset << 2));
  if (b) {
    return (const char *)b->v;
  }
  else {
    return NULL;
  }
}

inline const char *ActSimObj::isBreakPt (int type, int offset)
{
  if (!_B) return 0;

  ihash_bucket_t *b;
  
  if (type == 3) {
    type = 2;
  }
  b = ihash_lookup (_B, (unsigned long)type | ((unsigned long)offset << 2));
  if (b) {
    return (const char *)b->v;
  }
  else {
    return NULL;
  }
}


#endif /* __ACT_SIM_H__ */
