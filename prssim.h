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

#include <common/simdes.h>
#include "actsim.h"


/**
 * Used to represent upgoing and downgoing delays. We need one bit of info
 * to know if this is a single value or a value table; this information is
 * not in this class, otherwise padding requirements would increase the
 * storage needs. The bit is passed in to all methods.
 *
 * Delay table representation:
 *   [0] = # of slots
 *   [2*i+1] and [2*i+2] is slot i, corresponding to an (idx,val) pair
 *
 * If the msb of idx is 1, that is an unused slot.
 *
 *
 */
class gate_delay_info_onedir {
private:
  union {
    int val;			// up delay is either a value (std)
    int *val_tab;		// ... or a value table (not std)
  };
public:
  // default is to assume it is a fixed delay
  gate_delay_info_onedir() { val = -1; }

  void clear() { val = -1; }

  void delete_table() { FREE (val_tab); }

#define VAL_UNUSED (int)((1UL << (8*sizeof(int)-1)))
  
  int lookup (int idx, bool fixed_delay = true) {
    if (fixed_delay) {
      return val;
    }
    int maxval = -1;
    for (int i=0; i < val_tab[0]; i++) {
      if (val_tab[2*i+1] == VAL_UNUSED) break;
      if (val_tab[2*i+1] == idx) {
	return val_tab[2*i+2];
      }
      if (maxval < val_tab[2*i+2]) {
	maxval = val_tab[2*i+2];
      }
    }
    // not found, return max value (conservative); returns -1 if there
    // are no values in the table, i.e. the default delay.
    return maxval;
  }


  void add (int idx, int dval, bool fixed_delay = true) {
    int j = -1;
    if (fixed_delay) {
      val = dval;
      return;
    }
    for (int i=0; i < val_tab[0]; i++) {
      if (val_tab[2*i+1] == idx) {
	if (val_tab[2*i+2] < dval) {
	  // this maxes out the sdf_conds for the moment
	  val_tab[2*i+2] = dval;
	}
	return;
      }
      if (val_tab[2*i+1] == VAL_UNUSED) {
	j = i;
	break;
      }
    }
    if (j == -1) {
      j = val_tab[0] + 2;
      REALLOC (val_tab, int, j*2+1);
      val_tab[0] = j;
      j -= 2;
      val_tab[2*j+1] = idx;
      val_tab[2*j+2] = dval;
      val_tab[2*j+3] = VAL_UNUSED;
    }
    else {
      val_tab[2*j+1] = idx;
      val_tab[2*j+2] = dval;
    }
  }

  /*
   * Increment delay!
   */
  void inc (int idx, int dval, bool fixed_delay = true) {
    int j = -1;
    if (fixed_delay) {
      val = dval;
      return;
    }
    for (int i=0; i < val_tab[0]; i++) {
      if (val_tab[2*i+1] == idx) {
	val_tab[2*i+2] += dval;
	return;
      }
      if (val_tab[2*i+1] == VAL_UNUSED) {
	j = i;
	break;
      }
    }
    if (j == -1) {
      j = val_tab[0] + 2;
      REALLOC (val_tab, int, j*2+1);
      val_tab[0] = j;
      j -= 2;
      val_tab[2*j+1] = idx;
      val_tab[2*j+2] = dval;
      val_tab[2*j+3] = VAL_UNUSED;
    }
    else {
      val_tab[2*j+1] = idx;
      val_tab[2*j+2] = dval;
    }
  }

  void dump_table(FILE *fp) {
    if (val_tab[0] != 0) {
      fprintf (fp, "[");
      for (int i=0; i < val_tab[0]; i++) {
	if (val_tab[2*i+1] == VAL_UNUSED) break;
	if (i != 0) {
	  fprintf (fp, " ");
	}
	fprintf (fp, "%d:%d", val_tab[2*i+1], val_tab[2*i+2]);
      }
      fprintf (fp, "]");
    }
    else {
      fprintf (fp, "*");
    }
  }

  void mkTables() {
    MALLOC (val_tab, int, 3);
    val_tab[0] = 1;
    val_tab[1] = VAL_UNUSED; // unused marker
  }
#undef VAL_UNUSED

};

struct gate_delay_info {
  gate_delay_info_onedir up, dn;

  gate_delay_info() : up(), dn() { }

  void clear() {
    up.clear ();
    dn.clear();
  }

  void mkTables() {
    up.mkTables();
    dn.mkTables();
  }

  void delete_tables() {
    up.delete_table();
    dn.delete_table();
  }

