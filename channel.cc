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
      ch->inst_id = id->Clone();
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
}

			     
ChanMethods::ChanMethods (Channel *c)
{
  _ch = c;
  _dummy = NULL;
  for (int i=0; i < ACT_NUM_STD_METHODS; i++) {
    A_INIT (_ops[i].op);
    _compile (i, c->getMethod (i));
  }
}

ChanMethods::~ChanMethods ()
{
  for (int i=0; i < ACT_NUM_STD_METHODS; i++) {
    A_FREE (_ops[i].op);
  }
}


void ChanMethods::_compile (int idx, act_chp_lang *hse)
{
  if (!hse) {
    return;
  }
  switch (hse->type) {
  case ACT_CHP_SKIP:
    A_NEW (_ops[idx].op, one_chan_op);
    A_NEXT (_ops[idx].op).type = CHAN_OP_SKIP;
    A_INC (_ops[idx].op);
    break;
    
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (listitem_t *li = list_first (hse->u.semi_comma.cmd); li;
	 li = list_next (li)) {
      _compile (idx, (act_chp_lang *)list_value (li));
    }
    break;
    
  case ACT_CHP_ASSIGN:
  case ACT_CHP_ASSIGNSELF:
    A_NEW (_ops[idx].op, one_chan_op);
    if (strcmp (hse->u.assign.id->getName(), "self") == 0) {
      A_NEXT (_ops[idx].op).type = CHAN_OP_SELF;
      A_NEXT (_ops[idx].op).e = hse->u.assign.e;
    }
    else {
      if (hse->u.assign.e->type == E_TRUE) {
	A_NEXT (_ops[idx].op).type = CHAN_OP_BOOL_T;
      }
      else if (hse->u.assign.e->type ==  E_FALSE) {
	A_NEXT (_ops[idx].op).type = CHAN_OP_BOOL_F;
      }
      else {
	fatal_error ("%s: Assignment expr error? %d\n", _ch->getName(),
		     hse->u.assign.e->type);
      }
      A_NEXT (_ops[idx].op).var = hse->u.assign.id;
    }
    A_INC (_ops[idx].op);
    break;
    
  case ACT_CHP_SELECT:
    {
      int root_idx, tmp;
      act_chp_gc_t *gc = hse->u.gc;
      root_idx = A_LEN (_ops[idx].op);
      while (gc) {
	int pos;
	A_NEW (_ops[idx].op, one_chan_op);
	pos = A_LEN (_ops[idx].op);
	A_NEXT (_ops[idx].op).type = CHAN_OP_SEL;
	if (gc->g) {
	  A_NEXT (_ops[idx].op).e = gc->g;
	}
	else {
	  A_NEXT (_ops[idx].op).e = const_expr_bool (1);
	}
	A_INC (_ops[idx].op);
	_compile (idx, gc->s);
	A_NEW (_ops[idx].op, one_chan_op);
	A_NEXT (_ops[idx].op).type = CHAN_OP_GOTO;
	A_INC (_ops[idx].op);
	if (gc->next) {
	  _ops[idx].op[pos].idx = A_LEN (_ops[idx].op);
	}
	else {
	  _ops[idx].op[pos].idx = root_idx;
	}
	gc = gc->next;
      }
      Assert (A_LAST (_ops[idx].op).type == CHAN_OP_GOTO, "What?");
      A_LAST (_ops[idx].op).idx = A_LEN (_ops[idx].op);
      A_NEW (_ops[idx].op, one_chan_op);
      A_NEXT (_ops[idx].op).type = CHAN_OP_SKIP;
      A_INC (_ops[idx].op);
      tmp = root_idx;
      do {
	Assert (_ops[idx].op[tmp].type == CHAN_OP_SEL, "What?");
	tmp = _ops[idx].op[tmp].idx;
	if (tmp != root_idx) {
	  Assert (_ops[idx].op[tmp-1].type == CHAN_OP_GOTO, "What?!");
	  _ops[idx].op[tmp-1].idx = A_LEN (_ops[idx].op)-1;
	}
      } while (tmp != root_idx);
    }
    break;
    
  case ACT_CHP_LOOP:
  case ACT_CHP_DOLOOP:
    warning ("%s: Internal loop not working yet...", _ch->getName());
    break;


  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
  case ACT_CHP_FUNC:
  case ACT_CHP_SEMILOOP:
  case ACT_CHP_COMMALOOP:
  case ACT_CHP_HOLE:
  case ACT_CHP_MACRO:
  case ACT_HSE_FRAGMENTS:
  default:
    warning ("%s: Ignoring unsupported construct in channel methods: %d\n",
	     _ch->getName(), hse->type);
    break;
  }
}


