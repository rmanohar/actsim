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

#include <bitset.h>
#include <simdes.h>
#include <string.h>
#include <act/act.h>
#include <act/passes.h>

#define E_CHP_VARBOOL  (E_NEWEND + 1)
#define E_CHP_VARINT   (E_NEWEND + 2)
#define E_CHP_VARCHAN  (E_NEWEND + 3)

#define E_CHP_VARBOOL_DEREF  (E_NEWEND + 4)
#define E_CHP_VARINT_DEREF   (E_NEWEND + 5)
#define E_CHP_VARCHAN_DEREF  (E_NEWEND + 6)

#define E_PROBEIN  (E_NEWEND + 7)
#define E_PROBEOUT  (E_NEWEND + 8)

/*
 *
 * Core simulation library
 *
 */

class ActSimCore;

struct act_channel_state {
  unsigned int send_here:4;
  unsigned int sender_probe:1;	// 1 if the send_here wait is actually
				// due to a probe
  
  unsigned int recv_here:4;
  unsigned int receiver_probe:1; // receiver is probing and waiting as
				 // a result
  
  int len;
  int data, data2;
  WaitForOne *w;
  WaitForOne *probe;		// probe wake-up
};

struct expr_res {
  unsigned int v;		/* value */
  int width;			/* bitwidth */
};

#define MAX_LOCAL_PCS 1024

  
class ActSimState {
public:
  ActSimState (int bools, int ints, int chans);
  ~ActSimState ();

  int getInt (int x);
  void setInt (int x, int v);
  int getBool (int x);
  void setBool (int x, int v);
  act_channel_state *getChan (int x);

  void gStall (SimDES *s) { gshared->AddObject (s); }
  void gRemove (SimDES *s) { gshared->DelObject (s); }
  void gWakeup () { gshared->Notify (MAX_LOCAL_PCS); }
  
private:
  bitset_t *bits;		/* Booleans */
  int nbools;			/* # of Booleans */
  
  int *ival;			/* integers */
  int nints;			/* number of integers */

  act_channel_state *chans;	/* channel state */
  int nchans;			/* numchannels */

  /*--- what about other bits of state?! ---*/
  WaitForOne *gshared;
};


class ActSimObj : public SimDES {
public:
  ActSimObj (ActSimCore *sim);

  
  int getGlobalOffset (int loc, int type); // 0 = bool, 1 = int,
                                           // 2 = chan

  void setOffsets (chp_offsets *x) { _o = *x; }
  void setPorts (int *_bool, int *_int, int *_chan) {
    _abs_port_bool = _bool;
    _abs_port_int = _int;
    _abs_port_chan = _chan;
  }

  void setName (ActId *id) { if (id) { name = id->Clone(); } else { name = NULL; } }

protected:
  chp_offsets _o;		/* my state offsets for all local
				   state */
  ActSimCore *_sc;

  ActId *name;
  
  int *_abs_port_bool;		/* index of ports: absolute scale */
  int *_abs_port_chan;		/* these arrays are reversed! */
  int *_abs_port_int;
};

class ChpSimGraph;

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
  ActSimState *getState ();
  void setState (ActSimState *);

  int getInt (int x) { return state->getInt (x); }
  void setInt (int x, int v) { state->setInt (x, v); }
  int getBool (int x) { return state->getBool (x); }
  void setBool (int x, int v) { state->setBool (x, v); }
  act_channel_state *getChan (int x) { return state->getChan (x); }

  /* local state map; does not include ports; ports are allocated in parent */
  int mapIdToLocalOffset (act_connection *c, stateinfo_t *si);

  Scope *CurScope() { return _curproc ? _curproc->CurScope() : root_scope; }
  stateinfo_t *cursi() { return _cursi; }

  int getLocalOffset (ActId *id, stateinfo_t *si, int *type);
  int getLocalOffset (act_connection *c, stateinfo_t *si, int *type);
  /* encoding: >= 0 = local state. Add to process offset to get global
     offset.

     for negative:
        -2x : global negative "x"; add "x" to total globals to get abs offset
   (-2x + 1): port offset. add "x" to total ports to get port
              offset

      sets type to 0, 1, 2 for bool, int, chan
  */
  void gStall (SimDES *s) { state->gStall (s); }
  void gRemove (SimDES *s) { state->gRemove (s); }
  void gWakeup () { state->gWakeup(); }

protected:
  Act *a;

  struct iHashtable *map;	/* map from process pointer to
				   chpsimgraph */

  unsigned int root_is_ns:1;	/* root is the global namespace? */
  Process *simroot;		/* set if root is not the global ns */
  Scope *root_scope;		/* root scope */
  act_languages *root_lang;	/* languages in the root scope */

  ActSimState *state;		/* the state vector */
  ActStatePass *sp;		/* the information about states */
  ActBooleanizePass *bp;	/* Booleanize pass */
  
  Process *_curproc;		/* current process, if any */
  ActId *_curinst;		/* current instance path, if any */
  chp_offsets _curoffset;	/* offset of parent process */
  stateinfo_t *_cursi;		/* current state info */

  int *_cur_abs_port_bool;	/* index of ports: absolute scale */
  int *_cur_abs_port_chan;
  int *_cur_abs_port_int;
  
  
  stateinfo_t *_rootsi;		/* root stateinfo; needed for globals */
  chp_offsets globals;		/* globals */
  
  /*-- add parts to the simulator --*/
  void _add_language (int lev, act_languages *l);
  void _add_all_inst (Scope *sc);

  ChpSimGraph *_build_chp_graph (act_chp_lang_t *c, ChpSimGraph **stop);

  /*- add specific language -*/
  void _add_chp (act_chp *c);
  void _add_hse (act_chp *c);
  void _add_dflow (act_dataflow *c);
  void _add_prs (act_prs *c);

  /*-- returns the current level selected --*/
  int _getlevel ();

  void _initSim ();	      /* create simulation */
};


class ActSim : public ActSimCore {
public:
  ActSim (Process *root = NULL); // root of the simulation
  ~ActSim ();

  void setBp (act_connection *c); // breakpoint if this changes
  void clrBp (act_connection *c); // clear breakpoint

  /* set and get state */

  void setBool (act_connection *c, bool b, int delay = 0); 
  void setInt (act_connection *c, int ival, int delay = 0);
  void setChan (act_connection *c, act_channel_state *s, int delay = 0);
  
  bool getBool (act_connection *);
  int getInt (act_connection *);
  act_channel_state *getChan (act_connection *);

  int numFanout (act_connection *);
  void getFanout (act_connection *c, act_connection **n);

  int numFanin (act_connection *c);
  void getFanin (act_connection *c, act_connection **n);

  act_connection *runSim (act_connection **cause);

  act_connection *runStep (act_connection **cause);

  void saveSim (FILE *);
  void restoreSim (FILE *);
  
private:

};


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
      int var;
      Expr *e;
    } assign;			/* var := e */
    struct {
      int chvar;
      list_t *el;		/* list of expressions */
    } send;
    struct {
      int chvar;
      list_t *vl;		/* list of vars */
    } recv;
  } u;
};

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
};



#endif /* __ACT_SIM_H__ */
