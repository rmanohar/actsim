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
#include "actsim.h"
#include "chpsim.h"
#include "prssim.h"
#include "xycesim.h"
#include <time.h>
#include <math.h>
#include <ctype.h>

/*

  Core ACT simulation library.

  A simulation is initially empty.

  There will be a certain ACT point that is the root of the
  simulation. This root is either the global namespace, or a process.

  By default, the simulation engine will pull in all the pieces of the
  design, and build a discrete-event simulation engine for the entire
  circuit.

  If you want to do something more special, then the simulation engine
  can be initialized with the root, and then pieces can be added to
  the simulation engine.

*/


act_connection *ActSim::runSim (act_connection **cause)
{
  Event *ret;

  if (SimDES::isEmpty()) {
    warning ("Empty simulation!");
    return NULL;
  }

  ret = SimDES::Run ();
  
  return NULL;
}

act_connection *ActSim::Step (long nsteps)
{
  Event *ret;
  if (SimDES::isEmpty()) {
    warning ("Empty simulation!");
    return NULL;
  }

  ret = SimDES::Advance (nsteps);

  return NULL;
}

act_connection *ActSim::Advance (long delay)
{
  Event *ret;
  if (SimDES::isEmpty()) {
    warning ("Empty simulation!");
    return NULL;
  }

  ret = SimDES::AdvanceTime (delay);

  return NULL;
}

ActSim::ActSim (Process *root, SDF *sdf) : ActSimCore (root, sdf)
{
  /* nothing */
  _init_simobjs = NULL;

  if (sdf) {
    sdf->reportUnusedCells ("actsim-sdf", stderr);
    _sdf_report ();
    _sdf_clear_errors ();
  }
}

ActSim::~ActSim()
{
  /* stuff here */
  if (_init_simobjs) {
    listitem_t *li;
    for (li = list_first (_init_simobjs); li; li = list_next (li)) {
      ChpSim *x = (ChpSim *) list_value (li);
      li = list_next (li);
      ChpSimGraph *g = (ChpSimGraph *) list_value (li);
      delete g;
      delete x;
    }
  }
  list_free (_init_simobjs);
}


bool _match_hseprs (Event *e)
{
  if (dynamic_cast <OnePrsSim *> (e->getObj())) {
    return true;
  }
  ChpSim *x = dynamic_cast <ChpSim *> (e->getObj());
  if (x && x->isHseMode()) {
    return true;
  }
  return false;
}

static void _init_prs_objects (ActInstTable *I)
{
  hash_bucket_t *b;
  hash_iter_t hi;
  if (!I) return;

  if (I->obj) {
    PrsSim *x = dynamic_cast<PrsSim *> (I->obj);
    if (x) { x->initState (); }
  }

  if (I->H) {
    hash_iter_init (I->H, &hi);
    while ((b = hash_iter_next (I->H, &hi))) {
      ActInstTable *x = (ActInstTable *)b->v;
      _init_prs_objects (x);
    }
  }
}