int ChanMethods::runProbe (ActSimCore *sim,
			   act_channel_state *ch,
			   int idx)
{
  if (!_dummy) {
    _dummy = new ChpSim (NULL, 0, NULL, sim, NULL);
    _dummy->setFrag (ch);
  }
  
  Expr *e = ch->ct->geteMethod (idx);
  if (!e) {
    warning ("%s: requested probe is not defined.", ch->ct->getName());
    fprintf (stderr, " Instance: ");
    ch->inst_id->Print (stderr);
    fprintf (stderr, "\n");
    return 0;
  }
  expr_res r = _dummy->exprEval (e);
  if (r.v) {
    return 1;
  }
  else {
    return 0;
  }
}


int ChanMethods::runMethod (ActSimCore *sim,
			    act_channel_state *ch,
			    int idx,
			    int from)
{
  ihash_bucket_t *b;
  act_connection *c;
  int v;
  expr_res r;
  
  if (!_dummy) {
    _dummy = new ChpSim (NULL, 0, NULL, sim, NULL);
    _dummy->setFrag (ch);
  }

  while (from < A_LEN (_ops[idx].op)) {
    switch (_ops[idx].op[from].type) {
    case CHAN_OP_SKIP:
      from++;
      break;

    case CHAN_OP_GOTO:
      from = _ops[idx].op[from].idx;
      break;

    case CHAN_OP_BOOL_T:
    case CHAN_OP_BOOL_F:
      c = _ops[idx].op[from].var->Canonical (ch->ct->CurScope ());
      b = ihash_lookup (ch->fH, (long)c);
      if (!b) {
	fatal_error ("%s: Internal error running method %d", ch->ct->getName(),
		     idx);
      }
      v = _dummy->getBool (b->i);
      if (_ops[idx].op[from].type == CHAN_OP_BOOL_T) {
	if (v != 1) {
	  _dummy->setBool (b->i, 1);
	  v = -1;
	}
      }
      else {
	if (v != 0) {
	  _dummy->setBool (b->i, 0);
	  v = -1;
	}
      }
      if (v == -1) {
	if (_dummy->isWatched (0, b->i)) {
	  ActId *tmpid;
	  _dummy->msgPrefix ();
	  tmpid = c->toid();
	  tmpid->Print (stdout);
	  delete tmpid;
	  printf (" := %c\n", _ops[idx].op[from].type == CHAN_OP_BOOL_T ?
		  '1' : '0');
	}
	_dummy->boolProp (b->i);
      }
      from++;
      break;

    case CHAN_OP_SELF:
      /* expression evaluation!!! */
      r = _dummy->exprEval (_ops[idx].op[from].e);
      ch->data.setSingle (r);
      from++;
      break;

    case CHAN_OP_SEL:
      /* expression evaluation! */
      r = _dummy->exprEval (_ops[idx].op[from].e);
      if (r.v) {
	from++;
      }
      else {
	if (_ops[idx].op[from].idx <= from) {
	  return _ops[idx].op[from].idx;
	}
	else {
	  from = _ops[idx].op[from].idx;
	}
      }
      break;

    default:
      fatal_error ("What?!");
      break;
    }
  }
  /* done! */
  return -1;
}

