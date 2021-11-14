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
#include <common/simdes.h>
#include "prssim.h"
#include <common/qops.h>

PrsSim::PrsSim (PrsSimGraph *g, ActSimCore *sim, Process *p)
: ActSimObj (sim, p)
{
  _sc = sim;
  _g = g;
  _sim = list_new ();
}

PrsSim::~PrsSim()
{
  listitem_t *li;
  for (li = list_first (_sim); li; li = list_next (li)) {
    OnePrsSim *x = (OnePrsSim *) list_value (li);
    delete x;
  }
  list_free (_sim);
}

int PrsSim::Step (int ev_type)
{
  fatal_error ("This should never be called!");
  return 1;
}

void PrsSim::printStatus (int val)
{
  listitem_t *li;
  int emit_name = 0;
  
  for (li = list_first (_sim); li; li = list_next (li)) {
    if (((OnePrsSim *)list_value (li))->matches (val)) {
      if (!emit_name) {
	name->Print (stdout);
	printf (" { ");
	emit_name = 1;
      }
      else {
	printf (" ");
      }
      ((OnePrsSim *)list_value (li))->printName ();
    }
  }
  if (emit_name) {
    printf (" }\n");
  }
}


void PrsSim::_computeFanout (prssim_expr *e, SimDES *s)
{
  if (!e) return;
  switch (e->type) {
  case PRSSIM_EXPR_AND:
  case PRSSIM_EXPR_OR:
    _computeFanout (e->l, s);
    _computeFanout (e->r, s);
    break;

  case PRSSIM_EXPR_NOT:
    _computeFanout (e->l, s);
    break;

  case PRSSIM_EXPR_VAR:
    {
      int off = getGlobalOffset (e->vid, 0); // boolean
      _sc->incFanout (off, 0, s);
    }
    break;

  case PRSSIM_EXPR_TRUE:
  case PRSSIM_EXPR_FALSE:
    break;

  default:
    Assert (0, "What?");
    break;
  }
}
    

void PrsSim::computeFanout ()
{
  prssim_stmt *x;

  for (x = _g->getRules(); x; x = x->next) {
    /* -- create rule -- */
    OnePrsSim *t = new OnePrsSim (this, x);
    list_append (_sim, t);
    if (x->type == PRSSIM_RULE) {
      _computeFanout (x->up[0], t);
      _computeFanout (x->up[1], t);
      _computeFanout (x->dn[0], t);
      _computeFanout (x->dn[1], t);
    }
    else {
      int off;
      if (x->type == PRSSIM_PASSP || x->type == PRSSIM_TGATE) {
	off = getGlobalOffset (x->_g, 0);
	_sc->incFanout (off, 0, t);
      }
      if (x->type == PRSSIM_PASSN || x->type == PRSSIM_TGATE) {
	off = getGlobalOffset (x->g, 0);
	_sc->incFanout (off, 0, t);
      }
      off = getGlobalOffset (x->t1, 0);
      _sc->incFanout (off, 0, t);
      off = getGlobalOffset (x->t2, 0);
      _sc->incFanout (off, 0, t);
    }
  }
}

static int _attr_check (const char *nm, act_attr_t *attr)
{
  while (attr) {
    if (strcmp (attr->attr, nm) == 0) {
      Assert (attr->e->type == E_INT, "What?");
      return attr->e->u.v;
    }
    attr = attr->next;
  }
  return -1;
}

static struct Hashtable *at_table;

