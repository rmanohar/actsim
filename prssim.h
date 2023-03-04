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
    
  bool setBool (int lid, int v);
  void printName (FILE *fp, int lid);

  void dumpState (FILE *fp);

  inline int getDelay (int delay) { return _sc->getDelay (delay); }
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
  int Step (Event *ev);
  void propagate ();
  void printName ();
  int matches (int val);
  void registerExcl ();
  void flushPending ();
  int isPending() { return _pending == NULL ? 0 : 1; }
};


#endif /* __ACT_CHP_SIM_H__ */