void ActSim::runInit ()
{
  ActNamespace *g = ActNamespace::Global();
  act_initialize *x;
  int fragmented_set = 0;
  listitem_t *li;

  setMode (1);

  XyceActInterface::getXyceInterface()->initXyce();

  /*-- random init --*/
  for (int i=0; i < A_LEN (_rand_init); i++) {
    int v = random() % 2;
    if (getBool (_rand_init[i]) == 2) {
      if (setBool (_rand_init[i], v)) {
	SimDES **arr = getFO (_rand_init[i], 0);
	for (int j=0; j < numFanout (_rand_init[i], 0); j++) {
	  ActSimDES *x = dynamic_cast<ActSimDES *>(arr[j]);
	  Assert (x, "What?");
	  x->propagate ();
	}
      }
    }
  }

  /*-- XXX: initially true production rules --*/
  _init_prs_objects (&I);
  
  /*-- reset channels that are fragmented --*/
  for (int i=0; i < state->numChans(); i++) {
    act_channel_state *ch = state->getChan (i);
    if ((ch->fragmented & 0x1) != (ch->fragmented >> 1)) {
      if (ch->fragmented & 0x1) {
	/* input fragmented, so do sender reset protocol */
	if (ch->cm->runMethod (this, ch, ACT_METHOD_SEND_INIT, 0) != -1) {
	  warning ("Failed to initialize fragmented channel!");
	  fprintf (stderr, "   Type: %s; inst: `", ch->ct->getName());
	  ch->inst_id->Print (stderr);
	  fprintf (stderr, "'\n");
	}
	fragmented_set = 1;
      }
      else {
	/* output fragmented, so do receiver fragmented protocol */
	if (ch->cm->runMethod (this, ch, ACT_METHOD_RECV_INIT, 0) != -1) {
	  warning ("Failed to initialize fragmented channel!");
	  fprintf (stderr, "   Type: %s; inst: `", ch->ct->getName());
	  ch->inst_id->Print (stderr);
	  fprintf (stderr, "'\n");
	}
	fragmented_set = 1;
      }
    }
  }

  /* -- initialize blocks -- */
  if (!g->getlang() || !g->getlang()->getinit()) {
    if (fragmented_set) {
      int count = 0;
      while (SimDES::matchPendingEvent (_match_hseprs) && count < 100) {
	count++;
	if (SimDES::AdvanceTime (10) != NULL) {
	  warning ("breakpoint?");
	}
      }
      if (count == 100) {
	warning ("Pending production rule events during reset phase?");
      }
    }
    setMode (0);
    for (li = list_first (_chp_sim_objects); li; li = list_next (li)) {
      ChpSim *cx = (ChpSim *) list_value (li);
      cx->sWakeup ();
    }
    return;
  }
  int num = 1;
  x = g->getlang()->getinit();

  while (x->next) {
    num++;
    x = x->next;
  }

  listitem_t **lia;
  MALLOC (lia, listitem_t *, num);

  x = g->getlang()->getinit();
  for (int i=0; i < num; i++) {
    Assert (x, "What?");
    lia[i] = list_first (x->actions);
    x = x->next;
  }
  Assert (!x, "What?");

  /* -- everything except the last action -- */
  int more_steps = 1;

  if (!_init_simobjs) {
    _init_simobjs = list_new ();
  }

  while (more_steps) {
    more_steps = 0;
    for (int i=0; i < num; i++) {
      if (lia[i] && list_next (lia[i])) {
	act_chp_lang_t *c = (act_chp_lang_t *) list_value (lia[i]);
	chpsimgraph_info *ci = ChpSimGraph::buildChpSimGraph (this, c);
	ChpSim *sim_init =
	  new ChpSim (ci, c, this, NULL);

	list_append (_init_simobjs, sim_init);
	list_append (_init_simobjs, ci->g);

	ci->g = NULL;
	delete ci;
	  
	lia[i] = list_next (lia[i]);
	
	more_steps = 1;

	int count = 0;
	do {
	  if (SimDES::AdvanceTime (100) != NULL) {
	    warning ("breakpoint?");
	  }
	  count++;
	  if (!SimDES::matchPendingEvent (_match_hseprs)) {
	    break;
	  }
	} while (count < 100);
	if (count == 100) {
	  warning ("Pending production rule events during reset phase?");
	}
      }
    }
  }
  for (int i=0; i < num; i++) {
    if (lia[i]) {
      act_chp_lang_t *c = (act_chp_lang_t *) list_value (lia[i]);
      chpsimgraph_info *ci = ChpSimGraph::buildChpSimGraph (this, c);
      
      ChpSim *sim_init =
	new ChpSim (ci, c, this, NULL);

      list_append (_init_simobjs, sim_init);
      list_append (_init_simobjs, ci->g);

      ci->g = NULL;
      delete ci;
	  
      lia[i] = list_next (lia[i]);
    }
  }
  FREE (lia);
  setMode (0);
  for (li = list_first (_chp_sim_objects); li; li = list_next (li)) {
    ChpSim *cx = (ChpSim *) list_value (li);
    cx->sWakeup ();
  }
}



/*-------------------------------------------------------------------------
 * Logging
 *-----------------------------------------------------------------------*/
static FILE *alog_fp = NULL;

FILE *actsim_log_fp (void)
{
  return alog_fp ? alog_fp : stdout;
}

void actsim_log (const char *s, ...)
{
  va_list ap;

  va_start (ap, s);
  vfprintf (actsim_log_fp(), s, ap);
  va_end (ap);
  if (alog_fp) {
    fflush (alog_fp);
  }
}

void actsim_close_log (void)
{
  if (alog_fp) {
    fclose (alog_fp);
  }
  alog_fp = NULL;
}