prssim_expr *_convert_prs (ActSimCore *sc, act_prs_expr_t *e, int type)
{
  prssim_expr *x, *tmp;
  
  if (!e) return NULL;

  NEW (x, prssim_expr);
  switch (e->type) {
  case ACT_PRS_EXPR_AND:
    if (type == 1) {
      x->type = PRSSIM_EXPR_OR;
    }
    else {
      x->type = PRSSIM_EXPR_AND;
    }
    x->l = _convert_prs (sc, e->u.e.l, type);
    x->r = _convert_prs (sc, e->u.e.r, type);
    break;

  case ACT_PRS_EXPR_OR:
    if (type == 1) {
      x->type = PRSSIM_EXPR_AND;
    }
    else {
      x->type = PRSSIM_EXPR_OR;
    }
    x->l = _convert_prs (sc, e->u.e.l, type);
    x->r = _convert_prs (sc, e->u.e.r, type);
    break;

  case ACT_PRS_EXPR_NOT:
    if (type == 0) {
      FREE (x);
      x = _convert_prs (sc, e->u.e.l, 1);
    }
    else if (type == 1) {
      FREE (x);
      x = _convert_prs (sc, e->u.e.l, 0);
    }
    else {
      x->type = PRSSIM_EXPR_NOT;
      x->l = _convert_prs (sc, e->u.e.l, 2);
      x->r = NULL;
    }
    break;

  case ACT_PRS_EXPR_VAR:
    if (type != 0) {
      x->type = PRSSIM_EXPR_NOT;
      NEW (x->l, prssim_expr);
      x->r = NULL;
      tmp = x->l;
    }
    else {
      tmp = x;
    }
    tmp->type = PRSSIM_EXPR_VAR;
    tmp->vid = sc->getLocalOffset (e->u.v.id, sc->cursi(), NULL);
    tmp->c = e->u.v.id->Canonical (sc->cursi()->bnl->cur);
    break;

  case ACT_PRS_EXPR_TRUE:
    if (type != 0) {
      x->type = PRSSIM_EXPR_FALSE;
    }
    else {
      x->type = PRSSIM_EXPR_TRUE;
    }
    break;

  case ACT_PRS_EXPR_FALSE:
    if (type != 0) {
      x->type = PRSSIM_EXPR_TRUE;
    }
    else {
      x->type = PRSSIM_EXPR_FALSE;
    }
    break;

  case ACT_PRS_EXPR_LABEL:
    {
      hash_bucket_t *b = hash_lookup (at_table, e->u.l.label);
      if (!b) {
	fatal_error ("Unknown label `%s'", e->u.l.label);
      }
      act_prs_lang_t *pl = (act_prs_lang_t *) b->v;
      if (pl->u.one.dir == 0) {
	if (type != 2) {
	  x = _convert_prs (sc, pl->u.one.e, (type == 0 ? 1 : 0));
	}
	else {
	  /* is this right?! */
	  x = _convert_prs (sc, pl->u.one.e, 0);
	}
      }
      else {
	x = _convert_prs (sc, pl->u.one.e, type);
      }
    }
    break;

  default:
    fatal_error ("Huh?");
    x = NULL;
    break;
  }
  return x;
}


static void _merge_prs (ActSimCore *sc, struct prssim_expr **pe,
			act_prs_expr_t *e, int type)
{
  prssim_expr *ex = _convert_prs (sc, e, type);
  if (!*pe) {
    *pe = ex;
  }
  else {
    prssim_expr *x;
    NEW (x, prssim_expr);
    x->type = PRSSIM_EXPR_OR;
    x->l = *pe;
    x->r = ex;
    *pe = x;
  }
  return;
}

