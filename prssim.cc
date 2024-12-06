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

/* 
 * Special event types for single event transient and single event delay
 */
#define PRSSIM_SET_START_EVENT 16
#define PRSSIM_SET_STOP_EVENT 20
#define PRSSIM_SED_EVENT 28


PrsSim::PrsSim (PrsSimGraph *g, ActSimCore *sim, Process *p)
: ActSimObj (sim, p)
{
  _sc = sim;
  _g = g;
  _sim = NULL;
  _nobjs = 0;
  _inst_gate_delay = NULL;
}

PrsSim::~PrsSim()
{
  for (int i=0; i < _nobjs; i++) {
    _sim[i].~OnePrsSim();
    if (_inst_gate_delay) {
      _inst_gate_delay[i]->delete_tables();
      delete _inst_gate_delay[i];
    }
  }
  if (_sim) {
    FREE (_sim);
  }
  if (_inst_gate_delay) {
    FREE (_inst_gate_delay);
  }
  _nobjs = 0;
}


void PrsSim::initState ()
{
  for (int i=0; i < _nobjs; i++) {
    _sim[i].propagate(NULL);
  }
}

int PrsSim::Step (Event */*ev*/)
{
  fatal_error ("This should never be called!");
  return 1;
}

extern ActSim *glob_sim;

void PrsSim::printStatus (int val, bool io_glob)
{
  listitem_t *li;
  int emit_name = 0;

  if (io_glob) {
    stateinfo_t *si = glob_sim->getsi (_proc);

    if (!si) {
      return;
    }
    
    for (int i=0; i < si->ports.numBools(); i++) {
      int port_idx = -(i+1)*2+1;
      int pval = getBool (port_idx);
      if (pval == val) {
	int dy;
	act_connection *c = glob_sim->getConnFromOffset (_proc, port_idx,
							 0, &dy);
	Assert (c, "Hmm");
	
	if (!emit_name) {
	  if (name) {
	    name->Print (stdout);
	  }
	  else {
	    printf ("-top-");
	  }
	  printf (" port:{ ");
	  emit_name = 1;
	}
	else {
	  printf (" ");
	}
	c->Print (stdout);
      }
    }
  }
  else {
    for (int i=0; i < _nobjs; i++) {
      if (_sim[i].matches (val)) {
	if (!emit_name) {
	  if (name) {
	    name->Print (stdout);
	  }
	  else {
	    printf ("-top-");
	  }
	  printf (" { ");
	  emit_name = 1;
	}
	else {
	  printf (" ");
	}
	_sim[i].printName ();
      }
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
  int count = 0;

  for (x = _g->getRules(); x; x = x->next) {
    count++;
  }
  if (count > 0) {
    _nobjs = count;
    MALLOC (_sim, OnePrsSim, count);
  }
  count = 0;
  for (x = _g->getRules(); x; x = x->next) {
    /* -- create rule -- */
    new (&_sim[count++]) OnePrsSim (this, x);
    OnePrsSim *t = &_sim[count-1];

    if (x->type == PRSSIM_RULE) {
      // XXX: check if this is part of a multi-driver!
      int gid = myGid (x->vid);
      MultiPrsSim *mp = _sc->getMulti (gid);
      SimDES *_fo;
      if (mp) {
#if 0
	printf ("found multi! => ");
	name->Print (stdout);
	printf ("/");
	x->c->Print (stdout);
	printf ("\n");
#endif	
	mp->addOnePrsSim (t);
	_fo = mp;
      }
      else {
	_fo = t;
      }
      _computeFanout (x->up[0], _fo);
      _computeFanout (x->up[1], _fo);
      _computeFanout (x->dn[0], _fo);
      _computeFanout (x->dn[1], _fo);
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
      return attr->e->u.ival.v;
    }
    attr = attr->next;
  }
  return -1;
}

static struct Hashtable *at_table;

static sdf_cell *current_ci;
static prssim_stmt *current_stmt;
static double sdf_ts_conv;

/*
  WARNING: this assumes that double -> int conversion truncates the
  fractional part
*/
static int my_conv (double v)
{
  int res;
  double cvt = v * sdf_ts_conv;

  // WARNING: this should not happen
  if (cvt < 0) return 1;
  
  res = (int) cvt;

  double x = cvt - res;

  if (fabs(x-0.5) < 1e-6) {
    // round to nearest even
    if (res & 1) {
      res++;
    }
  }
  else if (x > 0.5) {
    res++;
  }
  return res;
}


// use current_ci to find path 
static sdf_path *_find_sdf_path (ActId *in_id, ActId *out_id, int is_fall)
{
  if (!current_ci) return NULL;
  
  for (int i=0; i < A_LEN (current_ci->_paths); i++) {
    sdf_path *p = &current_ci->_paths[i];
    // from might be NULL
    // to cannot be NULL
    if (p->type == SDF_ELEM_DEVICE) {
      Assert (p->to, "What");
      if (p->to->isEqual (out_id)) {
	// match device!
	p->used = 1;
	return p;
      }
    }
    else if (p->type == SDF_ELEM_IOPATH) {
      Assert (p->to && p->from, "What?");
      if (p->to->isEqual (out_id) && p->from->isEqual (in_id) &&
	  (p->dirfrom == 0 ||
	   (p->dirfrom == 1 /* posedge */ && is_fall == 0) ||
	   (p->dirfrom == 2 /* negedge */ && is_fall == 1))) {
	p->used = 1;
	return p;
      }
    }
    else {
      /* XXX: that's it: we need to put interconnect delays somewhere! */
	
      // PORT, INTERCONNECT, NETDELAY
      if (p->from && p->to) {
	if (p->to->isEqual (out_id) && p->from->isEqual (in_id)) {
	  p->used = 1;
	}
      }
      else if (p->to) {
	if (p->to->isEqual (out_id)) {
	  p->used = 1;
	}
      }
      else {
	Assert (0, "Should not be here!");
	p->used = 1;
      }
    }
  }
  return NULL;
}
				 

static gate_delay_info *current_gi;

static void _record_inst_delays (ActSimCore *sc, act_prs_expr_t *e, int type)
{
  int is_fall;
  
  if (!e) return;

  switch (e->type) {
  case ACT_PRS_EXPR_AND:
  case ACT_PRS_EXPR_OR:
    _record_inst_delays (sc, e->u.e.l, type);
    _record_inst_delays (sc, e->u.e.r, type);
    break;

  case ACT_PRS_EXPR_NOT:
    if (type == 2) {
      _record_inst_delays (sc, e->u.e.l, 2);
    }
    else {
      _record_inst_delays (sc, e->u.e.l, 1-type);
    }
    break;

  case ACT_PRS_EXPR_VAR:
    if (type != 0) {
      is_fall = 1;
    }
    else {
      is_fall = 0;
    }
    {
      int vid = sc->getLocalOffset (e->u.v.id, sc->cursi(), NULL);
      act_connection *c = e->u.v.id->Canonical (sc->cursi()->bnl->cur);
      if (current_ci) {
	/*-- look through SDF paths from e->u.v.id to current_stmt->c --*/
	ActId *out_id = current_stmt->c->toid();
	sdf_path *p = _find_sdf_path (e->u.v.id, out_id, is_fall);
	if (p) {
	  if (p->abs) {
	    current_gi->up.add (vid, my_conv (p->d.z2o.typ), false);
	    current_gi->dn.add (vid, my_conv (p->d.o2z.typ), false);
	  }
	  else {
	    current_gi->up.inc (vid, my_conv (p->d.z2o.typ), false);
	    current_gi->dn.inc (vid, my_conv (p->d.o2z.typ), false);
	  }
	}
	delete out_id;
      }
    }
    break;

  case ACT_PRS_EXPR_TRUE:
  case ACT_PRS_EXPR_FALSE:
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
	  _record_inst_delays (sc, pl->u.one.e, (type == 0 ? 1 : 0));
	}
	else {
	  _record_inst_delays (sc, pl->u.one.e, 0);
	}
      }
      else {
	_record_inst_delays (sc, pl->u.one.e, type);
      }
    }
    break;

  default:
    fatal_error ("Huh?");
    break;
  }
}
	