void actsim_set_log (FILE *fp)
{
  actsim_close_log ();
  alog_fp = fp;
}

void actsim_log_flush (void)
{
  fflush (actsim_log_fp ());
}



/* pending events */

static int _pend_count;
static int _pend_prs;

static bool _match_count (Event *e)
{
  _pend_count++;
  if (dynamic_cast <OnePrsSim *> (e->getObj())) {
    _pend_prs++;
  }
  return false;
}

static bool _match_verbose (Event *e)
{
  OnePrsSim *op = dynamic_cast<OnePrsSim *> (e->getObj());
  if (op) {
    return false;
  }
  ChpSim *ch = dynamic_cast<ChpSim *> (e->getObj());
  if (ch) {
    return false;
  }
  return false;
}

void runPending (bool verbose)
{
  _pend_count = 0;
  _pend_prs = 0;
  SimDES::matchPendingEvent (_match_count);
  printf ("Pending events: %d", _pend_count);
  if (_pend_prs > 0) {
    printf ("; prs events: %d", _pend_prs);
  }
  printf ("\n");
#if 0  
  if (verbose) {
    SimDES::matchPendingEvent (_match_verbose);
  }
#endif  
}


/*------------------------------------------------------------------------
 *
 *  Basic methods for all act simulation objects
 *
 *------------------------------------------------------------------------
 */
ActSimObj::ActSimObj (ActSimCore *sim, Process *p)
{
  _sc = sim;
  _proc = p;
  _abs_port_bool = NULL;
  _abs_port_int = NULL;
  _abs_port_chan = NULL;
  name = NULL;
  _shared = new WaitForOne(0);
}


int ActSimObj::getGlobalOffset (int loc, int type)
{
  int locoff;
  int *portoff;

#if 0
  printf ("[get glob: %d, type=%d] ", loc, type);
#endif  
  
  switch (type) {
  case 0:
    locoff = _o.numAllBools();
    portoff = _abs_port_bool;
    break;
  case 1:
    locoff = _o.numInts();
    portoff = _abs_port_int;
    break;
  case 2:
    locoff = _o.numChans();
    portoff = _abs_port_chan;
    break;
  default:
    fatal_error ("Unknown type: has to be 0, 1, or 2");
    break;
  }

  if (loc >= 0) {
#if 0
    printf (" -> %d var\n", locoff + loc);
#endif
    return locoff + loc;
  }
  else {
    loc = -loc;
    if (loc & 1) {
      /* local port */
      loc = (loc + 1)/2 - 1;
#if 0
      printf (" -> %d localport @ %d\n", portoff[loc], loc);
#endif
      return portoff[loc];
    }
    else {
      /* global */
      loc = loc/2 - 1;
#if 0
      printf (" -> %d global\n", loc);
#endif
      return loc;
    }
  }
}


void ActSimObj::propagate (void *cause)
{
  /* by default, wake me up if stalled on something shared */
  sWakeup ();
}


ActSimObj::~ActSimObj()
{
  if (_abs_port_bool) {
    FREE (_abs_port_bool);
  }
  if (_abs_port_int) {
    FREE (_abs_port_int);
  }
  if (_abs_port_chan) {
    FREE (_abs_port_chan);
  }
  delete name;
  delete _shared;
}


void ActSimObj::addWatchPoint (int type, int offset, const char *name)
{
  if (type == 3) {
    type = 2;
  }

  int glob = getGlobalOffset (offset, type);
  _sc->addWatchPt (type, glob, name);
}

void ActSimObj::toggleBreakPt (int type, int offset, const char *name)
{
  if (type == 3) {
    type = 2;
  }

  int glob = getGlobalOffset (offset, type);
  _sc->toggleBreakPt (type, glob, name);
}


void ActSimObj::delWatchPoint (int type, int offset)
{
  if (type == 3) {
    type = 2;
  }

  int glob = getGlobalOffset (offset, type);
  _sc->delWatchPt (type, glob);
}

void ActSimObj::msgPrefix (FILE *fp)
{
  if (fp == NULL) {
    fp = stdout;
  }
  BigInt tm = CurTime();
  fprintf (fp, "[");
  tm.decPrint (fp, 20);
  fprintf (fp, "] <");
  if (name) {
    name->Print (fp);
  }
  fprintf (fp, ">  ");
}