  void dump_tables(FILE *fp) {
    fprintf (fp, "{up: ");
    up.dump_table (fp);
    fprintf (fp, "; dn: ");
    dn.dump_table (fp);
    fprintf (fp, "}");
  }
};


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
    struct {
      int vid;
      act_connection *c;
    };
  };
};

#define PRSSIM_RULE  0
#define PRSSIM_PASSP 1
#define PRSSIM_PASSN 2
#define PRSSIM_TGATE 3

#define PRSSIM_NORM 0
#define PRSSIM_WEAK 1


struct prssim_stmt {
  unsigned int type:2;		/* RULE, P, N, TRANSGATE */
  unsigned int unstab:1;	/* is unstable? */
  unsigned int std_delay:1;     /* 1 if this uses the standard delay,
				   0 if it uses delay tables */
  struct prssim_stmt *next;

  // default inst-independent delays/delay tables
  gate_delay_info delay;
  
  int delay_override_length; /* The next event has a manually overridden delay */

  union {
    struct {
      prssim_expr *up[2], *dn[2];
      int vid;
      act_connection *c;
    };
    struct {
      int t1, t2, g, _g;
    };
  };

  int delayUp(int idx) {
    return delay.up.lookup (idx, std_delay ? true : false);
  }

  int delayDn(int idx) {
    return delay.dn.lookup (idx, std_delay ? true : false);
  }

  void setUpDelay (int idx, int val) {
    if (std_delay) return;
    delay.up.add (idx, val, false);
  }
  void setDnDelay (int idx, int val) {
    if (std_delay) return;
    delay.dn.add (idx, val, false);
  }
  void incUpDelay (int idx, int val) {
    if (std_delay) return;
    delay.up.inc (idx, val, false);
  }
  void incDnDelay (int idx, int val) {
    if (std_delay) return;
    delay.dn.inc (idx, val, false);
  }

  void setDelayDefault() {
    std_delay = 1;
    delay.clear ();
  }

  /*
   * delay tables:
   *    [0] = # of slots
   *    [2*i+1] and [2*i+2] is slot i, corresponding to an (idx, val) pair
   */
  void setDelayTables() {
    delay.mkTables ();
  }

  void setUpDelay (int val) {
    if (!std_delay) return;
    delay.up.add (0, val, true);
  }

  void setDnDelay (int val) {
    if (!std_delay) return;
    delay.dn.add (0, val, true);
  }

  bool simpleDelay() {
    return std_delay ? true : false;
  }
};
  

class PrsSimGraph {
private:
  struct prssim_stmt *_rules, *_tail;
  struct Hashtable *_labels;

  void _add_one_rule (ActSimCore *, act_prs_lang_t *, sdf_cell *);
  void _add_one_gate (ActSimCore *, act_prs_lang_t *);
  
public:
  PrsSimGraph();
  ~PrsSimGraph();
  
  void addPrs (ActSimCore *, act_prs_lang_t *, sdf_cell *);

  prssim_stmt *getRules () { return _rules; }
  struct Hashtable *getLabels() { return _labels; }


  static PrsSimGraph *buildPrsSimGraph (ActSimCore *, act_prs *, sdf_cell *ci);
  static void checkFragmentation (ActSimCore *, PrsSim *, act_prs *);
  static void checkFragmentation (ActSimCore *, PrsSim *, act_prs_lang_t *);
  static void checkFragmentation (ActSimCore *, PrsSim *, ActId *, int);
  static void checkFragmentation (ActSimCore *, PrsSim *, act_prs_expr_t *);
  
};

class PrsSim : public ActSimObj {
 public:
  PrsSim (PrsSimGraph *, ActSimCore *sim, Process *p);
     /* initialize simulation, and create initial event */
  ~PrsSim ();

  int Step (Event *ev);		/* run a step of the simulation */

  void computeFanout ();

  void initState ();

  int getBool (int lid, int *gid = NULL) {
    int off = getGlobalOffset (lid, 0);
    if (gid) {
      *gid = off;
    }
    return _sc->getBool (off);
  }

  int isSpecialBool (int lid) {
    int off = getGlobalOffset (lid, 0);
    return _sc->isSpecialBool (off);
  }

  bool isHazard (int lid) {
    int off = getGlobalOffset (lid, 0);
    return _sc->isHazard (off);
  }
  
  int myGid (int lid) { return getGlobalOffset (lid, 0); }