static prssim_expr *_convert_prs (ActSimCore *sc, act_prs_expr_t *e, int type)
{
  prssim_expr *x, *tmp;
  int is_fall;
  
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
      is_fall = 1;
    }
    else {
      tmp = x;
      is_fall = 0;
    }
    tmp->type = PRSSIM_EXPR_VAR;
    tmp->vid = sc->getLocalOffset (e->u.v.id, sc->cursi(), NULL);
    tmp->c = e->u.v.id->Canonical (sc->cursi()->bnl->cur);
    if (current_ci) {
      /*-- look through SDF paths from e->u.v.id to current_stmt->c --*/
      ActId *out_id = current_stmt->c->toid();

      /* XXX: SDF we're currently ignoring SDF conditions */
      for (int i=0; i < A_LEN (current_ci->_paths); i++) {
	sdf_path *p = &current_ci->_paths[i];
	// from might be NULL
	// to cannot be NULL
	if (p->type == SDF_ELEM_DEVICE) {
	  Assert (p->to, "What");
	  if (p->to->isEqual (out_id)) {
	    // match device!
	    p->used = 1;
	    // ADD TO UP AND DOWN for the output current_stmt->vid */
	    if (current_stmt->simpleDelay()) {
	      current_stmt->setDelayTables();
	    }
	    if (p->abs) {
	      current_stmt->setUpDelay (tmp->vid, my_conv (p->d.z2o.typ));
	      current_stmt->setDnDelay (tmp->vid, my_conv (p->d.o2z.typ));
	    }
	    else {
	      current_stmt->incUpDelay (tmp->vid, my_conv (p->d.z2o.typ));
	      current_stmt->incDnDelay (tmp->vid, my_conv (p->d.o2z.typ));
	    }
	    break;
	  }
	}
	else if (p->type == SDF_ELEM_IOPATH) {
	  Assert (p->to && p->from, "What?");
	  if (p->to->isEqual (out_id) && p->from->isEqual (e->u.v.id) &&
	      (p->dirfrom == 0 ||
	       (p->dirfrom == 1 /* posedge */ && is_fall == 0) ||
	       (p->dirfrom == 2 /* negedge */ && is_fall == 1))) {
	    p->used = 1;
	    if (current_stmt->simpleDelay()) {
	      current_stmt->setDelayTables();
	    }
	    if (p->abs) {
	      current_stmt->setUpDelay (tmp->vid, my_conv (p->d.z2o.typ));
	      current_stmt->setDnDelay (tmp->vid, my_conv (p->d.o2z.typ));
	    }
	    else {
	      current_stmt->incUpDelay (tmp->vid, my_conv (p->d.z2o.typ));
	      current_stmt->incDnDelay (tmp->vid, my_conv (p->d.o2z.typ));
	    }
	    break;
	  }
	}
	else {
	  /* XXX: that's it: we need to put interconnect delays somewhere! */
	  
	  // PORT, INTERCONNECT, NETDELAY
	  if (p->from && p->to) {
	    if (p->to->isEqual (out_id) && p->from->isEqual (e->u.v.id)) {
	      p->used = 1;
	    }
	  }
	  else if (p->to) {
	    if (p->to->isEqual (out_id)) {
	      p->used = 1;
	    }
	  }
	  else {
	    Assert (0, "Should not be here!");
	    p->used = 1;
	  }
	}
      }
      delete out_id;
#if 0
      printf ("look-for: ");
      e->u.v.id->Print (stdout);
      printf ("%c to ", is_fall ? '-' : '+');
      current_stmt->c->Print (stdout);
      printf ("\n");
#endif      
    }
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