void PrsSimGraph::_add_one_rule (ActSimCore *sc, act_prs_lang_t *p)
{
  struct prssim_stmt *s;
  int rhs;
  act_connection *rhsc;

  if (p->u.one.label) {
    hash_bucket_t *b = hash_add (at_table, (char *)p->u.one.id);
    b->v = p;
    return;
  }

  rhs = sc->getLocalOffset (p->u.one.id, sc->cursi(), NULL);
  rhsc = p->u.one.id->Canonical (sc->cursi()->bnl->cur);
  for (s = _rules; s; s = s->next) {
    if (s->type == PRSSIM_RULE) {
      if (s->vid == rhs) {
	break;
      }
    }
  }
  if (!s) {
    NEW (s, struct prssim_stmt);
    s->next = NULL;
    s->type = PRSSIM_RULE;
    s->vid = rhs;
    s->c = rhsc;
    s->unstab = 0;
    s->delay_up = 10;
    s->delay_dn = 10;
    s->up[0] = NULL;
    s->up[1] = NULL;
    s->dn[0] = NULL;
    s->dn[1] = NULL;
    q_ins (_rules, _tail, s);
  }

  int weak, delay;
  if (_attr_check ("weak", p->u.one.attr) == 1) {
    weak = PRSSIM_WEAK;
  }
  else {
    weak = PRSSIM_NORM;
  }
  if (_attr_check ("unstab", p->u.one.attr) == 1) {
    s->unstab = 1;
  }
  delay = _attr_check ("after", p->u.one.attr);

  /*-- now handle the rule --*/

  at_table = _labels;
  switch (p->u.one.arrow_type) {
  case 0:
    /* normal arrow */
    if (p->u.one.dir) {
      _merge_prs (sc, &s->up[weak], p->u.one.e, 0);
      if (delay >= 0) {
	s->delay_up = delay;
      }
    }
    else {
      _merge_prs (sc, &s->dn[weak], p->u.one.e, 0);
      if (delay >= 0) {
	s->delay_dn = delay;
      }
    }
    break;

  case 1:
    /* combinational */
    if (p->u.one.dir) {
      _merge_prs (sc, &s->up[weak], p->u.one.e, 0);
      _merge_prs (sc, &s->dn[weak], p->u.one.e, 1);
    }
    else {
      _merge_prs (sc, &s->dn[weak], p->u.one.e, 0);
      _merge_prs (sc, &s->up[weak], p->u.one.e, 1);
    }
    if (delay >= 0) {
      s->delay_up = delay;
      s->delay_dn = delay;
    }
    break;
    
  case 2:
    /* state-holding */
    if (p->u.one.dir) {
      _merge_prs (sc, &s->up[weak], p->u.one.e, 0);
      _merge_prs (sc, &s->dn[weak], p->u.one.e, 2);
    }
    else {
      _merge_prs (sc, &s->dn[weak], p->u.one.e, 0);
      _merge_prs (sc, &s->up[weak], p->u.one.e, 2);
    }
    if (delay >= 0) {
      s->delay_up = delay;
      s->delay_dn = delay;
    }
    break;
    
  default:
    fatal_error ("Unknown arrow type (%d)", p->u.one.arrow_type);
    break;
  }
  at_table = NULL;
}

void PrsSimGraph::_add_one_gate (ActSimCore *sc, act_prs_lang_t *p)
{
  struct prssim_stmt *s;
  NEW (s, struct prssim_stmt);
  s->next = NULL;
  if (p->u.p.g) {
    if (p->u.p._g) {
      s->type = PRSSIM_TGATE;
    }
    else {
      s->type = PRSSIM_PASSN;
    }
  }
  else {
    s->type = PRSSIM_PASSP;
  }
  if (p->u.p.g) {
    s->g = sc->getLocalOffset (p->u.p.g, sc->cursi(), NULL);
  }
  if (p->u.p._g) {
    s->_g = sc->getLocalOffset (p->u.p._g, sc->cursi(), NULL);
  }
  s->t1 = sc->getLocalOffset (p->u.p.s, sc->cursi(), NULL);
  s->t2 = sc->getLocalOffset (p->u.p.d, sc->cursi(), NULL);

  q_ins (_rules, _tail, s);
}

void PrsSimGraph::addPrs (ActSimCore *sc, act_prs_lang_t *p)
{
  while (p) {
    switch (p->type) {
    case ACT_PRS_RULE:
      _add_one_rule (sc, p);
      break;
      
    case ACT_PRS_GATE:
      _add_one_gate (sc, p);
      break;
      
    case ACT_PRS_TREE:
    case ACT_PRS_SUBCKT:
      addPrs (sc, p->u.l.p);
      break;

    default:
      fatal_error ("Unknown prs type (%d)", p->type);
      break;
    }
    p = p->next;
  }
}

PrsSimGraph::PrsSimGraph ()
{
  _rules = NULL;
  _tail = NULL;
  _labels = hash_new (4);
}

static void _free_prssim_expr (prssim_expr *e)
{
  if (!e) return;
  switch (e->type) {
  case PRSSIM_EXPR_AND:
  case PRSSIM_EXPR_OR:
    _free_prssim_expr (e->r);
  case PRSSIM_EXPR_NOT:
    _free_prssim_expr (e->l);
    break;

  case PRSSIM_EXPR_VAR:
  case PRSSIM_EXPR_TRUE:
  case PRSSIM_EXPR_FALSE:
    break;

  default:
    fatal_error ("free_prssim_expr (%d) unknown\n", e->type);
    break;
  }
  FREE (e);
}

