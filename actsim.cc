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
  if (config_exists ("sim.device.timescale")) {
    _int_to_float_timescale = config_get_real ("sim.device.timescale");
  }
  else {
    /* 10ps default */
    _int_to_float_timescale = 10e-12;
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

/*--- Excl Constraints -- */

struct iHashtable *ActExclConstraint::eHashHi = NULL;
struct iHashtable *ActExclConstraint::eHashLo = NULL;
ActSimCore *ActExclConstraint::_sc = NULL;

void ActExclConstraint::Init ()
{
  if (eHashHi == NULL) {
    eHashHi = ihash_new (4);
    eHashLo = ihash_new (4);
  }
}

/* dir = 1 : up going */
ActExclConstraint::ActExclConstraint (int *nodes, int _sz, int dir)
{
  ihash_bucket_t *b;

  if (_sz == 0) return;

  Init ();

  sz = _sz;
  MALLOC (n, int, sz);

  /* uniquify list */
  for (int i=0; i < sz; i++) {
    int j;
    for (j=0; j < i; j++) {
      if (nodes[j] == nodes[i]) {
	break;
      }
    }
    if (j != i) {
      warning ("Exclusive list has duplicate node; skipped");
      sz = 0;
      FREE (n);
      return;
    }
    n[i] = nodes[i];
  }

  struct iHashtable *H;
  if (dir) {
    H = eHashHi;
  }
  else {
    H = eHashLo;
  }

  MALLOC (nxt, ActExclConstraint *, sz);
  MALLOC (objs, OnePrsSim *, sz);

  /* add to lists */
  for (int i=0; i < sz; i++) {
    objs[i] = NULL;
    b = ihash_lookup (H, n[i]);
    if (!b) {
      b = ihash_add (H, n[i]);
      b->v = NULL;
    }
    
    nxt[i] = (ActExclConstraint *)b->v;
    b->v = this;
  }
}


ActExclConstraint *ActExclConstraint::findHi (int n)
{
  if (!eHashHi) return NULL;
  ihash_bucket_t *b = ihash_lookup (eHashHi, n);
  if (!b) {
    return NULL;
  }
  else {
    return (ActExclConstraint *)b->v;
  }
}

ActExclConstraint *ActExclConstraint::findLo (int n)
{
  if (!eHashLo) return NULL;
  ihash_bucket_t *b = ihash_lookup (eHashLo, n);
  if (!b) {
    return NULL;
  }
  else {
    return (ActExclConstraint *)b->v;
  }
}

int ActExclConstraint::safeChange (ActSimState *st, int n, int v)
{
  ActExclConstraint *tmp, *next;

  if (v == 1) {
    tmp = findHi (n);
  }
  else if (v == 0) {
    tmp = findLo (n);
  }
  else {
    return 1;
  }

  if (!tmp) return 1;
  while (tmp) {
    next = NULL;
    for (int i=0; i < tmp->sz; i++) {
      if (n == tmp->n[i]) {
	next = tmp->nxt[i];
      }
      else {
	if (st->getBool (tmp->n[i]) != (1-v)) {
	  return 0;
	}
      }
    }
    tmp = next;
  }

  /* now kill any pending changes */
  if (v == 1) {
    tmp = findHi (n);
  }
  else if (v == 0) {
    tmp = findLo (n);
  }

  int first = 0;

  if (is_rand_excl()) {
    first = 1;
  }
  
  while (tmp) {
    next = NULL;

    if (first) {
      int count = 1;
      for (int i=0; i < tmp->sz; i++) {
	if (tmp->objs[i]) {
	  if (tmp->objs[i]->isPending()) {
	    count++;
	  }
	}
      }
      Assert (count > 0, "What?!");
      if (_sc->getRandom (count) != 0) {
	return 0;
      }
    }
    first = 0;
    
    for (int i=0; i < tmp->sz; i++) {
      if (n == tmp->n[i]) {
	next = tmp->nxt[i];
      }
      else {
	/* kill any pending change here */
	if (tmp->objs[i]) {
	  tmp->objs[i]->flushPending ();
	}
      }
    }
    tmp = next;
  }

  return 1;
}



/*--- Timing Constraints -- */

struct iHashtable *ActTimingConstraint::THash  = NULL;

void ActTimingConstraint::Init ()
{
  if (THash == NULL) {
    THash = ihash_new (8);
  }
}

#define TIMING_TRIGGER(x)  (((v) == 1 && f[x].up) || ((v) == 0 && f[x].dn))

extern ActSim *glob_sim;

void ActTimingConstraint::update (int sig, int v)
{
  if (glob_sim->isResetMode()) {
    return;
  }
  
  if (n[0] == sig) {
    if (TIMING_TRIGGER (0)) {
      state = ACT_TIMING_START;
    }
    else {
      state = ACT_TIMING_INACTIVE;
    }
  }
  if (n[1] == sig) {
    if (state != ACT_TIMING_INACTIVE && TIMING_TRIGGER (1)) {
      if (state == ACT_TIMING_PENDING) {
	printf ("WARNING: timing constraint in [ ");
	if (obj) {
	  if (obj->getName()) {
	    obj->getName()->Print (stdout);
	  }
	  else {
	    printf ("-top-");
	  }
	  printf (":%s", obj->getProc()->getName() ?
		  obj->getProc()->getName() : "-none-");
	}
	printf (" ] ");
	Print (stdout);
	printf (" violated!\n");
	printf (">> time: %lu\n", ActSimDES::CurTimeLo());
	state = ACT_TIMING_INACTIVE;
      }
      else if (state == ACT_TIMING_START) {
	if (margin != 0) {
	  ts = ActSimDES::CurTimeLo();
	  state = ACT_TIMING_PENDINGDELAY;
	}
	else {
	  /* if unstable, set state to inactive */
	}
      }
      else if (state == ACT_TIMING_PENDINGDELAY) {
	/* update time */
	ts = ActSimDES::CurTimeLo();
      }
    }
  }
  if (n[2] == sig) {
    if (state != ACT_TIMING_INACTIVE && TIMING_TRIGGER (2)) {
      if (state == ACT_TIMING_PENDINGDELAY) {
	if (ts + margin > ActSimDES::CurTimeLo()) {
	  printf ("WARNING: timing constraint ");
	  Print (stdout);
	  printf (" violated!\n");
	  printf (">> time: %lu\n", ActSimDES::CurTimeLo());
	  state = ACT_TIMING_INACTIVE;
	}
      }
      else {
	state = ACT_TIMING_PENDING;
      }
    }
  }
}


ActTimingConstraint *ActTimingConstraint::getNext (int sig)
{
  if (sig == n[0]) {
    return nxt[0];
  }
  else if (sig == n[1]) {
    return nxt[1];
  }
  else if (sig == n[2]) {
    return nxt[2];
  }
  else {
    return NULL;
  }
}


int ActTimingConstraint::isEqual (ActTimingConstraint *t)
{
  if (t->n[0] == n[0] &&  t->n[1] == n[1] && t->n[2] == n[2] &&
      margin == t->margin) {
    return 1;
  }
  return 0;
}

#define TIMING_CHAR(up,down)  ((up) ? ((down)  ? ' ' : '+') : ((down) ? '-' : '?'))

void ActTimingConstraint::Print (FILE *fp)
{
  if (c[0] && c[1] && c[2]) {
    ActId *id[3];
    for (int i=0; i < 3; i++) {
      id[i] = c[i]->toid();
    }
    fprintf (fp, " ");
    id[0]->Print (fp);
    fprintf (fp, "%c : ", TIMING_CHAR(f[0].up, f[0].dn));
    id[1]->Print (fp);
    fprintf (fp, "%c < [%d] ", TIMING_CHAR(f[1].up, f[1].dn), margin);
    id[2]->Print (fp);
    fprintf (fp, "%c", TIMING_CHAR(f[2].up, f[2].dn));
    for (int i=0; i < 3; i++) {
      delete id[i];
    }
  }
  else {
    fprintf (fp, " #%d%c : #%d%c < [%d] #%d%c ",
	     n[0], TIMING_CHAR(f[0].up, f[0].dn),
	     n[1], TIMING_CHAR(f[1].up, f[1].dn), margin,
	     n[2], TIMING_CHAR(f[2].up, f[2].dn));
  }
}

ActTimingConstraint::ActTimingConstraint (ActSimObj *_obj,
					  int root, int a, int b,
					  int _margin,
					  int *extra)
{
  Init ();

  obj = _obj;
  n[0] = root;
  n[1] = a;
  n[2] = b;
  margin = _margin;
  
  for (int i=0; i < 3; i++) {
    nxt[i] = NULL;
    c[i] = NULL;
    
    switch (extra[i] & 0x3) {
    case 0:
      f[i].up = 1;
      f[i].dn = 1;
      break;
      
    case 1:
      f[i].up = 1;
      f[i].dn = 0;
      break;
      
    case 2:
      f[i].up = 0;
      f[i].dn = 1;
      break;
      
    default:
      fatal_error ("What?!");
    }
  }

  
  state = ACT_TIMING_INACTIVE;

  /* -- add links -- */
  for (int i=0; i < 3; i++) {
    ihash_bucket_t *b;

    if ((i < 1 || n[i] != n[i-1]) && (i < 2 || n[i] != n[i-2])) {
      b = ihash_lookup (THash, n[i]);
      if (!b) {
	b = ihash_add (THash, n[i]);
	b->v = NULL;
      }
      else {
	if (i == 0) {
	  /* check for duplicates! */
	  ActTimingConstraint *tmp = (ActTimingConstraint *)b->v;
	  while (tmp) {
	    if (tmp->isEqual (this)) {
	      n[0] = -1;
	      return;
	    }
	    tmp = tmp->getNext (root);
	  }
	}
      }
      nxt[i] = (ActTimingConstraint *)b->v;
      b->v = this;
    }
  }    
}

ActTimingConstraint *ActTimingConstraint::findBool (int v)
{
  ihash_bucket_t *b;
  Init ();
  b = ihash_lookup (THash, v);
  if (!b) {
    return NULL;
  }
  else {
    return (ActTimingConstraint *) b->v;
  }
}


ActTimingConstraint::~ActTimingConstraint ()
{
  
}

void ActExclConstraint::addObject (int id, OnePrsSim *object)
{
  for (int i=0; i < sz; i++) {
    if (n[i] == id) {
      if (objs[i]) {
	warning ("Exclusive constraint is in a multi-driver context. This may not work properly.");
      }
      objs[i] = object;
      return;
    }
  }
}

ActExclConstraint *ActExclConstraint::getNext (int nid)
{
  for (int i=0; i < sz; i++) {
    if (n[i] == nid) {
      return nxt[i];
    }
  }
  return NULL;
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

