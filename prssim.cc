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
#include <simdes.h>
#include "prssim.h"
#include "qops.h"

PrsSim::PrsSim (PrsSimGraph *g, ActSimCore *sim)
: ActSimObj (sim)
{
  
}

void PrsSim::Step (int ev_type)
{

}

void PrsSim::computeFanout ()
{
  
  
}

static int _attr_check_weak (act_attr_t *attr)
{
  while (attr) {
    if (strcmp (attr->attr, "weak") == 0) {
      Assert (attr->e->type == E_INT, "What?");
      if (attr->e->u.v) {
	return 1;
      }
      else {
	return 0;
      }
    }
  }
  return 0;
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

  if (p->u.one.label) {
    hash_bucket_t *b = hash_add (at_table, (char *)p->u.one.id);
    b->v = p;
    return;
  }

  rhs = sc->getLocalOffset (p->u.one.id, sc->cursi(), NULL);
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
    s->up[0] = NULL;
    s->up[1] = NULL;
    s->dn[0] = NULL;
    s->dn[1] = NULL;
    q_ins (_rules, _tail, s);
  }

  int weak;
  if (_attr_check_weak (p->u.one.attr)) {
    weak = PRSSIM_WEAK;
  }
  else {
    weak = PRSSIM_NORM;
  }

  /*-- now handle the rule --*/

  at_table = _labels;
  switch (p->u.one.arrow_type) {
  case 0:
    /* normal arrow */
    if (p->u.one.dir) {
      _merge_prs (sc, &s->up[weak], p->u.one.e, 0);
    }
    else {
      _merge_prs (sc, &s->dn[weak], p->u.one.e, 0);
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


PrsSimGraph *PrsSimGraph::buildPrsSimGraph (ActSimCore *sc, act_prs *p, act_spec *spec)
{
  PrsSimGraph *pg;

  pg = new PrsSimGraph ();
  
  while (p) {
    pg->addPrs (sc, p->p);
    p = p->next;
  }
  return pg;
}