PrsSimGraph::~PrsSimGraph()
{
  hash_free (_labels);
  while (_rules) {
    switch (_rules->type) {
    case PRSSIM_RULE:
      _free_prssim_expr (_rules->up[0]);
      _free_prssim_expr (_rules->up[1]);
      _free_prssim_expr (_rules->dn[0]);
      _free_prssim_expr (_rules->dn[1]);
      break;
      
    case PRSSIM_PASSP:
    case PRSSIM_PASSN:
    case PRSSIM_TGATE:
      break;
    }

    _tail = _rules->next;
    FREE (_rules);
    _rules = _tail;
  }
}

PrsSimGraph *PrsSimGraph::buildPrsSimGraph (ActSimCore *sc, act_prs *p)
{
  PrsSimGraph *pg;

  pg = new PrsSimGraph ();
  
  while (p) {
    pg->addPrs (sc, p->p);
    p = p->next;
  }
  return pg;
}


/* 2 = X */
static const int _not_table[3] = { 1, 0, 2 };

static const int _and_table[3][3] = { { 0, 0, 0 },
				      { 0, 1, 2 },
				      { 0, 2, 2 } };

static const int _or_table[3][3] = { { 0, 1, 2 },
				     { 1, 1, 1 },
				     { 2, 1, 2 } };

#define PENDING_NONE 0
#define PENDING_0    (1+0)
#define PENDING_1    (1+1)
#define PENDING_X    (1+2)

int OnePrsSim::eval (prssim_expr *x)
{
  int a, b;
  if (!x) { return 0; }
  switch (x->type) {
  case PRSSIM_EXPR_AND:
    a = eval (x->l);
    b = eval (x->r);
    return _and_table[a][b];
    break;
    
  case PRSSIM_EXPR_OR:
    a = eval (x->l);
    b = eval (x->r);
    return _or_table[a][b];
    break;

  case PRSSIM_EXPR_NOT:
    a = eval (x->l);
    return _not_table[a];
    break;

  case PRSSIM_EXPR_VAR:
    return _proc->getBool (x->vid);
    break;

  case PRSSIM_EXPR_TRUE:
    return 1;
    break;
    
  case PRSSIM_EXPR_FALSE:
    return 0;
    break;

  default:
    fatal_error("What?");
    break;
  }
  return 0;
}

static int _breakpt;

int OnePrsSim::Step (int ev_type)
{
  int u_state, d_state;
  int t = SIM_EV_TYPE (ev_type);
  const char *nm;

  _breakpt = 0;

  /*-- fire rule --*/
  switch (_me->type) {
  case PRSSIM_PASSP:
  case PRSSIM_PASSN:
  case PRSSIM_TGATE:

    break;

  case PRSSIM_RULE:
    _proc->setBool (_me->vid, t);
    if (flags == (1 + t)) {
      flags = PENDING_NONE;
    }
    break;

  default:
    fatal_error ("What?");
    break;
  }
  return 1-_breakpt;
}

void OnePrsSim::printName ()
{
  _proc->printName (stdout, _me->vid);
}

int OnePrsSim::matches (int val)
{
  if (_proc->getBool (_me->vid) == val) {
    return 1;
  }
  else {
    return 0;
  }
}


#define DO_SET_VAL(x)						\
  do {								\
    if (_proc->getBool (_me->vid) != (x)) {			\
      if (flags != (1 + (x))) {					\
	flags = (1 + (x));					\
	_pending = new Event (this, SIM_EV_MKTYPE ((x), 0),	\
			      _proc->getDelay ((x) == 0 ?	\
					       _me->delay_dn :	\
					       _me->delay_up));	\
      }								\
    }								\
  } while (0)


#define WARNING_MSG(s,t)			\
  do {						\
    _proc->msgPrefix();				\
    printf ("WARNING: " s " on `");		\
    _proc->printName (stdout, _me->vid);	\
    printf (t "'\n");				\
    if (_proc->onWarning() == 2) {		\
      exit (1);					\
    }						\
    else if (_proc->onWarning() == 1) {		\
      _breakpt = 1;				\
    }						\
  } while (0)

