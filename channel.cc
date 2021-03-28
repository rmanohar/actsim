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

static void _hse_record_ids (act_channel_state *ch,
			     ActSimCore *sc, ActSimObj *c,
			     ActId *ch_name,
			     ActId *id)
{
  /*-- self is a special case --*/
  if (!id->Rest() && (strcmp (id->getName(), "self") == 0)) {
    return;
  }
  
  act_connection *ac = id->Canonical (ch->ct->CurScope());
  ihash_bucket_t *b;

  b = ihash_lookup (ch->fH, (long)ac);
  if (!b) {
    int off;
    int type;
    
    b = ihash_add (ch->fH, (long)ac);

    ActId *tmp = ch_name;
    while (tmp->Rest()) {
      tmp = tmp->Rest();
    }
    tmp->Append (id);

    off = sc->getLocalOffset (tmp, sc->cursi(), &type);
    Assert (type == 0, "HSE in channel has non-boolean ops?");
    off = c->getGlobalOffset (off, 0);
    b->i = off;

    tmp->prune();
  }
}


static void _hse_record_ids (act_channel_state *ch,
			     ActSimCore *sc, ActSimObj *c,
			     ActId *ch_name,
			     Expr *e)
{
  if (!e) return;
  switch (e->type) {
  case E_TRUE:
  case E_FALSE:
  case E_INT:
  case E_REAL:
    break;

    /* binary */
  case E_AND:
  case E_OR:
  case E_PLUS:
  case E_MINUS:
  case E_MULT:
  case E_DIV:
  case E_MOD:
  case E_LSL:
  case E_LSR:
  case E_ASR:
  case E_XOR:
  case E_LT:
  case E_GT:
  case E_LE:
  case E_GE:
  case E_EQ:
  case E_NE:
    _hse_record_ids (ch, sc, c, ch_name, e->u.e.l);
    _hse_record_ids (ch, sc, c, ch_name, e->u.e.r);
    break;
    
  case E_UMINUS:
  case E_COMPLEMENT:
  case E_NOT:
    _hse_record_ids (ch, sc, c, ch_name, e->u.e.l);
    break;

  case E_QUERY:
    _hse_record_ids (ch, sc, c, ch_name, e->u.e.l);
    _hse_record_ids (ch, sc, c, ch_name, e->u.e.r->u.e.l);
    _hse_record_ids (ch, sc, c, ch_name, e->u.e.r->u.e.r);
    break;

  case E_VAR:
    _hse_record_ids (ch, sc, c, ch_name, (ActId *)e->u.e.l);
    break;

  case E_BUILTIN_BOOL:
  case E_BUILTIN_INT:
    _hse_record_ids (ch, sc, c, ch_name, e->u.e.l);
    break;
    
  case E_SELF:
    break;

  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
}

static void _hse_record_ids (act_channel_state *chs,
			     ActSimCore *sc, ActSimObj *c,
			     ActId *ch_name,
			     act_chp_lang_t *ch)
{
  listitem_t *li;
  
  if (!ch) return;
  
  switch (ch->type) {
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (li = list_first (ch->u.semi_comma.cmd); li; li = list_next (li)) {
      _hse_record_ids (chs, sc, c, ch_name, (act_chp_lang_t *) list_value (li));
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_DOLOOP:
  case ACT_CHP_LOOP:
    for (act_chp_gc_t *gc = ch->u.gc; gc; gc = gc->next) {
      _hse_record_ids (chs, sc, c, ch_name, gc->s);
      _hse_record_ids (chs, sc, c, ch_name, gc->g);
    }
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
    fatal_error ("HSE cannot use send/receive or log!");
    break;
    
  case ACT_CHP_FUNC:
    break;
    
  case ACT_CHP_ASSIGN:
    _hse_record_ids (chs, sc, c, ch_name, ch->u.assign.id);
    _hse_record_ids (chs, sc, c, ch_name, ch->u.assign.e);
    break;

  default:
    fatal_error ("Unknown chp type %d\n", ch->type);
    break;
  }
}

void sim_recordChannel (ActSimCore *sc, ActSimObj *c, ActId *id)
{
  int loff;

  /* find the channel state pointer */
  loff = sc->getLocalOffset (id, sc->cursi(), NULL);
  loff = c->getGlobalOffset (loff, 2);
  act_channel_state *ch = sc->getChan (loff);

  InstType *it = sc->cursi()->bnl->cur->FullLookup (id, NULL);
  Channel *ct = dynamic_cast <Channel *> (it->BaseType());

  if (ct) {
    /* now find each boolean, and record it in the channel state! */
    if (!ch->fH) {
      ch->fH = ihash_new (4);
      ch->ct = ct;
    }
    for (int i=0; i < ACT_NUM_STD_METHODS; i++) {
      act_chp_lang_t *x = ct->getMethod (i);
      if (x) {
	_hse_record_ids (ch, sc, c, id, x);
      }
    }
    for (int i=0; i < ACT_NUM_EXPR_METHODS; i++) {
      Expr *e = ct->geteMethod (i+ACT_NUM_STD_METHODS);
      if (e) {
	_hse_record_ids (ch, sc, c, id, e);
      }
    }
  }
}

			     