  /**
   * @brief Set the value of the current node to the given value, report the change up the chain for logging, 
   * and propagate the signal down to all nodes this one fans out to.
   * 
   * @param lid ID of the signal we are looking to change.
   * @param v The target value to change it to.
   * @param me Part of the cause tracking. Object that is used as the cause in propagation tracking. Forwarded to next.
   * @param cause Part of the cause tracking. Object which caused this state change.
   * @return true The state of the node was successfully changed in the state vector.
   * @return false Changing the state of the node in the global state vector failed.
   */
  bool setBool (int lid, int v, OnePrsSim *me, ActSimObj *cause = NULL);

  void printName (FILE *fp, int lid);
  void sPrintName (char *buf, int sz, int lid);

  void dumpState (FILE *fp);

  inline int getDelay (int lower_bound, int upper_bound) {
    if (upper_bound == -1) return _sc->getDelay (lower_bound);
    return _sc->getDelay (lower_bound, upper_bound); 
  }
  inline int isResetMode() { return _sc->isResetMode (); }
  inline int onWarning() { return _sc->onWarning(); }

  void printStatus (int val, bool io_glob = false);

  void registerExcl ();

  void updateDelays (act_prs *prs, sdf_celltype *ci);

  inline gate_delay_info *getInstDelay (OnePrsSim *sim);
  
 private:
  void _computeFanout (prssim_expr *, SimDES *);

  void _updatePrs (act_prs_lang_t *p);
  
  PrsSimGraph *_g;

  int _nobjs;			     // # of simulation objects
  OnePrsSim *_sim;		     // simulation objects
  gate_delay_info **_inst_gate_delay; // delay info specific to each instance
};


/*-- not actsimobj so that it can be lightweight --*/
class OnePrsSim : public ActSimDES {
private:
  PrsSim *_proc;		// process core [maps, etc]
  struct prssim_stmt *_me;	// the rule
  Event *_pending;
  int eval (prssim_expr *, int cause_id = -1, int *lid = NULL);

public:
  OnePrsSim (PrsSim *p, struct prssim_stmt *x);


  /**
   * @brief Takes an event and parses it. If the event changes the value of the node, do so by calling setBool
   * 
   * The method can take separate kinds of events, the lowest two bits set values while upper bits provide more information.
   * If t & 0b11100 is a value other than 0, a special event is parsed. This is either the begin or of a single event upset
   * (0b100xx and 0b10100 respectively, where xx indicates the value the node should be forced to), or a single event delay,
   * (0bxx11100 where the bits above the event type encode the length of the delay).
   * 
   * @param ev The event to parse
   * @return int 
   */
  int Step (Event *ev);


  /**
   * @brief Propagate an incoming logic change event to all nodes connected to this one.
   * For this, all incoming rules for these nodes are evaluated and, if a logic level change
   * is required, an event is scheduled for the change to happen after the node's delay.
   * 
   */
  void propagate (void *cause);

  void printName ();
  inline int getMyLocalID () { return _me->vid; };
  int matches (int val);
  void registerExcl ();
  void flushPending ();
  int isPending() { return _pending == NULL ? 0 : 1; };

  /**
  * @brief Create and register SEU start and end events and put them into the event queue
  * 
  * @param start_delay The delay from the start of the simulation to the start of the SEU
  * @param upset_duration The duration of the SEU
  * @param force_value The value the SEU forces the node to
  * @return true Always returned at the moment
  * @return false Not used currently
  */
  bool registerSEU(int start_delay, int upset_duration, int force_value);
  
  
  /**
   * @brief Create and register an SED event and put it into the event queue
   * 
   * @param start_delay The delay from the start of the simulation to the start of the SED
   * @param delay_duration The duration of the artificial delay
   * @return true Always returned at the moment
   * @return false Not used currently
   */
  bool registerSED(int start_delay, int delay_duration);

  void sPrintCause (char *buf, int sz);
  int causeGlobalIdx ();

  friend class MultiPrsSim;
};

class MultiPrsSim : public ActSimDES {
private:
  OnePrsSim **_objs;
  int _nobjs, _count;
public:
  MultiPrsSim (int nobjs) {
    Assert (nobjs > 1, "What?");
    _nobjs = nobjs;
    MALLOC (_objs, OnePrsSim *, _nobjs);
    _count = 0;
  }
  
  ~MultiPrsSim () {
    if (_objs) {
      FREE (_objs);
      _objs = NULL;
    }
  }

  void addOnePrsSim (OnePrsSim *x) {
    Assert (_count < _nobjs, "What?");
    _objs[_count++] = x;
  }

  /*-- virtual methods --*/
  int Step (Event *ev);
  void propagate (void *cause);
  void sPrintCause (char *buf, int sz);
  int causeGlobalIdx ();
};

#endif /* __ACT_CHP_SIM_H__ */
