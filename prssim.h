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
  unsigned int type:2; /* RULE, P, N, TRANSGATE */
  unsigned int unstab:1; /* is unstable? */
  struct prssim_stmt *next;
  int delay_up, delay_dn;
  int delay_up_max, delay_dn_max; /* used when delay for a node is random, then delay_up/down are used for minimum delay */
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
};
  

class PrsSimGraph {
private:
  struct prssim_stmt *_rules, *_tail;
  struct Hashtable *_labels;

  void _add_one_rule (ActSimCore *, act_prs_lang_t *);
  void _add_one_gate (ActSimCore *, act_prs_lang_t *);
  
public:
  PrsSimGraph();
  ~PrsSimGraph();
  
  void addPrs (ActSimCore *, act_prs_lang_t *);

  prssim_stmt *getRules () { return _rules; }


  static PrsSimGraph *buildPrsSimGraph (ActSimCore *, act_prs *);
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

  int getBool (int lid) { int off = getGlobalOffset (lid, 0); return _sc->getBool (off); }
  int isSpecialBool (int lid) { int off = getGlobalOffset (lid, 0); return _sc->isSpecialBool (off); }

  bool isHazard (int lid) { int off = getGlobalOffset (lid, 0); return _sc->isHazard (off); }
  
  int myGid (int lid) { return getGlobalOffset (lid, 0); }


  /**
   * @brief Set the value of the current node to the given value, report the change up the chain for logging, 
   * and propagate the signal down to all nodes this one fans out to.
   * 
   * @param lid ID of the signal we are looking to change.
   * @param v The target value to change it to.
   * @return true The state of the node was successfully changed in the state vector.
   * @return false Changing the state of the node in the global state vector failed.
   */
  bool setBool (int lid, int v);

  /**
   * @brief Set the node to a forced logic value
   * 
   * This will override any rules that might be in place and propagate the new
   * forced value the node was set to. Subsequent calls to setBool will not affect the
   * displayed value of the node.
   * 
   * The value of the node before the force call and all calls made to setBool after are
   * buffered. The latest valid value this node would have had, had the force never occurred,
   * will be restored by calling unmask
   * 
   * @param lid Local ID of the node to force
   * @param v Value to force the node to
   */
  void setForced (int lid, int v);

  /**
   * @brief Test if the current node is masked by a forced value
   * 
   * @param x Local ID of the node to be tested
   * @return true The true value is hidden
   * @return false The value displayed is not externally forced
   */
  bool isMasked (int lid) { int off = getGlobalOffset (lid, 0); return _sc->isMasked (off); }

  /**
   * @brief Restore normal operation of the node after value force
   * 
   * Restore the value of the node to what it should be displaying, had the value force not happened.
   * 
   * @param lid Local ID of the node to force
   * @return true The actual node value was successfully unmasked
   * @return false The actual node value was never masked to begin with
   */
  bool unmask (int lid);

  /**
   * @brief Find the PRS rule for a given local ID
   * 
   * @param vid Local ID of the node
   * @return OnePrsSim* 
   */
  OnePrsSim* findRule (int vid);

  void printName (FILE *fp, int lid);

  void dumpState (FILE *fp);

  inline int getDelay (int lower_bound, int upper_bound) {
    if (upper_bound == -1) return _sc->getDelay (lower_bound);
    return _sc->getDelay (lower_bound, upper_bound); 
  }
  inline int isResetMode() { return _sc->isResetMode (); }
  inline int onWarning() { return _sc->onWarning(); }

  void printStatus (int val, bool io_glob = false);

  void registerExcl ();
  
 private:
  void _computeFanout (prssim_expr *, SimDES *);
  
  void varSet (int id, int type, BigInt &v);
  int varSend (int pc, int wakeup, int id, BigInt &v);
  int varRecv (int pc, int wakeup, int id, BigInt *v);

  PrsSimGraph *_g;
  list_t *_sim;			// simulation objects
};


/*-- not actsimobj so that it can be lightweight --*/
class OnePrsSim : public ActSimDES {
private:
  PrsSim *_proc;		// process core [maps, etc]
  struct prssim_stmt *_me;	// the rule
  Event *_pending;
  int eval (prssim_expr *);

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
  void propagate ();


  /**
   * @brief Set the node to a given value. 
   * 
   * It creates a new event with the delay dependent on the current mode
   * of the node.
   * 
   * There are several possible delay modes a node can have. It can either:
   * Have a set up/down delay.
   * Derive its up/down delay from global delay bounds randomly.
   * Get assigned an unconstrained random delay.
   * Derive its up/down delay from node delay bounds randomly (not implemented yet).
   * 
   * @param nid ID of the node to affect
   * @param val Value to set the node to
   */
  void setVal(int nid, int val);
  
  void printName ();
  inline int getMyLocalID () { return _me->vid; };
  int matches (int val);
  void registerExcl ();
  void flushPending ();
  int isPending() { return _pending == NULL ? 0 : 1; }

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

};


#endif /* __ACT_CHP_SIM_H__ */