#define MAKE_NODE_X						\
  do {								\
    if (_proc->getBool (_me->vid) != 2) {			\
      if (flags != PENDING_X) {					\
	if (_pending) {						\
	  _pending->Remove ();					\
	}							\
	flags = PENDING_X;					\
	_pending = new Event (this, SIM_EV_MKTYPE (2, 0), 1);	\
      }								\
    }								\
    else {							\
      if (flags == PENDING_0 || flags == PENDING_1) {		\
	flags = 0;						\
	if (_pending) {						\
	  _pending->Remove();					\
	}							\
      }								\
    }								\
  } while (0)

void OnePrsSim::propagate ()
{
  int u_state, d_state;
  int u_weak = 0, d_weak = 0;

  /*-- fire rule --*/
  switch (_me->type) {
  case PRSSIM_PASSP:
  case PRSSIM_PASSN:
  case PRSSIM_TGATE:
    break;

  case PRSSIM_RULE:
    /* evaluate up, up-weak and dn, dn-weak */
    u_state = eval (_me->up[PRSSIM_NORM]);
    if (u_state == 0) {
      u_state = eval (_me->up[PRSSIM_WEAK]);
      u_weak = 1;
    }

    d_state = eval (_me->dn[PRSSIM_NORM]);
    if (d_state == 0) {
      d_state = eval (_me->dn[PRSSIM_WEAK]);
      d_weak = 1;
    }

    /* -- check for unstable rules -- */
    if (flags == PENDING_1 && u_state != 1) {
      if (u_state == 2) {
	if (!_proc->isResetMode() && !_me->unstab) {
	  WARNING_MSG ("weak-unstable transition", "+");
	}
      }
      else {
	if (!_me->unstab) {
	  WARNING_MSG ("unstable transition", "+");
	}
      }
      _pending->Remove();
      if (_proc->getBool (_me->vid) != 2) {
	_pending = new Event (this, SIM_EV_MKTYPE (2, 0), 1);
	flags = PENDING_X;
      }
    }

    if (flags == PENDING_0 && d_state != 1) {
      if (d_state == 2) {
	if (!_proc->isResetMode() && !_me->unstab) {
	  WARNING_MSG ("weak-unstable transition", "-");
	}
      }
      else {
	if (!_me->unstab) {
	  WARNING_MSG ("unstable transition", "-");
	}
      }
      _pending->Remove();
      if (_proc->getBool (_me->vid) != 2) {
	_pending = new Event (this, SIM_EV_MKTYPE (2, 0), 1);
	flags = PENDING_X;
      }
    }

    if (u_state == 0) {
      switch (d_state) {
      case 0:
	/* nothing to do */
	break;

      case 1:
      case 2:
	/* set to 0 */
	DO_SET_VAL (0);
	break;
      }
    }
    else if (u_state == 1) {
      switch (d_state) {
      case 0:
	/* set to 1 */
	DO_SET_VAL (1);
	break;

      case 2:
	if (!u_weak && d_weak) {
	  DO_SET_VAL (1);
	}
	else {
	  WARNING_MSG ("weak-interference", "");
	  MAKE_NODE_X;
	}
	break;

      case 1:
	/* interference */
	if (u_weak && !d_weak) {
	  DO_SET_VAL (0);
	}
	else if (!u_weak && d_weak) {
	  DO_SET_VAL (1);
	}
	else {
	  WARNING_MSG ("interference", "");
	  MAKE_NODE_X;
	}
	break;
      }
    }
    else {
      /* u_state == 2 */
      switch (d_state) {
      case 0:
	/* set to 1 */
	DO_SET_VAL (1);
	break;

      case 1:
	if (u_weak && !d_weak) {
	  /* set to 0 */
	  DO_SET_VAL (0);
	}
	else {
	  WARNING_MSG ("weak-interference", "");
	  MAKE_NODE_X;
	}
	break;

      case 2:
	/* set to X */
	if (!_proc->isResetMode()) {
	  WARNING_MSG ("weak-interference", "");
	}
	MAKE_NODE_X;
	break;
      }
    }
    break;

  default:
    fatal_error ("What?");
    break;
  }
}

void PrsSim::printName (FILE *fp, int lid)
{
  act_connection *c;
  int dx;
  c = _sc->getConnFromOffset (_proc, lid, 0, &dx);
  if (!c) {
    fprintf (fp, "-?-");
  }
  else {
    ActId *tmp = c->toid();
    tmp->Print (fp);
    delete tmp;
    if (dx != -1) {
      fprintf (fp, "[%d]", dx);
    }
  }
}


