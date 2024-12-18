/*************************************************************************
 *
 *  Copyright (c) 2024 Rajit Manohar
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
#include "prssim.h"


/*************************************************************************
 *
 * Support function for constraints
 *
 *
 * Boolean variables that hvae constraints are marked as special, and
 * the constraint checks are invoked in the setBool() function that is
 * used across all simulation levels.
 *
 *
 *************************************************************************
 */


/*------------------------------------------------------------------------
 *
 *  mk_exclhi/mk_excllo constraints
 *
 * These are used to model arbiters.
 *
 *------------------------------------------------------------------------
 */

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


/*------------------------------------------------------------------------
 *
 *  exclhi/excllo constraints
 *
 *  These are signal monitors for expectations from the circuit.
 *
 *------------------------------------------------------------------------
 */

struct iHashtable *ActExclMonitor::eHashHi = NULL;
struct iHashtable *ActExclMonitor::eHashLo = NULL;
bool ActExclMonitor::enable = false;

void ActExclMonitor::Init ()
{
  if (eHashHi == NULL) {
    eHashHi = ihash_new (4);
    eHashLo = ihash_new (4);
  }
}  

/* dir = 1 : up going */
ActExclMonitor::ActExclMonitor (ActSimObj *_obj, int *nodes, int _sz, int dir)
{
  ihash_bucket_t *b;

  if (_sz == 0) return;

  Init ();

  sz = _sz;
  MALLOC (n, int, sz);
  MALLOC (c, act_connection *, sz);
  obj = _obj;

  /* uniquify list */
  for (int i=0; i < sz; i++) {
    int j;
    c[i] = NULL;
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

  MALLOC (nxt, ActExclMonitor *, sz);

  /* add to lists */
  for (int i=0; i < sz; i++) {
    b = ihash_lookup (H, n[i]);
    if (!b) {
      b = ihash_add (H, n[i]);
      b->v = NULL;
    }
    nxt[i] = (ActExclMonitor *)b->v;
    b->v = this;
  }
}

ActExclMonitor *ActExclMonitor::findHi (int n)
{
  if (!eHashHi) return NULL;
  ihash_bucket_t *b = ihash_lookup (eHashHi, n);
  if (!b) {
    return NULL;
  }
  else {
    return (ActExclMonitor *)b->v;
  }
}

ActExclMonitor *ActExclMonitor::findLo (int n)
{
  if (!eHashLo) return NULL;
  ihash_bucket_t *b = ihash_lookup (eHashLo, n);
  if (!b) {
    return NULL;
  }
  else {
    return (ActExclMonitor *)b->v;
  }
}

int ActExclMonitor::safeChange (ActSimState *st, int n, int v)
{
  ActExclMonitor *tmp, *next;

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
	  printf ("WARNING: excl-%s constraint in [ ", v ? "hi" : "lo");
	  if (tmp->obj) {
	    if (tmp->obj->getName()) {
	      tmp->obj->getName()->Print (stdout);
	    }
	    else {
	      printf ("-top-");
	    }
	    printf (":%s", tmp->obj->getProc()->getName() ?
		    tmp->obj->getProc()->getName() : "-none-");
	  }
	  printf (" ] ");
	  for (int j=0; j < tmp->sz; j++) {
	    if (j != 0)  {
	      printf (",");
	    }
	    if (tmp->c[j]) {
	      tmp->c[j]->Print (stdout);
	    }
	  }
	  printf (" violated!\n");
	  printf (">> time: %lu\n", ActSimDES::CurTimeLo());
	  return 0;
	}
      }
    }
    tmp = next;
  }
  return 1;
}



/*------------------------------------------------------------------------
 *
 *  Timing constraints
 *
 * These are timing forks in the design.
 *
 *------------------------------------------------------------------------
 */


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