void PrsSimGraph::_add_one_rule (ActSimCore *sc, act_prs_lang_t *p, sdf_cell *ci)
{
  struct prssim_stmt *s;
  int rhs;
  act_connection *rhsc;

  at_table = _labels;
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
    if (ci) {
      s->setDelayTables ();
      ci->used = 1;
    }
    else {
      s->setDelayDefault ();
    }
    s->delay_override_length = 0;
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
  current_stmt = s;
  current_ci = ci;
  switch (p->u.one.arrow_type) {
  case 0:
    /* normal arrow */
    if (p->u.one.dir) {
      _merge_prs (sc, &s->up[weak], p->u.one.e, 0);
      if (delay >= 0 && s->simpleDelay()) {
	s->setUpDelay (delay);
      }
    }
    else {
      _merge_prs (sc, &s->dn[weak], p->u.one.e, 0);
      if (delay >= 0 && s->simpleDelay()) {
	s->setDnDelay (delay);
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
    if (delay >= 0 && s->simpleDelay()) {
      s->setUpDelay (delay);
      s->setDnDelay (delay);
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
    if (delay >= 0 && s->simpleDelay()) {
      s->setUpDelay (delay);
      s->setDnDelay (delay);
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
  s->setDelayDefault ();
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

void PrsSimGraph::addPrs (ActSimCore *sc, act_prs_lang_t *p, sdf_cell *ci)
{
  double conv, sdf_units;
  conv = sc->getTimescale ();
  sdf_units = sc->sdfTimeMetricUnits();

  if (sdf_units >= 0) {
    // sdf exists, has units!
    //    time = X in SDF => X * sdf_units => (X * sdf_units) / conv units
    conv = sdf_units/conv;
  }
  else {
    conv = -1;
  }
  /* set conversion from sdf units to actsim integer units. */
  sdf_ts_conv = conv;
  
  while (p) {
    switch (ACT_PRS_LANG_TYPE (p->type)) {
    case ACT_PRS_RULE:
      _add_one_rule (sc, p, ci);
      break;
      
    case ACT_PRS_GATE:
      _add_one_gate (sc, p);
      break;

    case ACT_PRS_DEVICE:
      /* devs not simulated */
      break;
      
    case ACT_PRS_TREE:
    case ACT_PRS_SUBCKT:
      addPrs (sc, p->u.l.p, ci);
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
      if (!_rules->std_delay) {
	_rules->delay.delete_tables();
      }
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

PrsSimGraph *PrsSimGraph::buildPrsSimGraph (ActSimCore *sc, act_prs *p,
					    sdf_cell *ci)
{
  PrsSimGraph *pg;

  pg = new PrsSimGraph ();
  
  while (p) {
    pg->addPrs (sc, p->p, ci);
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

int OnePrsSim::eval (prssim_expr *x, int cause, int *lid)
{
  int a, b;
  if (!x) { return 0; }
  switch (x->type) {
  case PRSSIM_EXPR_AND:
    a = eval (x->l, cause, lid);
    b = eval (x->r, cause, lid);
    return _and_table[a][b];
    break;
    
  case PRSSIM_EXPR_OR:
    a = eval (x->l, cause, lid);
    b = eval (x->r, cause, lid);
    return _or_table[a][b];
    break;

  case PRSSIM_EXPR_NOT:
    a = eval (x->l, cause, lid);
    return _not_table[a];
    break;

  case PRSSIM_EXPR_VAR:
    {
      int gid;
      int ret = _proc->getBool (x->vid, &gid);
      if (gid == cause) {
	*lid = x->vid;
      }
      return ret;
    }
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

/*
 * Delay lookup:
 *    - if this has an instance-specific delay, use it!
 *    - otherwise get the delay from the instance-agnostic table.
 *
 *   ob = object corresponding to firing
 *   of = object that redcords flags
 *   These two are the same except for multi-drivers; in
 *   multi-drivers, the 0th object is used for flag information.
 *
 *   nid = node id
 *   lidc, gird = local / global cause id
 *
 *   x = value to be set (0 or 1)
 *
 */
#define DO_SET_VAL(ob,of,nid,lidc,gidc,x)				\
  do {									\
    if ((ob)->_proc->getBool ((ob)->nid) != (x)) {			\
      if ((of)->flags != (1 + (x))) {					\
	int ed;								\
	if (gidc != -1) {						\
	  /* cause */							\
	  gate_delay_info *gi = (ob)->_proc->getInstDelay (ob);		\
	  if (gi) {							\
	    ed =((x) == 0 ? gi->dn.lookup (lidc, false) : gi->up.lookup (lidc, false));	\
	  }								\
	  else {							\
	    ed = ((x) == 0 ? (ob)->_me->delayDn (lidc) : (ob)->_me->delayUp (lidc)); \
	  }								\
	}								\
	else {								\
	  ed = ((x) == 0 ? (ob)->_me->delayDn (0) : (ob)->_me->delayUp (0)); \
	}								\
	ed = (ob)->_proc->getDelay (ed);				\
        if ((ob)->_me->delay_override_length != 0) {                    \
          ed = (ob)->_me->delay_override_length;                        \
          (ob)->_me->delay_override_length = 0;                         \
        }                                                               \
	(of)->flags = (1 + (x));					\
	(of)->_pending = new Event (this, SIM_EV_MKTYPE ((x), 0), ed);	\
      }									\
    }									\
  } while (0)


int OnePrsSim::Step (Event *ev)
{
  void *cause = ev->getCause ();
  int causeid;
  int lid = -1;
  int ev_type = ev->getType ();
  int t = SIM_EV_TYPE (ev_type);

  if (cause) {
    ActSimDES *xx = (ActSimDES *) cause;
    causeid = xx->causeGlobalIdx();
  }
  else {
    causeid = -1;
  }

  _breakpt = 0;
  _pending = NULL;

  /*-- fire rule --*/
  switch (_me->type) {
  case PRSSIM_PASSP:
  case PRSSIM_PASSN:
  case PRSSIM_TGATE:

    // if this is an SET or delay event, ignore
    if ((t & 0b11100) == PRSSIM_SET_START_EVENT || t == PRSSIM_SET_STOP_EVENT || t == PRSSIM_SED_EVENT) {
      break;
    }

    if (flags == (1+t)) {
      flags = PENDING_NONE;
    }
    if (!_proc->setBool (_me->t2, t, this, (ActSimObj *)ev->getCause())) {
      flags = PENDING_NONE;
    }
    break;

  case PRSSIM_RULE:

    // if this is an SED event, install the delay and continue
    if ((t & 0b11111) == PRSSIM_SED_EVENT) {
      _me->delay_override_length = t >> 5;
      return 1-_breakpt;
    }

    // if this is an SET event, force the value immediately
    if ((t & 0b11100) == PRSSIM_SET_START_EVENT) {
      Assert (!_proc->isMasked (_me->vid), "No two SETs at the same time allowed! Too upsetting...");

      _proc->setForced (_me->vid, t & 0b11);
      return 1-_breakpt;
    }

    // return to normal operation once the SET has ended
    if (t == PRSSIM_SET_STOP_EVENT) {
      
      _proc->unmask (_me->vid);
      return 1-_breakpt;
    }

    // it seems to be a normal value update
    if (flags == (1 + t)) {
      flags = PENDING_NONE;
    }
    if (!_proc->setBool (_me->vid, t, this, (ActSimObj *)ev->getCause())) {
      flags = PENDING_NONE;
    }

    /* 
       I just set this node to X and there is nothing pending;
       so check if there should be a X -> 0 or X -> 1 tarnsition
       pending that cleans up this X value
    */
    if (t == 2 /* X */ && flags == PENDING_NONE) {
      int u_state, d_state, u_weak, d_weak;

      // evaluate the pullup network
      u_weak = 0;
      d_weak = 0;
      u_state = eval (_me->up[PRSSIM_NORM], causeid,
		      causeid == -1 ? NULL : &lid);
      if (u_state == 0) {
	u_state = eval (_me->up[PRSSIM_WEAK], causeid,
			causeid == -1 ? NULL : &lid);
	if (u_state != 0) {
	  u_weak = 1;
	}
      }

      // evaluate the pulldown network
      d_state = eval (_me->dn[PRSSIM_NORM], causeid,
		      causeid == -1 ? NULL : &lid);
      if (d_state == 0) {
	d_state = eval (_me->dn[PRSSIM_WEAK], causeid,
			causeid == -1 ? NULL : &lid);
	if (d_state != 0) {
	  d_weak = 1;
	}
      }

      /* copied from propagate() */

      // we're being pulled down, update to 0
      if (u_state == 0) {
	if (d_state == 1) {
	  DO_SET_VAL (this,this,_me->vid, lid, causeid, 0);
	}
      }

      // something is pulling up
      else if (u_state == 1) {
	if (d_state == 0) {
	  DO_SET_VAL (this,this,_me->vid, lid, causeid, 1);
	}
	else if (d_state == 2 && (!u_weak && d_weak)) {
	  DO_SET_VAL (this,this,_me->vid, lid, causeid, 1);
	}
	else if (d_state == 1) {
	  if (u_weak && !d_weak) {
	    DO_SET_VAL (this,this,_me->vid, lid, causeid, 0);
	  }
	  else if (!u_weak && d_weak) {
	    DO_SET_VAL (this,this,_me->vid, lid, causeid, 1);
	  }
	}
      }
      else {
	/* u_state == 2 */
	if (d_state == 1) {
	  if (u_weak && !d_weak) {
	    DO_SET_VAL (this,this,_me->vid, lid, causeid, 0);
	  }
	}
      }
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


#define WARNING_MSG(ob,s,t)				\
  do {							\
    (ob)->_proc->msgPrefix();				\
    printf ("WARNING: " s " on `");			\
    (ob)->_proc->printName (stdout, (ob)->_me->vid);	\
    if (cause) {					\
      ActSimDES *xx = (ActSimDES *) cause;		\
      char buf[1024];					\
      xx->sPrintCause (buf, 1024);			\
      printf ("'   [by %s]\n", buf);			\
    }							\
    else {						\
      printf (t "'\n");					\
    }							\
    if ((ob)->_proc->onWarning() == 2) {		\
      exit (1);						\
    }							\
    else if ((ob)->_proc->onWarning() == 1) {		\
      _breakpt = 1;					\
    }							\
  } while (0)


#define MAKE_NODE_X(obj,nid)						\
  do {									\
    if ((obj)->_proc->getBool ((obj)->nid) != 2) {			\
      if ((obj)->flags != PENDING_X) {					\
	if ((obj)->_pending) {						\
	  (obj)->_pending->Remove ();					\
	}								\
	(obj)->flags = PENDING_X;					\
	(obj)->_pending = new Event (this, SIM_EV_MKTYPE (2, 0), 1, cause); \
      }									\
    }									\
    else {								\
      if ((obj)->flags == PENDING_0 || (obj)->flags == PENDING_1) {	\
	(obj)->flags = 0;						\
	if ((obj)->_pending) {						\
	  (obj)->_pending->Remove();					\
	}								\
      }									\
    }									\
  } while (0)

void OnePrsSim::propagate (void *cause)
{
  int u_state, d_state;
  int u_weak = 0, d_weak = 0;
  int lid = -1;
  int causeid;

  if (cause) {
    ActSimDES *xx = (ActSimDES *) cause;
    causeid = xx->causeGlobalIdx ();
  }
  else {
    causeid = -1;
  }

  /*-- fire rule --*/
  switch (_me->type) {
  case PRSSIM_PASSP:
    /* XXX: SDF right now we don't trace SDF delays through
       pass/transmission gates */
    u_state = _proc->getBool (_me->_g);
    if (u_state == 0) {
      u_weak = _proc->getBool (_me->t1);
      d_weak = _proc->getBool (_me->t2);
      if (u_weak == 1 && d_weak != 1) {
	DO_SET_VAL (this,this,_me->t2, -1, -1, 1);
      }
      else if (u_weak == 2 && d_weak != 2) {
	MAKE_NODE_X (this,_me->t2);
      }
    }
    else if (u_state == 2) {
      u_weak = _proc->getBool (_me->t1);
      d_weak = _proc->getBool (_me->t2);
      if (u_weak == 1 && d_weak != 1) {
	MAKE_NODE_X (this,_me->t2);
      }
    }
    break;
    
  case PRSSIM_PASSN:
    u_state = _proc->getBool (_me->g);
    if (u_state == 1) {
      u_weak = _proc->getBool (_me->t1);
      d_weak = _proc->getBool (_me->t2);
      if (u_weak == 0 && d_weak != 0) {
	DO_SET_VAL (this,this,_me->t2, -1, -1, 0);
      }
      else if (u_weak == 2 && d_weak != 2) {
	MAKE_NODE_X (this,_me->t2);
      }
    }
    else if (u_state == 2) {
      u_weak = _proc->getBool (_me->t1);
      d_weak = _proc->getBool (_me->t2);
      if (u_weak == 0 && d_weak != 0) {
	MAKE_NODE_X (this,_me->t2);
      }
    }
    break;
    
  case PRSSIM_TGATE:
    u_state = _proc->getBool (_me->_g);
    d_state = _proc->getBool (_me->g);
    u_weak = _proc->getBool (_me->t1);
    d_weak = _proc->getBool (_me->t2);
    if (u_weak == 1) {
      if (u_state == 0) {
	DO_SET_VAL (this,this,_me->t2, -1, -1, 1);
      }
      else if (u_state == 2) {
	MAKE_NODE_X (this,_me->t2);
      }
    }
    else if (u_weak == 0) {
      if (d_state == 1) {
	DO_SET_VAL (this,this,_me->t2, -1, -1, 0);
      }
      else if (d_state == 2) {
	MAKE_NODE_X (this,_me->t2);
      }
    }
    else if (u_weak == 2 && (d_state == 1 || u_state == 0)) {
      MAKE_NODE_X (this,_me->t2);
    }
    break;

  case PRSSIM_RULE:
    /* evaluate up, up-weak and dn, dn-weak */
    u_state = eval (_me->up[PRSSIM_NORM], causeid, causeid == -1 ? NULL : &lid);
    if (u_state == 0) {
      u_state = eval (_me->up[PRSSIM_WEAK], causeid,
		      causeid == -1 ? NULL : &lid);
      if (u_state != 0) {
        u_weak = 1;
      }
    }

    d_state = eval (_me->dn[PRSSIM_NORM], causeid,
		    causeid == -1 ? NULL : &lid);
    if (d_state == 0) {
      d_state = eval (_me->dn[PRSSIM_WEAK], causeid,
		      causeid == -1 ? NULL : &lid);
      if (d_state != 0) {
	d_weak = 1;
      }
    }

    /* -- check for unstable rules -- */
    if (flags == PENDING_1 && u_state != 1) {
      if (u_state == 2) {
	if (!_proc->isResetMode() && !_me->unstab) {
	  if (!_proc->isHazard (_me->vid)) {
	    WARNING_MSG (this,"weak-unstable transition", "+");
	  }
	}
      }
      else {
	if (!_me->unstab) {
	  WARNING_MSG (this,"unstable transition", "+");
	}
      }
      MAKE_NODE_X (this,_me->vid);
#if 0
      _pending->Remove();
      if (_proc->getBool (_me->vid) != 2) {
	_pending = new Event (this, SIM_EV_MKTYPE (2, 0), 1, cause);
	flags = PENDING_X;
      }
#endif      
    }

    if (flags == PENDING_0 && d_state != 1) {
      if (d_state == 2) {
	if (!_proc->isResetMode() && !_me->unstab) {
	  if (!_proc->isHazard (_me->vid)) {
	    WARNING_MSG (this,"weak-unstable transition", "-");
	  }
	}
      }
      else {
	if (!_me->unstab && !_proc->isHazard (_me->vid)) {
	  WARNING_MSG (this,"unstable transition", "-");
	}
      }
      MAKE_NODE_X (this,_me->vid);
#if 0      
      _pending->Remove();
      if (_proc->getBool (_me->vid) != 2) {
	_pending = new Event (this, SIM_EV_MKTYPE (2, 0), 1, cause);
	flags = PENDING_X;
      }
#endif      
    }

    if (u_state == 0) {
      switch (d_state) {
      case 0:
	/* nothing to do */
	break;

      case 1:
	/* set to 0 */
	DO_SET_VAL (this,this,_me->vid, lid, causeid, 0);
	break;
	
      case 2:
	if (_proc->getBool (_me->vid) == 1) {
	  /* u = 0, d = X: if output=1, it is now X */
	  MAKE_NODE_X (this,_me->vid);
	}
	break;
      }
    }
    else if (u_state == 1) {
      switch (d_state) {
      case 0:
	/* set to 1 */
	DO_SET_VAL (this,this,_me->vid, lid, causeid, 1);
	break;

      case 2:
	if (!u_weak && d_weak) {
	  DO_SET_VAL (this,this,_me->vid, lid, causeid, 1);
	}
	else {
	  if (!_proc->isResetMode()) {
	    WARNING_MSG (this,"weak-interference", "");
	  }
	  MAKE_NODE_X (this,_me->vid);
	}
	break;

      case 1:
	/* interference */
	if (u_weak && !d_weak) {
	  DO_SET_VAL (this,this,_me->vid, lid, causeid, 0);
	}
	else if (!u_weak && d_weak) {
	  DO_SET_VAL (this,this,_me->vid, lid, causeid, 1);
	}
	else {
	  WARNING_MSG (this, "interference", "");
	  MAKE_NODE_X (this,_me->vid);
	}
	break;
      }
    }
    else {
      /* u_state == 2 */
      switch (d_state) {
      case 0:
	if (_proc->getBool (_me->vid) == 0) {
	  MAKE_NODE_X (this,_me->vid);
	}
	break;

      case 1:
	if (u_weak && !d_weak) {
	  /* set to 0 */
	  DO_SET_VAL (this,this,_me->vid, lid, causeid, 0);
	}
	else {
	  if (!_proc->isResetMode()) {
	    WARNING_MSG (this, "weak-interference", "");
	  }
	  MAKE_NODE_X (this,_me->vid);
	}
	break;

      case 2:
	/* set to X */
	if (!_proc->isResetMode()) {
	  WARNING_MSG (this, "weak-interference", "");
	}
	MAKE_NODE_X (this,_me->vid);
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


void PrsSim::sPrintName (char *buf, int sz, int lid)
{
  act_connection *c;
  int dx;
  c = _sc->getConnFromOffset (_proc, lid, 0, &dx);
  if (!c) {
    snprintf (buf, sz, "-?-");
  }
  else {
    ActId *tid = c->toid();
    int pos = 0;
    tid->sPrint (buf, sz);
    delete tid;
    if (dx != -1) {
      pos = strlen (buf);
      sz -= pos;
      if (sz <= 1) return;
      snprintf (buf + pos, sz, "[%d]", dx);
    }
  }
}



bool PrsSim::setBool (int lid, int v, OnePrsSim *me, ActSimObj *cause)
{
  // grab the global offset of this node
  int off = getGlobalOffset (lid, 0);

  SimDES **arr;
  const ActSimCore::watchpt_bucket *nm;
  const char *nm2;
  int verb;
  int oval;

  verb = 0;

#ifdef DUMP_ALL
  verb = 1;
#endif

  // check if this node is watched
  if ((nm = _sc->chkWatchPt (0, off))) {
    verb = 1;
  }

  // check if this node is a break point
  if ((nm2 = _sc->chkBreakPt (0, off))) {
    verb |= 2;
  }

  // if either is true, we need to get the value first
  if (verb) {
    oval = _sc->getBool (off);
  }

#ifdef DUMP_ALL
  char buf[100];
  if (!nm) {
    snprintf (buf, 100, "nm#%d:g%d", lid, off);
    nm = buf;
  }
#endif  
  
  // try to set the node to a new value
  if (_sc->setBool (off, v)) {
    
    // if the node is being watched in some form, handle that
    if (verb) {
      if (oval != v) {
      	if (verb & 1) {
      	  msgPrefix ();

          // if the node is masked, because a different value is forced onto it
          // right now, mark that in the output
          if (_sc->isMasked (off)) {
      	    printf ("%s |= %c", nm->s, (v == 2 ? 'X' : ((char)v + '0')));
          }
          else {
      	    printf ("%s := %c", nm->s, (v == 2 ? 'X' : ((char)v + '0')));
          }
	  if (cause) {
	    ActSimDES *xx = (ActSimDES *) cause;
	    char buf[1024];
	    xx->sPrintCause (buf, 1024);
	    printf ("   [by %s]", buf);
	  }
	  printf ("\n");

	  BigInt tmpv;
	  tmpv = v;
	  _sc->recordTrace (nm, 0, ACT_CHAN_IDLE, tmpv);

	}
	if (verb & 2) {
	  msgPrefix ();
	  printf ("*** breakpoint %s\n", nm2);
	  _breakpt = 1;
	}
      }
    }

    // if the node is not currently masked by a forced value,
    // propagate the new value to the fanout of the node
    if (!_sc->isMasked (off)) {
      
      // grab the fanout
      arr = _sc->getFO (off, 0);

#ifdef DUMP_ALL
      printf (" >>> fanout: %d\n", _sc->numFanout (off, 0));
#endif

      // for every fanout connection, propagate the new value
      for (int i=0; i < _sc->numFanout (off, 0); i++) {
        ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
        Assert (p, "What?");

#ifdef DUMP_ALL
        printf ("   prop: ");
        {
	  ActSimObj *obj = dynamic_cast<ActSimObj *>(p);
	  if (obj) {
	    if (obj->getName()) {
	      obj->getName()->Print (stdout);
	    }
	    else {
	      printf ("-none-");
	    }
	  }
	  else {
	    printf ("#%p", p);
	  }
        }
        printf ("\n");
#endif      
        p->propagate (me);
      }
    }
    return true;
  }
  else {
    return false;
  }
}

void PrsSim::setForced (int lid, int v) 
{
  // grab the global ID
  int off = getGlobalOffset (lid, 0);

  SimDES **arr;
  const ActSimCore::watchpt_bucket *nm;
  const char *nm2;
  int verb;
  int oval;

  verb = 0;

#ifdef DUMP_ALL
  verb = 1;
#endif

  // check if this node is watched
  if ((nm = _sc->chkWatchPt (0, off))) {
    verb = 1;
  }

  // check if this node is a break point
  if ((nm2 = _sc->chkBreakPt (0, off))) {
    verb |= 2;
  }

  oval = _sc->getBool (off);

#ifdef DUMP_ALL
  char buf[100];
  if (!nm) {
    snprintf (buf, 100, "nm#%d:g%d", lid, off);
    nm = buf;
  }
#endif  
  
  // force the node to the new value
  _sc->setForced (off, v);
  
  // if the node is being watched in some form, handle that
  if (verb) {
    if (verb & 1) {
      msgPrefix ();
      printf ("%s <= %c\n", nm->s, (v == 2 ? 'X' : ((char)v + '0')));

      BigInt tmpv;
      tmpv = v;
      _sc->recordTrace (nm, 0, ACT_CHAN_IDLE, tmpv);

    }

    if (verb & 2) {
      msgPrefix ();
      printf ("*** breakpoint %s\n", nm2);
      _breakpt = 1;
    }
  }

  // if the forced value is different to the old value of the node,
  // propagate the new value to the fanout of the node
  if (v != oval) {
    
    // grab the fanout
    arr = _sc->getFO (off, 0);

#ifdef DUMP_ALL
    printf (" >>> fanout: %d\n", _sc->numFanout (off, 0));
#endif

    // for every fanout connection, propagate the new value
    for (int i=0; i < _sc->numFanout (off, 0); i++) {
      ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
      Assert (p, "What?");

#ifdef DUMP_ALL
      printf ("   prop: ");
      {
        ActSimObj *obj = dynamic_cast<ActSimObj *>(p);
        if (obj) {
          if (obj->getName()) {
            obj->getName()->Print (stdout);
          }
          else {
            printf ("-none-");
          }
        }
        else {
          printf ("#%p", p);
        }
      }
      printf ("\n");
#endif

      p->propagate (nullptr);
    }
  }
}

bool PrsSim::unmask (int lid)
{
  // grab the global ID
  int off = getGlobalOffset (lid, 0);

  SimDES **arr;
  const ActSimCore::watchpt_bucket *nm;
  const char *nm2;
  int verb;
  int oval;

  verb = 0;

#ifdef DUMP_ALL
  verb = 1;
#endif

  // check if this node is watched
  if ((nm = _sc->chkWatchPt (0, off))) {
    verb = 1;
  }

  // check if this node is a break point
  if ((nm2 = _sc->chkBreakPt (0, off))) {
    verb |= 2;
  }

  oval = _sc->getBool (off);

#ifdef DUMP_ALL
  char buf[100];
  if (!nm) {
    snprintf (buf, 100, "nm#%d:g%d", lid, off);
    nm = buf;
  }
#endif  
  
  // try to unmask the node
  if (_sc->unmask (off)) {

    // get the restored value
    int v = _sc->getBool (off);
    
    // if the node is being watched in some form, handle that
    if (verb) {
      if (verb & 1) {
        msgPrefix ();
        printf ("%s <- %c\n", nm->s, (v == 2 ? 'X' : ((char)v + '0')));

        BigInt tmpv;
        tmpv = v;
        _sc->recordTrace (nm, 0, ACT_CHAN_IDLE, tmpv);

      }

      if (verb & 2) {
        msgPrefix ();
        printf ("*** breakpoint %s\n", nm2);
        _breakpt = 1;
      }
    }

    // if the restored value is different than the forced value,
    // propagate the new value to the fanout of the node
    if (v != oval) {
      
      // grab the fanout
      arr = _sc->getFO (off, 0);

#ifdef DUMP_ALL
      printf (" >>> fanout: %d\n", _sc->numFanout (off, 0));
#endif

      // for every fanout connection, propagate the new value
      for (int i=0; i < _sc->numFanout (off, 0); i++) {
        ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
        Assert (p, "What?");

#ifdef DUMP_ALL
        printf ("   prop: ");
        {
        	ActSimObj *obj = dynamic_cast<ActSimObj *>(p);
        	if (obj) {
        	  if (obj->getName()) {
        	    obj->getName()->Print (stdout);
        	  }
        	  else {
        	    printf ("-none-");
        	  }
        	}
        	else {
        	  printf ("#%p", p);
        	}
        }
        printf ("\n");
#endif

        p->propagate (nullptr);
      }
    }
    return true;
  }
  else {
    return false;
  }
}

OnePrsSim* PrsSim::findRule (int vid) 
{
  listitem_t *li;

  // find the node which corresponds to the given global ID
  for (int i=0; i < _nobjs; i++) {
    if (_sim[i].getMyLocalID() == vid) return _sim + i;
  }

  Assert(0, "Rule not in here despite sim thinks it is");
  return nullptr;
}

void PrsSimGraph::checkFragmentation (ActSimCore *sc, PrsSim *ps,
				      ActId *id, int read_only)
{
  if (!id) return;
  sc->checkFragmentation (id, ps, sc->cursi(), read_only);
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
    checkFragmentation (sc, ps, e->u.v.id, 1);
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
	checkFragmentation (sc, ps, p->u.one.id, 0);
      }
      break;

    case ACT_PRS_GATE:
      checkFragmentation (sc, ps, p->u.p.g, 0);
      checkFragmentation (sc, ps, p->u.p._g, 0);
      checkFragmentation (sc, ps, p->u.p.s, 0);
      checkFragmentation (sc, ps, p->u.p.d, 1);
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

OnePrsSim::OnePrsSim (PrsSim *p, struct prssim_stmt *x)
{
  _proc = p;
  _me = x;
  _pending = NULL;
  
}

void OnePrsSim::registerExcl ()
{
  int gid;

  if (_me->type == PRSSIM_RULE) {
    gid = _proc->myGid (_me->vid);
    ActExclConstraint *xc = ActExclConstraint::findHi (gid);
    while (xc) {
      xc->addObject (gid, this);
      xc = xc->getNext (gid);
    }
    xc = ActExclConstraint::findLo (gid);
    while (xc) {
      xc->addObject (gid, this);
      xc = xc->getNext (gid);
    }
  }
}


bool OnePrsSim::registerSETEvent (int start_delay, int transient_duration, int force_value) {
  // create the transient start event
  int start_event = 0b10000 | (force_value & 0b11);

  // we don't really need to do anything beyond creating the events
  // the constructor automatically inserts them into the event queue
  new Event (this, SIM_EV_MKTYPE ((start_event), 0), start_delay);
  new Event (this, SIM_EV_MKTYPE ((0b10100), 0), start_delay + transient_duration);

  return true;
}


bool OnePrsSim::registerSEDEvent (int start_delay, int delay_duration) {
  // create the delay event
  int delay_event = 0b11100 | (delay_duration << 5);

  // we don't really need to do anything beyond creating the events
  // the constructor automatically inserts them into the event queue
  new Event (this, SIM_EV_MKTYPE ((delay_event), 0), start_delay);

  return true;
}


void PrsSim::registerExcl ()
{
  for (int i=0; i < _nobjs; i++) {
    _sim[i].registerExcl ();
  }
}


void OnePrsSim::flushPending ()
{
  if (_pending) {
    _pending->Remove ();
    flags = PENDING_NONE;
  }
}

void OnePrsSim::sPrintCause (char *buf, int sz)
{
  int pos = 0;
  int len;
  ActId *inst = _proc->getName(); 
  if (inst) {
    inst->sPrint (buf + pos, sz);
    len = strlen (buf);
    pos += len;
    sz -= len;
    if (sz <= 1) return;
    snprintf (buf + pos, sz, ".");
    pos++;
    sz--;
    if (sz <= 1) return;
  }
  _proc->sPrintName (buf + pos, sz, _me->vid);
  len = strlen (buf + pos);
  pos += len;
  sz -= len;
  if (sz <= 1) return;
  int cv = _proc->getBool (_me->vid);
  snprintf (buf + pos, sz, " <- %c", (cv == 2 ? 'X' : ((char)cv + '0')));
}

int OnePrsSim::causeGlobalIdx ()
{
  return _proc->getGlobalOffset (_me->vid, 0);
}



void PrsSim::_updatePrs (act_prs_lang_t *p)
{
  struct prssim_stmt *s;
  while (p) {
    switch (ACT_PRS_LANG_TYPE (p->type)) {
    case ACT_PRS_RULE:
      {
	int count = 0;
	if (p->u.one.label) return;
	/* now find the location of this rule in the prs list */
	int rhs = _sc->getLocalOffset (p->u.one.id, _sc->cursi(), NULL);
	for (s = _g->getRules(); s; s = s->next) {
	  if (s->type == PRSSIM_RULE) {
	    if (s->vid == rhs) {
	      current_gi = _inst_gate_delay[count];
	      current_stmt = s;
	      break;
	    }
	  }
	  count++;
	}
	Assert (s, "Something went wrong");
	
	switch (p->u.one.arrow_type) {
	case 0:
	  // normal arrow
	  _record_inst_delays (_sc, p->u.one.e, 0);
	  break;
	case 1:
	  // combinational
	  _record_inst_delays (_sc, p->u.one.e, 0);
	  _record_inst_delays (_sc, p->u.one.e, 1);
	  break;
	case 2:
	  _record_inst_delays (_sc, p->u.one.e, 0);
	  _record_inst_delays (_sc, p->u.one.e, 2);
	  break;
	default:
	  Assert (0, "Illegal arrow type");
	  break;
	}
	current_gi = NULL;
	current_stmt = NULL;
      }
      break;
      
    case ACT_PRS_GATE:
      // XXX: don't handle gates yet!
      break;

    case ACT_PRS_DEVICE:
      /* devs not simulated */
      break;
      
    case ACT_PRS_TREE:
    case ACT_PRS_SUBCKT:
      _updatePrs (p->u.l.p);
      break;

    default:
      fatal_error ("Unknown prs type (%d)", p->type);
      break;
    }
    p = p->next;
  }
}

void PrsSim::updateDelays (act_prs *prs, sdf_celltype *ci)
{
  if (!ci) return;
  if (!ci->inst) return;

  chash_bucket_t *cb;
  cb = chash_lookup (ci->inst, name);
  if (!cb) return;

  sdf_cell *di = (sdf_cell *)cb->v;

  double conv, sdf_units;
  conv = _sc->getTimescale ();
  sdf_units = _sc->sdfTimeMetricUnits();

  if (sdf_units >= 0) {
    // sdf exists, has units!
    //    time = X in SDF => X * sdf_units => (X * sdf_units) / conv units
    conv = sdf_units/conv;
  }
  else {
    conv = -1;
  }
  /* set conversion from sdf units to actsim integer units. */
  sdf_ts_conv = conv;

   di->used = true;
  // XXX: SDF now we need to translate this to internal delay info!
  prssim_stmt *x;
  int count = 0;
  for (x = _g->getRules(); x; x = x->next) {
    count++;
  }
  if (count > 0) {
    MALLOC (_inst_gate_delay, gate_delay_info *, count);
    for (int i=0; i < count; i++) {
      _inst_gate_delay[i] = new gate_delay_info;
      _inst_gate_delay[i]->mkTables ();
    }
  }

  at_table = _g->getLabels ();
  current_ci = di;
  while (prs) {
    _updatePrs (prs->p);
    prs = prs->next;
  }
}


inline gate_delay_info *PrsSim::getInstDelay (OnePrsSim *sim)
{
  int idx;
  if (!_inst_gate_delay) return NULL;
  idx = (int) (sim - _sim);
  if (idx < _nobjs) {
    return _inst_gate_delay[idx];
  }
  return NULL;
}


int MultiPrsSim::Step (Event *ev)
{
  void *cause = ev->getCause ();
  int causeid;
  int lid = -1;
  int ev_type = ev->getType ();
  int t = SIM_EV_TYPE (ev_type);

  if (cause) {
    ActSimDES *xx = (ActSimDES *) cause;
    causeid = xx->causeGlobalIdx();
  }
  else {
    causeid = -1;
  }

  _breakpt = 0;
  _objs[0]->_pending = NULL;

  if (_objs[0]->flags == (1 + t)) {
    _objs[0]->flags = PENDING_NONE;
  }
  if (!_objs[0]->_proc->setBool (_objs[0]->_me->vid, t,
				 _objs[0], (ActSimObj *)ev->getCause())) {
    _objs[0]->flags = PENDING_NONE;
  }
  /* 
     I just set this node to X and there is nothing pending;
     so check if there should be a X -> 0 or X -> 1 tarnsition
     pending that cleans up this X value
  */
  if (t == 2 /* X */ && _objs[0]->flags == PENDING_NONE) {
    int u_state, d_state, u_weak, d_weak;
    int u_idx, d_idx;
    // compute u_state, d_state, u_weak, d_weak

    u_weak = 0;
    u_state = 0;
    u_idx = 0;
    for (int i=0; i < _count; i++) {
      u_state = _objs[i]->eval (_objs[i]->_me->up[PRSSIM_NORM], causeid, causeid == -1 ? NULL : &lid);
      if (u_state == 1) {
	u_idx = i;
	break;
      }
    }
    if (!u_state) {
      for (int i=0; i < _count; i++) {
	u_state = _objs[i]->eval (_objs[i]->_me->up[PRSSIM_WEAK], causeid, causeid == -1 ? NULL : &lid);
	if (u_state == 1) {
	  u_idx = i;
	  break;
	}
      }
      if (u_state) {
	u_weak = 1;
      }
    }

    d_weak = 0;
    d_state = 0;
    d_idx = 0;
    for (int i=0; i < _count; i++) {
      d_state = _objs[i]->eval (_objs[i]->_me->dn[PRSSIM_NORM], causeid, causeid == -1 ? NULL : &lid);
      if (d_state == 1) {
	d_idx = i;
	break;
      }
    }
    if (!d_state) {
      for (int i=0; i < _count; i++) {
	d_state = _objs[i]->eval (_objs[i]->_me->dn[PRSSIM_WEAK], causeid, causeid == -1 ? NULL : &lid);
	if (d_state == 1) {
	  d_idx = i;
	  break;
	}
      }
      if (d_state) {
	d_weak = 1;
      }
    }

    /* copied from propagate() */
    if (u_state == 0) {
      if (d_state == 1) {
	DO_SET_VAL (_objs[d_idx],_objs[0], _me->vid, lid, causeid, 0);
      }
    }
    else if (u_state == 1) {
      if (d_state == 0) {
	DO_SET_VAL (_objs[u_idx],_objs[0], _me->vid, lid, causeid, 1);
      }
      else if (d_state == 2 && (!u_weak && d_weak)) {
	DO_SET_VAL (_objs[u_idx],_objs[0], _me->vid, lid, causeid, 1);
      }
      else if (d_state == 1) {
	if (u_weak && !d_weak) {
	  DO_SET_VAL (_objs[d_idx],_objs[0],_me->vid, lid, causeid, 0);
	}
	else if (!u_weak && d_weak) {
	  DO_SET_VAL (_objs[u_idx],_objs[0], _me->vid, lid, causeid, 1);
	}
      }
    }
    else {
      /* u_state == 2 */
      if (d_state == 1) {
	if (u_weak && !d_weak) {
	  DO_SET_VAL (_objs[d_idx],_objs[0],_me->vid, lid, causeid, 0);
	}
      }
    }
  }
  return 1-_breakpt;
}

void MultiPrsSim::propagate (void *cause)
{
  int lid = -1;
  int causeid;

  if (cause) {
    ActSimDES *xx = (ActSimDES *) cause;
    causeid = xx->causeGlobalIdx ();
  }
  else {
    causeid = -1;
  }

  int u_state, d_state, u_weak, d_weak, u_idx, d_idx;
  // compute u_state, d_state, u_weak, d_weak

  u_weak = 0;
  u_state = 0;
  u_idx = 0;
  for (int i=0; i < _count; i++) {
    u_state = _objs[i]->eval (_objs[i]->_me->up[PRSSIM_NORM], causeid, causeid == -1 ? NULL : &lid);
    if (u_state == 1) {
      u_idx = i;
      break;
    }
  }
  if (!u_state) {
    for (int i=0; i < _count; i++) {
      u_state = _objs[i]->eval (_objs[i]->_me->up[PRSSIM_WEAK], causeid, causeid == -1 ? NULL : &lid);
      if (u_state == 1) {
	u_idx = i;
	break;
      }
    }
    if (u_state) {
      u_weak = 1;
    }
  }

  d_weak = 0;
  d_state = 0;
  d_idx = 0;
  for (int i=0; i < _count; i++) {
    d_state = _objs[i]->eval (_objs[i]->_me->dn[PRSSIM_NORM], causeid, causeid == -1 ? NULL : &lid);
    if (d_state == 1) {
      d_idx = i;
      break;
    }
  }
  if (!d_state) {
    for (int i=0; i < _count; i++) {
      d_state = _objs[i]->eval (_objs[i]->_me->dn[PRSSIM_WEAK], causeid, causeid == -1 ? NULL : &lid);
      if (d_state == 1) {
	d_idx = i;
	break;
      }
    }
    if (d_state) {
      d_weak = 1;
    }
  }
  
  /* -- check for unstable rules -- */
  if (_objs[0]->flags == PENDING_1 && u_state != 1) {
    if (u_state == 2) {
      if (!_objs[0]->_proc->isResetMode() && !_objs[0]->_me->unstab) {
	if (!_objs[0]->_proc->isHazard (_objs[0]->_me->vid)) {
	  WARNING_MSG (_objs[0], "weak-unstable transition", "+");
	}
      }
    }
    else {
      if (!_objs[0]->_me->unstab) {
	WARNING_MSG (_objs[0], "unstable transition", "+");
      }
    }
    MAKE_NODE_X (_objs[0],_me->vid);
  }
  if (_objs[0]->flags == PENDING_0 && d_state != 1) {
    if (d_state == 2) {
      if (!_objs[0]->_proc->isResetMode() && !_objs[0]->_me->unstab) {
	if (!_objs[0]->_proc->isHazard (_objs[0]->_me->vid)) {
	  WARNING_MSG (_objs[0], "weak-unstable transition", "-");
	}
      }
    }
    else {
      if (!_objs[0]->_me->unstab && !_objs[0]->_proc->isHazard (_objs[0]->_me->vid)) {
	WARNING_MSG (_objs[0], "unstable transition", "-");
      }
    }
    MAKE_NODE_X (_objs[0],_me->vid);
  }

  if (u_state == 0) {
    switch (d_state) {
    case 0:
      /* nothing to do */
      break;

    case 1:
      /* set to 0 */
      DO_SET_VAL (_objs[d_idx],_objs[0], _me->vid, lid, causeid, 0);
      break;
	
    case 2:
      if (_objs[0]->_proc->getBool (_objs[0]->_me->vid) == 1) {
	/* u = 0, d = X: if output=1, it is now X */
	MAKE_NODE_X (_objs[0],_me->vid);
      }
      break;
    }
  }
  else if (u_state == 1) {
    switch (d_state) {
    case 0:
      /* set to 1 */
      DO_SET_VAL (_objs[u_idx],_objs[0], _me->vid, lid, causeid, 1);
      break;

    case 2:
      if (!u_weak && d_weak) {
	DO_SET_VAL (_objs[u_idx],_objs[0], _me->vid, lid, causeid, 1);
      }
      else {
	if (!_objs[0]->_proc->isResetMode()) {
	  WARNING_MSG (_objs[0], "weak-interference", "");
	}
	MAKE_NODE_X (_objs[0],_me->vid);
      }
      break;

    case 1:
      /* interference */
      if (u_weak && !d_weak) {
	DO_SET_VAL (_objs[d_idx],_objs[0], _me->vid, lid, causeid, 0);
      }
      else if (!u_weak && d_weak) {
	DO_SET_VAL (_objs[u_idx], _objs[0], _me->vid, lid, causeid, 1);
      }
      else {
	WARNING_MSG (_objs[0], "interference", "");
	MAKE_NODE_X (_objs[0],_me->vid);
      }
      break;
    }
  }
  else {
    /* u_state == 2 */
    switch (d_state) {
    case 0:
      if (_objs[0]->_proc->getBool (_objs[0]->_me->vid) == 0) {
	MAKE_NODE_X (_objs[0],_me->vid);
      }
      break;

    case 1:
      if (u_weak && !d_weak) {
	/* set to 0 */
	DO_SET_VAL (_objs[d_idx],_objs[0], _me->vid, lid, causeid, 0);
      }
      else {
	if (!_objs[0]->_proc->isResetMode()) {
	  WARNING_MSG (_objs[0], "weak-interference", "");
	}
	MAKE_NODE_X (_objs[0],_me->vid);
      }
      break;

    case 2:
      /* set to X */
      if (!_objs[0]->_proc->isResetMode()) {
	WARNING_MSG (_objs[0], "weak-interference", "");
      }
      MAKE_NODE_X (_objs[0],_me->vid);
      break;
    }
  }
}

void MultiPrsSim::sPrintCause (char *buf, int sz)
{
  _objs[0]->sPrintCause (buf, sz);
}

int MultiPrsSim::causeGlobalIdx ()
{
  return _objs[0]->causeGlobalIdx ();
}