void PrsSim::setBool (int lid, int v)
{
  int off = getGlobalOffset (lid, 0);
  SimDES **arr;
  const char *nm, *nm2;
  int verb;

  verb = 0;
  if ((nm = isWatched (0, lid))) {
    verb = 1;
  }
  if ((nm2 = isBreakPt (0, lid))) {
    verb |= 2;
  }
  if (verb) {
    int oval = _sc->getBool (off);
    if (oval != v) {
      if (verb & 1) {
	msgPrefix ();
	printf ("%s := %c\n", nm, (v == 2 ? 'X' : ((char)v + '0')));
      }
      if (verb & 2) {
	msgPrefix ();
	printf ("*** breakpoint %s\n", nm2);
	_breakpt = 1;
      }
    }
  }
  _sc->setBool (off, v);

  arr = _sc->getFO (off, 0);
  for (int i=0; i < _sc->numFanout (off, 0); i++) {
    ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
    Assert (p, "What?");
    p->propagate ();
  }
}

void PrsSimGraph::checkFragmentation (ActSimCore *sc, PrsSim *ps,
				      ActId *id)
{
  if (id->isFragmented (sc->cursi()->bnl->cur)) {
    ActId *tmp = id->unFragment (sc->cursi()->bnl->cur);
    int type;
    int loff = sc->getLocalOffset (tmp, sc->cursi(), &type);

    if (type == 2 || type == 3) {
      loff = ps->getGlobalOffset (loff, 2);
      act_channel_state *ch = sc->getChan (loff);
      if (type == 2) {
	/* input */
	ch->fragmented |= 1;
      }
      else {
	ch->fragmented |= 2;
	/* output */
      }
      sim_recordChannel (sc, ps, tmp);
      sc->registerFragmented (ch->ct);
      ch->cm = sc->getFragmented (ch->ct);
    }
    delete tmp;
  }
}

void PrsSimGraph::checkFragmentation (ActSimCore *sc, PrsSim *ps,
				      act_prs_expr_t *e)
{
  if (!e) return;
  switch (e->type) {
  case ACT_PRS_EXPR_AND:
  case ACT_PRS_EXPR_OR:
    checkFragmentation (sc, ps, e->u.e.l);
    checkFragmentation (sc, ps, e->u.e.r);
    break;

  case ACT_PRS_EXPR_NOT:
    checkFragmentation (sc, ps, e->u.e.l);
    break;

  case ACT_PRS_EXPR_VAR:
    checkFragmentation (sc, ps, e->u.v.id);
    break;

  case ACT_PRS_EXPR_LABEL:
  case ACT_PRS_EXPR_TRUE:
  case ACT_PRS_EXPR_FALSE:
    break;

  default:
    Assert (0, "Should not be here");
    break;
  }
}

void PrsSimGraph::checkFragmentation (ActSimCore *sc, PrsSim *ps,
				      act_prs_lang_t *p)
{
  while (p) {
    switch (p->type) {
    case ACT_PRS_RULE:
      checkFragmentation (sc, ps, p->u.one.e);
      if (!p->u.one.label) {
	checkFragmentation (sc, ps, p->u.one.id);
      }
      break;

    case ACT_PRS_GATE:
      checkFragmentation (sc, ps, p->u.p.g);
      checkFragmentation (sc, ps, p->u.p._g);
      checkFragmentation (sc, ps, p->u.p.s);
      checkFragmentation (sc, ps, p->u.p.d);
      break;

    case ACT_PRS_TREE:
    case ACT_PRS_SUBCKT:
      checkFragmentation (sc, ps, p->u.l.p);
      break;

    default:
      fatal_error ("loops?!");
      break;
    }
    p = p->next;
  }
}

void PrsSimGraph::checkFragmentation (ActSimCore *sc, PrsSim *ps, act_prs *p)
{
  while (p) {
    checkFragmentation (sc, ps, p->p);
    p = p->next;
  }
}


void PrsSim::dumpState (FILE *fp)
{
  fprintf (fp, "FIXME: prs dump state!\n");
}
