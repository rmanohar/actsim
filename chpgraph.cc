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
#include <stdio.h>
#include "chpsim.h"

int ChpSimGraph::cur_pending_count = 0;
int ChpSimGraph::max_pending_count = 0;
int ChpSimGraph::max_stats = 0;
struct Hashtable *ChpSimGraph::labels = NULL;


/*
 *
 *  Routines to build the CHP simulation graph from the CHP data
 *  structures
 *
 */

ChpSimGraph::ChpSimGraph (ActSimCore *s)
{
  state = s;
  stmt = NULL;
  next = NULL;
  all = NULL;
  wait = 0;
  totidx = 0;
#if 0  
  printf ("alloc %p\n", this);
#endif  
}


void ChpSimGraph::printStmt (FILE *fp, Process *p)
{
  act_connection *c;
  int dy;

  if (!stmt) {
    fprintf (fp, "(null)");
    return;
  }
  
  switch (stmt->type) {
  case CHPSIM_FORK:
    fprintf (fp, "concur-fork:");
    for (int i=0; i < stmt->u.fork; i++) {
      if (all[i]) {
	fprintf (fp, " ");
	all[i]->printStmt (fp, p);
      }
    }
    break;

  case CHPSIM_ASSIGN:
    fprintf (fp, "assign: ");
    if (!p) break;
    c = state->getConnFromOffset (p, stmt->u.assign.d.offset,
				  (stmt->u.assign.isint ? 1 : 0),
				  &dy);
    if (c) {
      ActId *t = c->toid();
      t->Print (fp);
      delete t;
      if (dy != -1) { fprintf (fp, "[]"); }
    }
    else {
      fprintf (fp, "-?-");
    }
    break;

  case CHPSIM_SEND:
    fprintf (fp, "send: ");
    if (!p) break;
    c = state->getConnFromOffset (p, stmt->u.sendrecv.chvar, 3, &dy);
    if (c) {
      ActId *t = c->toid();
      t->Print (fp);
      delete t;
      if (dy != -1) { fprintf (fp, "[]"); }
    }
    else {
      fprintf (fp, "-?-");
    }
    break;

  case CHPSIM_RECV:
    fprintf (fp, "recv: ");
    if (!p) break;
    c = state->getConnFromOffset (p, stmt->u.sendrecv.chvar, 2, &dy);
    if (c) {
      ActId *t = c->toid();
      t->Print (fp);
      delete t;
      if (dy != -1) { fprintf (fp, "[]"); }
    }
    else {
      fprintf (fp, "-?-");
    }
    break;


  case CHPSIM_FUNC:
    fprintf (fp, "func");
    break;

  case CHPSIM_COND:
    fprintf (fp, "cond: ");
    break;

  case CHPSIM_CONDARB:
    fprintf (fp, "cond-arb: ");
    break;

  case CHPSIM_LOOP:
    fprintf (fp, "loop: ");
    break;

  case CHPSIM_NOP:
    fprintf (fp, "nop ");
    break;

  default:
    fatal_error ("What?");
    break;
  }
}


ChpSimGraph *ChpSimGraph::completed (int pc, int *tot, int *done)
{
  *done = 0;
  if (!next || (stmt && (stmt->type == CHPSIM_COND || stmt->type == CHPSIM_CONDARB))) {
    return NULL;
  }
  if (next->wait > 0) {
#ifdef DUMP_ALL
    printf (" [wt=%d cur=%d{%d}]", next->wait, tot[next->totidx], next->totidx);
#endif
    tot[next->totidx]++;
    if (next->wait == tot[next->totidx]) {
      *done = 1;
      tot[next->totidx] = 0;
      return next;
    }
    else {
      return NULL;
    }
  }
  else {
    return next;
  }
}


static Expr *expr_to_chp_expr (Expr *e, ActSimCore *s, int *flags);
static void _free_chp_expr (Expr *e);

static void _free_deref (struct chpsimderef *d)
{
  if (d->range) {
    /* chp_idx is NULL if this is an actual array */
    if (d->chp_idx) {
      for (int i=0; i < d->range->nDims(); i++) {
	_free_chp_expr (d->chp_idx[i]);
      }
      FREE (d->chp_idx);
    }
    FREE (d->idx);
  }
  else {
    if (d->idx) {
      FREE (d->idx);
    }
  }
}

static struct chpsimderef *_mk_deref (ActId *id, ActSimCore *s, int *type,
				      int *width = NULL)
{
  struct chpsimderef *d;
  Scope *sc = s->cursi()->bnl->cur;
  int intype;

  ValueIdx *vx = id->rootVx (sc);
  Assert (vx->connection(), "What?");

  NEW (d, struct chpsimderef);
  d->stride = 1;
  d->idx = NULL;
  d->d = NULL;
  d->cx = vx->connection();

  /*
    If the base is a structure, and we are de-referencing a piece of
    it, then we need the offset *into* the structure itself
  */

  intype = *type;

  d->offset = s->getLocalOffset (vx->connection()->primary(),
				 s->cursi(), type, &d->width);

  if (intype != -1 && TypeFactory::isStructure (vx->t)) {

#if 0
    printf ("hi, I'm here, intype=%d\n", intype);
    printf ("id: "); id->Print (stdout);
    printf ("\n");
#endif

    Data *dt = dynamic_cast<Data *> (vx->t->BaseType());
    int i_off, b_off;
    if (dt->getStructOffsetPair (id->Rest(), &b_off, &i_off) == 0) {
      warning ("Illegal offset; how did this happen?");
      i_off = 0;
      b_off = 0;
    }

#if 0
    printf ("i-off: %d; b-off: %d\n", i_off, b_off);
#endif

    if (intype == 0) {
      d->offset += b_off;
      *type = 0;
    }
    else {
      d->offset += i_off;
      *type = 1;
      d->width = intype;
    }
    dt->getStructCount (&b_off, &i_off);
    if (intype == 0) {
      d->stride = b_off;
    }
    else {
      d->stride = i_off;
    }
#if 0
    printf ("offset: %d; stride: %d\n", d->offset, d->stride);
#endif
  }

  if (width) {
    *width = d->width;
  }

  if (*type == 0) {
    d->isbool = 1;
  }
  else {
    d->isbool = 0;
  }
  d->isenum = 0;
  
  d->range = vx->t->arrayInfo();
  Assert (d->range, "What?");
  Assert (d->range->nDims() > 0, "What?");
  MALLOC (d->idx, int, d->range->nDims());
  MALLOC (d->chp_idx, Expr *, d->range->nDims());
  
  /* now convert array deref into a chp array deref! */
  for (int i = 0; i < d->range->nDims(); i++) {
    int flags = 0;
    d->chp_idx[i] = expr_to_chp_expr (id->arrayInfo()->getDeref(i), s, &flags);
    d->idx[i] = -1;
  }

  return d;
}


static struct chpsimderef *
_mk_deref_struct (ActId *id, ActSimCore *s)
{
  struct chpsimderef *d = NULL;
  Scope *sc = s->cursi()->bnl->cur;

  ValueIdx *vx = id->rootVx (sc);
  Assert (vx->connection(), "What?");

  NEW (d, struct chpsimderef);
  d->stride = 1;
  d->cx = vx->connection();
  d->d = dynamic_cast<Data *> (vx->t->BaseType());
  d->isbool = 0;
  d->isenum = 0;
  Assert (d->d, "what?");
  if (!s->getLocalDynamicStructOffset (vx->connection()->primary(),
				       s->cursi(),
				       &d->offset, &d->width)) {
    fatal_error ("Structure derefence generation failed!");
    return NULL;
  }
    
  d->range = vx->t->arrayInfo();
  Assert (d->range, "What?");
  Assert (d->range->nDims() > 0, "What?");
  MALLOC (d->idx, int, d->range->nDims());
  MALLOC (d->chp_idx, Expr *, d->range->nDims());
  
  /* now convert array deref into a chp array deref! */
  for (int i = 0; i < d->range->nDims(); i++) {
    int flags = 0;
    d->chp_idx[i] = expr_to_chp_expr (id->arrayInfo()->getDeref(i), s, &flags);
    d->idx[i] = -1;
  }
  
  return d;
}

static void _add_deref_struct (ActSimCore *sc,
			       ActId *id, Data *d,
			       struct chpsimderef *ds)
{
  ActId *tmp, *tail;
	    
  tail = id->Tail();
  for (int i=0; i < d->getNumPorts(); i++) {
    InstType *it = d->getPortType (i);
    tmp = new ActId (d->getPortName(i));
    tail->Append (tmp);

    if (it->arrayInfo()) {
      Arraystep *as = it->arrayInfo()->stepper();
      while (!as->isend()) {
	Array *a = as->toArray();
	tmp->setArray (a);

	if (TypeFactory::isStructure (it)) {
	  Data *x = dynamic_cast<Data *> (it->BaseType());
	  Assert (x, "What?");
	  _add_deref_struct (sc, id, x, ds);
	}
	else {
	  ds->idx[ds->offset] = sc->getLocalOffset (id, sc->cursi(),
						    &ds->idx[ds->offset+1],
						    &ds->idx[ds->offset+2]);
	  if (TypeFactory::isEnum (it)) {
	    ds->idx[ds->offset+1] = 2;
	    ds->idx[ds->offset+2] = TypeFactory::enumNum (it);
	  }
	  ds->offset += 3;
	}
	delete a;
	as->step();
      }
      tmp->setArray (NULL);
    }
    else {
      if (TypeFactory::isStructure (it)) {
	Data *x = dynamic_cast<Data *> (it->BaseType());
	Assert (x, "What?");
	_add_deref_struct (sc, id, x, ds);
      }
      else {
	ds->idx[ds->offset] = sc->getLocalOffset (id, sc->cursi(),
						  &ds->idx[ds->offset+1],
						  &ds->idx[ds->offset+2]);
	if (TypeFactory::isEnum (it)) {
	  ds->idx[ds->offset+1] = 2;
	  ds->idx[ds->offset+2] = TypeFactory::enumNum (it);
	}
	ds->offset += 3;
      }
    }
    tail->prune ();
    delete tmp;
  }
}

static struct chpsimderef *
_mk_std_deref_struct (ActId *id, InstType *it, ActSimCore *s)
{
  struct chpsimderef *ds = NULL;
  bool is_array = false;
  Scope *sc = s->cursi()->bnl->cur;
  Data *d = dynamic_cast <Data *> (it->BaseType());
  Assert (d, "Hmm");

  if (it->arrayInfo() && !id->Tail()->isDeref()) {
    is_array = true;
  }

  NEW (ds, struct chpsimderef);
  ds->stride = 1;
  ds->range = NULL;
  ds->chp_idx = NULL;
  ds->idx = NULL;
  ds->d = d;
  ds->isbool = 0;
  ds->isenum = 0;

  state_counts ts;
  ActStatePass::getStructCount (d, &ts);

  int array_sz;

  if (is_array) {
    array_sz = it->arrayInfo()->size();
  }
  else {
    array_sz = 1;
  }

  ds->offset = 0;
  MALLOC (ds->idx, int, 3*array_sz*(ts.numInts() + ts.numBools()));

  if (!is_array) {
    _add_deref_struct (s, id, d, ds);
  }
  else {
    ActId *tail = id->Tail ();
    Assert (tail->arrayInfo() == NULL, "What?");
    for (int i=0; i < array_sz; i++) {
      Array *a = it->arrayInfo()->unOffset (i);
      tail->setArray (a);
      _add_deref_struct (s, id, d, ds);
      delete a;
    }
    tail->setArray (NULL);
  }

  Assert (ds->offset == 3*array_sz*(ts.numInts() + ts.numBools()), "What?");
  
  ds->cx = id->Canonical (sc);
  return ds;
}

static struct chpsimderef *
_mk_std_deref (ActId *id, InstType *it, ActSimCore *s)
{
  struct chpsimderef *d = NULL;
  Scope *sc = s->cursi()->bnl->cur;
  bool is_array = false;

  if (!it) {
    it = sc->FullLookup (id, NULL);
  }
  Assert (it, "Hmm");

  if (it->arrayInfo() && !id->Tail()->isDeref()) {
    is_array = true;
  }

  NEW (d, struct chpsimderef);
  d->stride = 1;
  d->range = NULL;
  d->chp_idx = NULL;
  d->idx = NULL;
  d->d = NULL;
  d->isbool = 0;
  d->isenum = 0;

  int type;
  if (is_array) {
    d->range = it->arrayInfo();
    MALLOC (d->idx, int, d->range->size());
    ActId *tmp = id->Tail ();
    Assert (!tmp->arrayInfo(), "What?");
    for (int i=0; i < d->range->size(); i++) {
      Array *a = d->range->unOffset (i);
      int t, width;
      tmp->setArray (a);
      d->idx[i] = s->getLocalOffset (id, s->cursi(), &t, &width);
      if (i == 0) {
	d->width = width;
	type = t;
      }
      else {
	Assert (t == type && width == d->width, "What?");
      }
      delete a;
    }
    tmp->setArray (NULL);
    d->offset = 0;
  }
  else {
    d->offset = s->getLocalOffset (id, s->cursi(), &type, &d->width);
  }
  if (type == 1) {
    if (TypeFactory::isEnum (it)) {
      d->isenum = 1;
      d->enum_sz = TypeFactory::enumNum (it);
    }
  }
  else {
    Assert (type == 0, "What?");
    d->isbool = 1;
  }
  d->cx = id->Canonical (sc);
  return d;
}

/*------------------------------------------------------------------------
 * The CHP simulation graph
 *------------------------------------------------------------------------
 */
static void _free_chp_expr (Expr *e)
{
  if (!e) return;
  switch (e->type) {
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
    _free_chp_expr (e->u.e.l);
    _free_chp_expr (e->u.e.r);
    break;
    
  case E_NOT:
  case E_UMINUS:
  case E_COMPLEMENT:
    _free_chp_expr (e->u.e.l);
    break;

  case E_QUERY:
    _free_chp_expr (e->u.e.l);
    _free_chp_expr (e->u.e.r->u.e.l);
    _free_chp_expr (e->u.e.r->u.e.r);
    FREE (e->u.e.r);
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    _free_chp_expr (e->u.e.l);
    _free_chp_expr (e->u.e.r);
    break;

  case E_CHP_BITFIELD:
    _free_chp_expr (e->u.e.r->u.e.l);
    _free_chp_expr (e->u.e.r->u.e.r);
    FREE (e->u.e.r);
    {
      struct chpsimderef *d;
      d = (struct chpsimderef *)e->u.e.l;
      _free_deref (d);
      FREE (d);
    }
    break;

  case E_TRUE:
  case E_FALSE:
  case E_INT:
    return;
    break;

  case E_REAL:
    break;

  case E_CHP_VARBOOL_DEREF:
  case E_CHP_VARINT_DEREF:
  case E_CHP_VARSTRUCT:
  case E_CHP_VARSTRUCT_DEREF:
    {
      struct chpsimderef *d = (struct chpsimderef *)e->u.e.l;
      _free_deref (d);
      FREE (d);
    }
    break;

  case E_CHP_VARCHAN:
  case E_CHP_VARINT:
  case E_CHP_VARBOOL:
    break;
    
  case E_PROBEIN:
  case E_PROBEOUT:
    break;
    
  case E_FUNCTION:
    {
      Expr *tmp = NULL;
      tmp = e->u.fn.r;
      while (tmp) {
	Expr *x;
	_free_chp_expr (tmp->u.e.l);
	x = tmp->u.e.r;
	FREE (tmp);
	tmp = x;
      }
    }
    break;

  case E_BUILTIN_INT:
  case E_BUILTIN_BOOL:
    _free_chp_expr (e->u.e.l);
    _free_chp_expr (e->u.e.r);
    break;



  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  FREE (e);
}

/*
 * Record static bitwidths for -, ~, and concat for  function
 * expressions. 
 */
static void fn_expr_bitwidth (Scope *sc, Expr *e, ActSimCore *s)
{
  if (!e) return;
  switch (e->type) {
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
    fn_expr_bitwidth (sc, e->u.e.l, s);
    fn_expr_bitwidth (sc, e->u.e.r, s);
    if (e->type == E_MINUS) {
      phash_bucket_t *b = s->exprWidth (e);
      if (!b) {
	int width;
	b = s->exprAddWidth (e);
	act_type_expr (sc, e, &width, 2);
	b->i = width;
      }
    }
    break;
    
  case E_NOT:
  case E_UMINUS:
  case E_COMPLEMENT:
    fn_expr_bitwidth (sc, e->u.e.l, s);
    {
      phash_bucket_t *b;
      int width;
      act_type_expr (sc, e->u.e.l, &width, 2);
      b = s->exprWidth (e->u.e.l);
      if (!b) {
	b = s->exprAddWidth (e->u.e.l);
	b->i = width;
      }
    }
    break;

  case E_QUERY:
    fn_expr_bitwidth (sc, e->u.e.l, s);
    fn_expr_bitwidth (sc, e->u.e.r->u.e.l, s);
    fn_expr_bitwidth (sc, e->u.e.r->u.e.r, s);
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    do {
      phash_bucket_t *b;
      fn_expr_bitwidth (sc, e->u.e.l, s);
      b = s->exprWidth (e->u.e.l);
      if (!b) {
	int width;
	b = s->exprAddWidth (e->u.e.l);
	act_type_expr (sc, e->u.e.l, &width, 2);
	b->i = width;
      }
      e = e->u.e.r;
    } while (e);
    break;

  case E_BITFIELD:
  case E_TRUE:
  case E_FALSE:
  case E_INT:
  case E_REAL:
  case E_VAR:
  case E_PROBE:
    break;
    
  case E_FUNCTION:
    {
      e = e->u.fn.r;
      while (e) {
	fn_expr_bitwidth (sc, e->u.e.l, s);
	e = e->u.e.r;
      }
    }
    break;

  case E_BUILTIN_INT:
  case E_BUILTIN_BOOL:
    fn_expr_bitwidth (sc, e->u.e.l, s);
    fn_expr_bitwidth (sc, e->u.e.r, s);
    break;

  case E_SELF:
  case E_SELF_ACK:
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
}

static void process_func_body_exprs (Function *f, act_chp_lang_t *c, ActSimCore *s)
{
  act_chp_gc_t *gc;
  
  if (!c) return;

  switch (c->type) {
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (listitem_t *li = list_first (c->u.semi_comma.cmd); li; li = list_next (li)) {
      process_func_body_exprs (f, (act_chp_lang_t *) list_value (li), s);
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
  case ACT_CHP_DOLOOP:
    gc = c->u.gc;
    if (gc->id) {
      fatal_error ("No replication in functions!");
    }
    while (gc) {
      fn_expr_bitwidth (f->CurScope(), gc->g, s);
      process_func_body_exprs (f, gc->s, s);
      gc = gc->next;
    }
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
    fatal_error ("Functions cannot use send/receive");
    break;
    
  case ACT_CHP_FUNC:
    if (strcmp (string_char (c->u.func.name), "log") == 0 ||
	strcmp (string_char (c->u.func.name), "log_p") == 0 ||
	strcmp (string_char (c->u.func.name), "warn") == 0 ||
	strcmp (string_char (c->u.func.name), "assert") == 0) {
      listitem_t *li;
      for (li = list_first (c->u.func.rhs); li; li = list_next (li)) {
	act_func_arguments_t *tmp = (act_func_arguments_t *)list_value (li);
	if (!tmp->isstring) {
	  fn_expr_bitwidth (f->CurScope(), tmp->u.e, s);
	}
      }
    }
    break;
    
  case ACT_CHP_ASSIGN:
    fn_expr_bitwidth (f->CurScope(), c->u.assign.e, s);
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
}



/*
 * flags: 0x1 set if probe exists
 *        0x2 set if shared variable exists
 */
static Expr *expr_to_chp_expr (Expr *e, ActSimCore *s, int *flags)
{
  Expr *ret;
  if (!e) return NULL;
  NEW (ret, Expr);
  ret->type = e->type;
  switch (e->type) {
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
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s, flags);
    ret->u.e.r = expr_to_chp_expr (e->u.e.r, s, flags);
    if (e->type == E_MINUS) {
      phash_bucket_t *b = s->exprWidth (ret);
      if (!b) {
	int width;
	b = s->exprAddWidth (ret);
	act_type_expr (s->CurScope(), e, &width, 2);
	b->i = width;
      }
    }
    break;
    
  case E_NOT:
  case E_UMINUS:
  case E_COMPLEMENT:
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s, flags);
    {
      phash_bucket_t *b;
      int width;
      act_type_expr (s->CurScope(), e->u.e.l, &width, 2);
      b = s->exprAddWidth (ret->u.e.l);
      b->i = width;
    }
    break;

  case E_QUERY:
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s, flags);
    NEW (ret->u.e.r, Expr);
    ret->u.e.r->type = e->u.e.r->type;
    ret->u.e.r->u.e.l = expr_to_chp_expr (e->u.e.r->u.e.l, s, flags);
    ret->u.e.r->u.e.r = expr_to_chp_expr (e->u.e.r->u.e.r, s, flags);
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    {
      Expr *tmp;
      tmp = ret;
      do {
	phash_bucket_t *b;
	tmp->u.e.l = expr_to_chp_expr (e->u.e.l, s, flags);
	b = s->exprWidth (tmp->u.e.l);
	if (!b) {
	  int width;
	  b = s->exprAddWidth (tmp->u.e.l);
	  act_type_expr (s->CurScope(), e->u.e.l, &width, 2);
	  b->i = width;
	}
	if (e->u.e.r) {
	  NEW (tmp->u.e.r, Expr);
	  tmp->u.e.r->type = e->u.e.r->type;
	}
	else {
	  tmp->u.e.r = NULL;
	}
	tmp = tmp->u.e.r;
	e = e->u.e.r;
      } while (e);
    }
    break;

  case E_BITFIELD:
    {
      int type;
      struct chpsimderef *d;

      if (ActBooleanizePass::isDynamicRef (s->cursi()->bnl,
					   ((ActId *)e->u.e.l))) {
	type = 1;
	d = _mk_deref ((ActId *)e->u.e.l, s, &type);
      }
      else {
	InstType *xit;
	act_type_var (s->cursi()->bnl->cur, (ActId *)e->u.e.l, &xit);
	Assert (xit, "Hmm");
	d = _mk_std_deref ((ActId *)e->u.e.l, xit, s);
      }
      
      ret->u.e.l = (Expr *) d;
      NEW (ret->u.e.r, Expr);
      ret->u.e.r->u.e.l = e->u.e.r->u.e.l;
      ret->u.e.r->u.e.r = e->u.e.r->u.e.r;
      ret->type = E_CHP_BITFIELD;
    }
    break;

  case E_TRUE:
  case E_FALSE:
  case E_INT:
    FREE (ret);
    ret = e;
    break;

  case E_REAL:
    ret->u.f = e->u.f;
    break;

  case E_VAR:
    {
      int type = -1;

      InstType *it = s->cursi()->bnl->cur->FullLookup ((ActId *)e->u.e.l,
						       NULL);
      
      if (ActBooleanizePass::isDynamicRef (s->cursi()->bnl,
					   ((ActId *)e->u.e.l))) {

	// XXX: check for arrays here!!!
	if (it->arrayInfo() && !((ActId *)e->u.e.l)->isDeref()) {
	  Assert (0, "entire array that is dynamic!");
	}
	
	if (TypeFactory::isStructure (it)) {
	  struct chpsimderef *ds = _mk_deref_struct ((ActId *)e->u.e.l, s);
	  ret->u.e.l = (Expr *) ds;
	  ret->u.e.r = (Expr *) ds->cx;
	  ret->type = E_CHP_VARSTRUCT_DEREF;
	}
	else {
	  if (TypeFactory::isBaseBoolType (it)) {
	    type = 0;
	  }
	  else {
	    type = TypeFactory::bitWidth (it);
	  }

	  struct chpsimderef *d = _mk_deref ((ActId *)e->u.e.l, s, &type);
	  ret->u.e.l = (Expr *) d;
	  ret->u.e.r = (Expr *) d->cx;
	  
	  if (type == 0) {
	    ret->type = E_CHP_VARBOOL_DEREF;
	  }
	  else {
	    ret->type = E_CHP_VARINT_DEREF;
	  }
	}
      }
      else {
	int w;
	act_boolean_netlist_t *bnl = s->cursi()->bnl;
	act_connection *cx = ((ActId *)e->u.e.l)->Canonical (bnl->cur);

	/* set potential shared variable flags */
	if (cx->isglobal()) {
	  *flags = *flags | 0x2;
	}
	for (int i=0; (((*flags) & 0x2) == 0) && i < A_LEN (bnl->ports); i++) {
	  if (cx == bnl->ports[i].c) {
	    *flags = *flags | 0x2;
	  }
	}
	for (int i=0; (((*flags) & 0x2) == 0) && i < A_LEN (bnl->chpports); i++) {
	  if (cx == bnl->chpports[i].c) {
	    *flags = *flags | 0x2;
	  }
	}
	for (int i=0; (((*flags) & 0x2) == 0) && i < A_LEN (bnl->instports); i++) {
	  if (cx == bnl->instports[i]) {
	    *flags = *flags | 0x2;
	  }
	}
	for (int i=0; (((*flags) & 0x2) == 0) && i < A_LEN (bnl->instchpports); i++) {
	  if (cx == bnl->instchpports[i]) {
	    *flags = *flags | 0x2;
	  }
	}
	
	if (TypeFactory::isStructure (it)) {
	  struct chpsimderef *ds =
	    _mk_std_deref_struct ((ActId *)e->u.e.l, it, s);

	  ret->u.e.l = (Expr *)ds;
	  ret->u.e.r = (Expr *)ds->cx;
	  ret->type = E_CHP_VARSTRUCT;
	}
	else if (it->arrayInfo() && !((ActId *)e->u.e.l)->Tail()->isDeref()) {
	  struct chpsimderef *d =
	    _mk_std_deref ((ActId *)e->u.e.l, it, s);
	  ret->u.e.l = (Expr *) d;
	  ret->u.e.r = (Expr *) d->cx;
	  ret->type = E_CHP_VARARRAY;
	}
	else {
	  ret->u.x.val = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), &type, &w);
	  ret->u.x.extra = w;
	//ret->u.x.extra = (unsigned long) ((ActId *)e->u.e.l)->Canonical (s->cursi()->bnl->cur);
	  if (type == 2) {
	    ret->type = E_CHP_VARCHAN;
	  }
	  else if (type == 1) {
	    ret->type = E_CHP_VARINT;
	  }
	  else if (type == 0) {
	    ret->type = E_CHP_VARBOOL;
	  }
	  else {
	    fatal_error ("Channel output variable used in expression?");
	  }
	}
      }
    }
    break;

  case E_PROBE:
    {
      int type;
      ret->u.x.val = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), &type);
      ret->u.x.extra = (unsigned long) ((ActId *)e->u.e.l)->Canonical
	(s->cursi()->bnl->cur);
      if (type == 2) {
	ret->type = E_PROBEIN;
      }
      else if (type == 3) {
	ret->type = E_PROBEOUT;
      }
      else {
	Assert (0, "Probe on a non-channel?");
      }
      *flags = *flags | 0x1;
    }
    break;
    
  case E_FUNCTION:
    ret->u.fn.s = e->u.fn.s;
    ret->u.fn.r = NULL;
    {
      Expr *tmp = NULL;
      e = e->u.fn.r;
      while (e) {
	if (!tmp) {
	  NEW (tmp, Expr);
	  ret->u.fn.r = tmp;
	}
	else {
	  NEW (tmp->u.e.r, Expr);
	  tmp = tmp->u.e.r;
	}
	tmp->u.e.r = NULL;
	tmp->type = e->type;
	tmp->u.e.l = expr_to_chp_expr (e->u.e.l, s, flags);
	e = e->u.e.r;
      }
      Function *f = (Function *)ret->u.fn.s;
      if (f->getlang() && f->getlang()->getchp()) {
	act_chp *chp = f->getlang()->getchp ();
	process_func_body_exprs (f, chp->c, s);
      }
    }
    break;

  case E_BUILTIN_INT:
  case E_BUILTIN_BOOL:
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s, flags);
    ret->u.e.r = expr_to_chp_expr (e->u.e.r, s, flags);
    break;

  case E_SELF:
  case E_SELF_ACK:
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  return ret;
}

static int _get_detailed_costs (int &pos, const stateinfo_t *si) 
{
  char buf[10240];
  char *nsname = NULL;
  if (si->bnl->p->getns() != ActNamespace::Global()) {
    nsname = si->bnl->p->getns()->Name();
  }
  if (nsname) {
    snprintf (buf, 10240, "sim.chp.%s::%s.delays", nsname+2, si->bnl->p->getName());
    FREE (nsname);
    nsname = NULL;
  }
  else {
    snprintf (buf, 10240, "sim.chp.%s.delays", si->bnl->p->getName());
  }
  if (!config_exists(buf)) {
    return config_get_int ("sim.chp.default_delay");
  }
  int *dda_table = config_get_table_int (buf);
  Assert (pos>=0, "huh?!");
  int ret;
  if (pos>=config_get_table_size(buf)) {
    warning ("Detailed annotation table mismatch!");
    ret = config_get_int ("sim.chp.default_delay");
  }
  else {
    ret = int(dda_table[pos++]);
  }
  if (debug_metrics) { 
    fprintf (stderr, "delay : %d\n", ret);
  }
  return ret;
}

static void _dump_debug (act_chp_lang_t *c) {
  if (debug_metrics) {
    if (c->type == ACT_CHP_ASSIGN || c->type == ACT_CHP_ASSIGNSELF || 
        c->type == ACT_CHP_RECV || c->type == ACT_CHP_SEND) {
      fprintf (stderr, "\n basic : (");
    }
    if (c->type == ACT_CHP_SELECT || c->type == ACT_CHP_SELECT_NONDET) {
      fprintf (stderr, "\n select : (");
    }
    if (c->type == ACT_CHP_COMMA) {
      fprintf (stderr, "\n parallel : (");
    }
    if (c->type == ACT_CHP_LOOP || c->type == ACT_CHP_DOLOOP) {
      fprintf (stderr, "\n loop : (");
    }
    chp_print(stderr, c);
    fprintf (stderr, ") :: ");
  }
  return;
}

static void _get_detailed_costs (int &pos, const stateinfo_t *si, chpsimstmt *stmt)
{
  stmt->bw_cost = 0;
  stmt->energy_cost = 0;
  stmt->delay_cost = _get_detailed_costs (pos, si);
}

static chpsimstmt *gc_to_chpsim (act_chp_gc_t *gc, ActSimCore *s, const int annotate_mode, int &dda_pos)
{
  chpsimcond *tmp;
  chpsimstmt *ret;
  int flags;
  
  ret = NULL;
  if (!gc) return ret;

  NEW (ret, chpsimstmt);
  ret->type = CHPSIM_COND;
  ret->delay_cost = (annotate_mode) ? _get_detailed_costs(dda_pos, s->cursi()) : 0;
  ret->energy_cost = 0;
  ret->bw_cost = 0;
  tmp = NULL;
  ret->u.cond.stats = -1;

  ret->u.cond.stats = ChpSimGraph::max_stats;
  ret->u.cond.is_shared = 0;
  ret->u.cond.is_probe = 0;

  flags = 0;
  while (gc) {
    ChpSimGraph::max_stats++;
    if (!tmp) {
      tmp = &ret->u.cond.c;
    }
    else {
      NEW (tmp->next, chpsimcond);
      tmp = tmp->next;
    }
    tmp->next = NULL;
    tmp->g = expr_to_chp_expr (gc->g, s, &flags);
    gc = gc->next;
  }

  if (flags & 0x1) {
    ret->u.cond.is_probe = 1;
  }
  if ((flags & 0x2) || s->isInternalParallel()) {
    ret->u.cond.is_shared = 1;
  }
  return ret;
}

chpsimgraph_info *ChpSimGraph::buildChpSimGraph (ActSimCore *sc,
						 act_chp_lang_t *c)
{
  ChpSimGraph *stop;
  chpsimgraph_info *gi;

  cur_pending_count = 0;
  max_pending_count = 0;
  max_stats = 0;
  labels = NULL;

  if (!c) return NULL;

  gi = new chpsimgraph_info;
  int annotate_mode = config_get_int ("sim.chp.detailed_delay_annotation");

  if (c->type == ACT_HSE_FRAGMENTS) {
    if (annotate_mode!=0) { fatal_error("Detailed annotation not supported for HSE fragments"); }
    int len = 0;
    int i;
    act_chp_lang_t *ch;
    ChpSimGraph **nstop;
    ChpSimGraph **frags;
    struct Hashtable *fH;
    hash_bucket_t *b;
    for (ch = c; ch; ch = ch->u.frag.next) {
      len++;
    }
    // i = # of fragments
    MALLOC (nstop, ChpSimGraph *, len);
    MALLOC (frags, ChpSimGraph *, len);
    fH = hash_new (4);
    i = 0;
    for (ch = c; ch; ch = ch->u.frag.next) {
      b = hash_add (fH, ch->label);
      b->i = i;
      int dummy = 0;
      frags[i] = _buildChpSimGraph (sc, ch->u.frag.body, &nstop[i], annotate_mode, dummy);
      i++;
    }

    // now wire everything up
    i = 0;
    for (ch = c; ch; ch = ch->u.frag.next) {
      if (ch->u.frag.nextlabel) {
	b = hash_lookup (fH, ch->u.frag.nextlabel);
	Assert (b, "Hmm");
	// now link nstop[i] to frags[b->i]
	if (nstop[i]->next) {
	  warning ("Unused target branch for statement #%d?", i);
	}
	else {
	  nstop[i]->next = frags[b->i];
	}
      }
      else {
	ChpSimGraph *bstmt;
	chpsimstmt *branch;
	chpsimcond *tmp;
	int flags;
	int j;

	// build chpsimstmt
	
	NEW (branch, chpsimstmt);
	branch->type = CHPSIM_COND;
	branch->delay_cost = 0;
	branch->energy_cost = 0;
        branch->bw_cost = 0;
	branch->u.cond.stats = -1;
	branch->u.cond.is_shared = 0;
	branch->u.cond.is_probe = 0;

	bstmt = new ChpSimGraph (sc);
	MALLOC (bstmt->all, ChpSimGraph *, len);
	bstmt->stmt = branch;

	// dummy next: empty statement!
	bstmt->next = new ChpSimGraph (sc);
	
	// we need a guarded command now!
	tmp = NULL;
	flags = 0;
	j = 0;
	for (listitem_t *li = list_first (ch->u.frag.exit_conds); li;
	     li = list_next (li)) {
	  // first thing is an expression
	  Expr *e = (Expr *) list_value (li);
	  if (!tmp) {
	    tmp = &branch->u.cond.c;
	  }
	  else {
	    NEW (tmp->next, chpsimcond);
	    tmp = tmp->next;
	  }
	  tmp->next = NULL;
	  tmp->g = expr_to_chp_expr (e, sc, &flags);

	  // then label
	  li = list_next (li);
	  b = hash_lookup (fH, (char *)list_value (li));
	  Assert (b, "What?");
	  bstmt->all[j] = frags[b->i];
	  j++;
	}
	if (flags & 0x1) {
	  branch->u.cond.is_probe = 1;
	}
	if (flags & 0x2) {
	  branch->u.cond.is_shared = 1;
	  // no internal parallelism!
	}
	if (nstop[i]->next) {
	  warning ("Unused target branch for statement #%d?", i);
	}
	else {
	  nstop[i]->next = bstmt;
	}
      }
      i++;
    }
    hash_free (fH);
    stop = frags[0];
    FREE (frags);
    FREE (nstop);
    gi->g = stop;
    gi->max_count = max_pending_count;
    gi->max_stats = max_stats;
    gi->labels = labels;
    gi->e = NULL;
    return gi;
  }
  stop = new ChpSimGraph (sc);
  int dda_pos = 0;
  gi->g = _buildChpSimGraph (sc, c, &stop, annotate_mode, dda_pos);
  gi->max_count = max_pending_count;
  gi->max_stats = max_stats;
  gi->e = NULL;
  gi->labels = labels;
  
  return gi;
}

static ChpSimGraph *_gen_nop (ActSimCore *sc)
{
  ChpSimGraph *ret = new ChpSimGraph (sc);
  NEW (ret->stmt, chpsimstmt);
  ret->stmt->type = CHPSIM_NOP;
  ret->stmt->delay_cost = 1;
  ret->stmt->energy_cost = 0;
  ret->stmt->bw_cost = 0;
  return ret;
}

static void _update_label (struct Hashtable **H,
			   const char *l, ChpSimGraph *ptr)
{
  if (!l) return;
  
  if (!(*H)) {
    *H = hash_new (2);
  }
  if (hash_lookup (*H, l)) {
    warning ("Duplicate label `%s'; ignored", l);
  }
  else {
    hash_bucket_t *b;
    b = hash_add (*H, l);
    b->v = ptr;
  }
}
    
static void _get_costs (stateinfo_t *si, ActId *id, chpsimstmt *stmt)
{
  char buf[1024];
  char *nsname;
 
  if (si->bnl->p->getns() != ActNamespace::Global()) {
    nsname = si->bnl->p->getns()->Name();
  }
  else {
    nsname = NULL;
  }
  char tmpbuf[900], tmpbuf2[900];

  if (nsname) {
    snprintf (tmpbuf, 900, "%s::%s", nsname+2, si->bnl->p->getName());
    FREE (nsname);
    nsname = NULL;
  }
  else {
    snprintf (tmpbuf, 900, "%s", si->bnl->p->getName());
  }

  id->sPrint (tmpbuf2, 900);
  
  if (debug_metrics) {
    fprintf (stderr, "Looking for metrics for: %s [%s]\n", tmpbuf, tmpbuf2);
  }

  snprintf (buf, 1024, "sim.chp.%s.%s.D", tmpbuf, tmpbuf2);
  if (config_exists (buf)) {
    if (debug_metrics) fprintf (stderr, " >> found %s\n", buf);
    stmt->delay_cost = config_get_int (buf);
    if (stmt->delay_cost < 0) {
      stmt->delay_cost = 0;
    }
  }
  else {
    // default delay is 10
    stmt->delay_cost = config_get_int ("sim.chp.default_delay");
  }

  snprintf (buf, 1024, "sim.chp.%s.%s.D_bw", tmpbuf, tmpbuf2);
  if (config_exists (buf)) {
    if (debug_metrics) fprintf (stderr, " >> found %s\n", buf);
    stmt->bw_cost = config_get_int (buf);
  }
  else {
    stmt->bw_cost = 0;
  }
  
  snprintf (buf, 1024, "sim.chp.%s.%s.E", tmpbuf, tmpbuf2);
  if (config_exists (buf)) {
    if (debug_metrics) fprintf (stderr, " >> found %s\n", buf);
    stmt->energy_cost = config_get_int (buf);
    if (stmt->energy_cost < 0) {
      stmt->energy_cost = 0;
    }
  }
  else {
    // default energy is 0
    stmt->energy_cost = config_get_int ("sim.chp.default_energy");
  }
}


ChpSimGraph *ChpSimGraph::_buildChpSimGraph (ActSimCore *sc,
					    act_chp_lang_t *c,
					    ChpSimGraph **stop, const int annotate_mode, int &dda_pos)
{
  ChpSimGraph *ret = NULL;
  ChpSimGraph *tmp2;
  int i, count;
  int tmp;
  int width;
  int used_slots = 0;
  ChpSimGraph *ostop;
  hash_bucket_t *b;
  
  if (!c) return NULL;

  switch (c->type) {
  case ACT_CHP_SEMI:
    count = cur_pending_count;
    if (list_length (c->u.semi_comma.cmd)== 1) {
      ret = _buildChpSimGraph
	(sc,
	 (act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop, annotate_mode, dda_pos);
      _update_label (&labels, c->label, ret);
      return ret;
    }
    used_slots = cur_pending_count;
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      cur_pending_count = count;
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ChpSimGraph *tmp = _buildChpSimGraph (sc, t, &tmp2, annotate_mode, dda_pos);
      if (tmp) {
	if (!ret) {
	  ret = tmp;
	  *stop = tmp2;
	}
	else {
	  (*stop)->next = tmp;
	  *stop = tmp2;
	}
      }
      used_slots = MAX(used_slots, cur_pending_count);
    }
    cur_pending_count = used_slots;
    break;

  case ACT_CHP_COMMA:
    if (list_length (c->u.semi_comma.cmd)== 1) {
      ret = _buildChpSimGraph
	(sc,
	 (act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop, annotate_mode, dda_pos);
      _update_label (&labels, c->label, ret);
      return ret;
    }
    ret = new ChpSimGraph (sc);
    ostop = *stop;
    *stop = new ChpSimGraph (sc);
    tmp = cur_pending_count++;
    if (cur_pending_count > max_pending_count) {
      max_pending_count = cur_pending_count;
    }
    ret->next = *stop; // not sure we need this, but this is the fork/join
		       // connection

    count = 0;
    MALLOC (ret->all, ChpSimGraph *, list_length (c->u.semi_comma.cmd));
    i = 0;

    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      ret->all[i] = _buildChpSimGraph (sc,
				      (act_chp_lang_t *)list_value (li), &tmp2, annotate_mode, dda_pos);
      if (ret->all[i]) {
	tmp2->next = *stop;
	count++;
      }
      i++;
    }
    if (count > 0) {
      _dump_debug (c);
      NEW (ret->stmt, chpsimstmt);
      ret->stmt->delay_cost = (annotate_mode) ? _get_detailed_costs(dda_pos, sc->cursi()) : 0;
      ret->stmt->energy_cost = 0;
      ret->stmt->bw_cost = 0;
      ret->stmt->type = CHPSIM_FORK;
      ret->stmt->u.fork = i;
      (*stop)->wait = count;
      (*stop)->totidx = tmp;
    }
    else {
      FREE (ret->all);
      FREE (ret);
      FREE (*stop);
      *stop = ostop;
      ret = NULL;
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
    ret = new ChpSimGraph (sc);
    _dump_debug (c);
    ret->stmt = gc_to_chpsim (c->u.gc, sc, annotate_mode, dda_pos);
    if (c->type == ACT_CHP_LOOP) {
      ret->stmt->type = CHPSIM_LOOP;
    }
    else if (c->type == ACT_CHP_SELECT_NONDET) {
      ret->stmt->type = CHPSIM_CONDARB;
    }
    i = 0;
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      i++;
    }
    Assert (i >= 1, "What?");
    MALLOC (ret->all, ChpSimGraph *, i);
      
    (*stop) = new ChpSimGraph (sc);

    //if (c->type == ACT_CHP_LOOP) {
    ret->next = (*stop);
    //}
    i = 0;
    used_slots = cur_pending_count;
    count = cur_pending_count;
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      cur_pending_count = count;
      ret->all[i] = _buildChpSimGraph (sc, gc->s, &tmp2, annotate_mode, dda_pos);
      if (ret->all[i]) {
	if (c->type == ACT_CHP_LOOP) {
	  /* loop back */
	  tmp2->next = ret;
	}
	else {
	  tmp2->next = *stop;
	}
      }
      else {
	if (c->type != ACT_CHP_LOOP) {
	  ret->all[i] = (*stop);
	}
	else {
	  ret->all[i] = ret;
	}
      }
      i++;
      used_slots = MAX (used_slots, cur_pending_count);
    }
    cur_pending_count = used_slots;
    break;
    
  case ACT_CHP_DOLOOP:
    {
      ChpSimGraph *ntmp;
      ChpSimGraph *nret = _buildChpSimGraph (sc, c->u.gc->s, &ntmp, annotate_mode, dda_pos);

      ret = new ChpSimGraph (sc);

      if (!nret) {
	nret = _gen_nop (sc);
	ntmp = nret;
      }
      ntmp->next = ret;
      _dump_debug (c);
      ret->stmt = gc_to_chpsim (c->u.gc, sc, annotate_mode, dda_pos);
      ret->stmt->type = CHPSIM_LOOP;
      (*stop) = new ChpSimGraph (sc);
      ret->next = (*stop);
      MALLOC (ret->all, ChpSimGraph *, 1);
      ret->all[0] = _buildChpSimGraph (sc, c->u.gc->s, &tmp2, annotate_mode, dda_pos);
      if (!ret->all[0]) {
	ret->all[0] = _gen_nop (sc);
	tmp2 = ret->all[0];
      }
      tmp2->next = ret;
      ret = nret;
    }
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
    {
      InstType *it = sc->cursi()->bnl->cur->FullLookup (c->u.comm.chan, NULL);
      Chan *ch;
      int ch_struct;

      ch_struct = 0;
      if (TypeFactory::isUserType (it)) {
	Channel *x = dynamic_cast<Channel *> (it->BaseType());
	ch = dynamic_cast<Chan *> (x->root()->BaseType());
      }
      else {
	ch = dynamic_cast<Chan *> (it->BaseType());
      }
      if (TypeFactory::isStructure (ch->datatype())) {
	ch_struct = 1;
      }

      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);
      if (annotate_mode) {
        _dump_debug (c);
        _get_detailed_costs (dda_pos, sc->cursi(), ret->stmt);
      }
      else {
        _get_costs (sc->cursi(), c->u.comm.chan, ret->stmt);
      }
      ret->stmt->type = CHPSIM_SEND;
      ret->stmt->u.sendrecv.is_struct = ch_struct;
      if (ch_struct == 0) {
	ret->stmt->u.sendrecv.width = TypeFactory::bitWidth (ch);
      }
      else {
	ret->stmt->u.sendrecv.width = TypeFactory::totBitWidth (ch);
      }
      ret->stmt->u.sendrecv.e = NULL;
      ret->stmt->u.sendrecv.d = NULL;
      ret->stmt->u.sendrecv.is_structx = 0;

      if (c->u.comm.e) {
	int flags = 0;
	ret->stmt->u.sendrecv.e = expr_to_chp_expr (c->u.comm.e, sc, &flags);
      }
      if (c->u.comm.var) {
	ActId *id = c->u.comm.var;
	int type = -1;
	struct chpsimderef *d;

	if (TypeFactory::isStructure (ch->acktype())) {
	  ret->stmt->u.sendrecv.is_structx = 2;
	  if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, id)) {
	    d = _mk_deref_struct (id, sc);
	    type = 1;
	  }
	  else {
	    d = _mk_std_deref_struct (id, ch->acktype(), sc);
	    type = 1;
	  }
	}
	else {
	  ret->stmt->u.sendrecv.is_structx = 1;
	  if (TypeFactory::isBaseBoolType (ch->acktype())) {
	    type = 0;
	  }
	  else {
	    type = TypeFactory::bitWidth (ch->acktype());
	  }
	  if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, id)) {
	    d = _mk_deref (id, sc, &type);
	  }
	  else {
	    InstType *xit;
	    act_type_var (sc->cursi()->bnl->cur, id, &xit);
	    Assert (xit, "Typechecking");
	    d = _mk_std_deref (id, xit, sc);
	    if (d->isbool) {
	      type = 0;
	    }
	    else {
	      type = 1;
	    }
	    width = d->width;
	  }
	}
	if (type == 3) {
	  type = 2;
	}
	ret->stmt->u.sendrecv.d = d;
	ret->stmt->u.sendrecv.d_type = type;
	if (TypeFactory::isEnum (ch->acktype())) {
	  ret->stmt->u.sendrecv.d->isenum = 1;
	  ret->stmt->u.sendrecv.d->enum_sz = TypeFactory::enumNum (ch->acktype());
	}
      }
      ret->stmt->u.sendrecv.chvar = sc->getLocalOffset (c->u.comm.chan, sc->cursi(), NULL);
      ret->stmt->u.sendrecv.vc = c->u.comm.chan->Canonical (sc->cursi()->bnl->cur);
      ret->stmt->u.sendrecv.flavor = c->u.comm.flavor;
      (*stop) = ret;
    }
    break;
    
    
  case ACT_CHP_RECV:
    {
      InstType *it = sc->cursi()->bnl->cur->FullLookup (c->u.comm.chan, NULL);
      Chan *ch;
      int ch_struct;

      ch_struct = 0;
      if (TypeFactory::isUserType (it)) {
	Channel *x = dynamic_cast<Channel *> (it->BaseType());
	ch = dynamic_cast<Chan *> (x->root()->BaseType());
      }
      else {
	ch = dynamic_cast<Chan *> (it->BaseType());
      }
      if (TypeFactory::isStructure (ch->datatype())) {
	ch_struct = 1;
      }
      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);
      if (annotate_mode) {
        _dump_debug (c);
        _get_detailed_costs (dda_pos, sc->cursi(), ret->stmt);
      }
      else {
        _get_costs (sc->cursi(), c->u.comm.chan, ret->stmt);
      }
      ret->stmt->type = CHPSIM_RECV;
      if (ch_struct) {
	ret->stmt->u.sendrecv.is_struct = 1;
	ret->stmt->u.sendrecv.width = TypeFactory::totBitWidth (ch);
      }
      else {
	ret->stmt->u.sendrecv.is_struct = 0;
	ret->stmt->u.sendrecv.width = TypeFactory::bitWidth (ch);
      }

      ret->stmt->u.sendrecv.e = NULL;
      ret->stmt->u.sendrecv.d = NULL;
      ret->stmt->u.sendrecv.is_structx = 0;

      if (c->u.comm.e) {
	int flags = 0;
	ret->stmt->u.sendrecv.e = expr_to_chp_expr (c->u.comm.e, sc, &flags);
	Assert (ch->acktype(), "Bidirectional channel inconsistency");
	if (TypeFactory::isStructure (ch->acktype())) {
	  ret->stmt->u.sendrecv.is_structx = 2;
	}
	else {
	  ret->stmt->u.sendrecv.is_structx = 1;
	}
      }
      if (c->u.comm.var) {
	ActId *id = c->u.comm.var;
	int type = -1;
	struct chpsimderef *d;

	/*-- if this is a structure, unravel the structure! --*/
	if (TypeFactory::isStructure (ch->datatype())) {
	  if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, id)) {
	    d = _mk_deref_struct (id, sc);
	  }
	  else {
	    d = _mk_std_deref_struct (id, ch->datatype(), sc);
	  }
	  type = 1;
	}
	else {
	  if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, id)) {
	    if (TypeFactory::isBaseBoolType (ch->datatype())) {
	      type = 0;
	    }
	    else {
	      type = TypeFactory::bitWidth (ch->datatype());
	    }
	    d = _mk_deref (id, sc, &type);
	  }
	  else {
	    InstType *xit;
	    act_type_var (sc->cursi()->bnl->cur, id, &xit);
	    Assert (xit, "Hmm");
	    d = _mk_std_deref (id, xit, sc);
	    width = d->width;
	    if (d->isbool) {
	      type = 0;
	    }
	    else {
	      type = 1;
	    }
	  }
	}
	if (type == 3) {
	  type = 2;
	}
	ret->stmt->u.sendrecv.d = d;
	ret->stmt->u.sendrecv.d_type = type;

	if (TypeFactory::isEnum (ch->datatype())) {
	  ret->stmt->u.sendrecv.d->isenum = 1;
	  ret->stmt->u.sendrecv.d->enum_sz = TypeFactory::enumNum (ch->datatype());
	}
      }
      ret->stmt->u.sendrecv.chvar = sc->getLocalOffset (c->u.comm.chan, sc->cursi(), NULL);
      ret->stmt->u.sendrecv.vc = c->u.comm.chan->Canonical (sc->cursi()->bnl->cur);
      ret->stmt->u.sendrecv.flavor = c->u.comm.flavor;
      (*stop) = ret;
    }
    break;

  case ACT_CHP_FUNC:
    if (strcmp (string_char (c->u.func.name), "log") == 0 
        || strcmp (string_char (c->u.func.name), "log_p") == 0 
        || strcmp (string_char (c->u.func.name), "warn") == 0
      ) {
      listitem_t *li;
      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);
      ret->stmt->delay_cost = 0;
      ret->stmt->energy_cost = 0;
      ret->stmt->bw_cost = 0;
      ret->stmt->type = CHPSIM_FUNC;
      ret->stmt->u.fn.name = string_char (c->u.func.name);
      ret->stmt->u.fn.l = list_new();
      for (li = list_first (c->u.func.rhs); li; li = list_next (li)) {
	act_func_arguments_t *tmp = (act_func_arguments_t *)list_value (li);
	act_func_arguments_t *x;
	NEW (x, act_func_arguments_t);
	x->isstring = tmp->isstring;
	if (tmp->isstring) {
	  x->u.s = tmp->u.s;
	}
	else {
	  int flags = 0;
	  x->u.e = expr_to_chp_expr (tmp->u.e, sc, &flags);
	}
	list_append (ret->stmt->u.fn.l, x);
      }
      (*stop) = ret;
    }
    else if (strcmp (string_char (c->u.func.name), "assert") == 0) {
      listitem_t *li;
      bool condition = true;
      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);
      ret->stmt->delay_cost = 0;
      ret->stmt->energy_cost = 0;
      ret->stmt->bw_cost = 0;
      ret->stmt->type = CHPSIM_FUNC;
      ret->stmt->u.fn.name = string_char (c->u.func.name);
      ret->stmt->u.fn.l = list_new();
      for (li = list_first (c->u.func.rhs); li; li = list_next (li)) {
	act_func_arguments_t *tmp = (act_func_arguments_t *)list_value (li);
  if (condition) {
    if (tmp->isstring) {
      fatal_error ("Built-in function 'assert' expects an expression as its first argument");
    } else {
      condition = false;
    }
  }
	act_func_arguments_t *x;
	NEW (x, act_func_arguments_t);
	x->isstring = tmp->isstring;
	if (tmp->isstring) {
	  x->u.s = tmp->u.s;
	}
	else {
	  int flags = 0;
	  x->u.e = expr_to_chp_expr (tmp->u.e, sc, &flags);
	}
	list_append (ret->stmt->u.fn.l, x);
      }
      (*stop) = ret;
    }
    else if (strcmp (string_char (c->u.func.name), "log_nl") == 0 || strcmp (string_char (c->u.func.name), "log_st") == 0) {
      listitem_t *li;
      bool condition = true;
      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);
      ret->stmt->delay_cost = 0;
      ret->stmt->energy_cost = 0;
      ret->stmt->bw_cost = 0;
      ret->stmt->type = CHPSIM_FUNC;
      ret->stmt->u.fn.name = string_char (c->u.func.name);
      ret->stmt->u.fn.l = list_new();
      (*stop) = ret;
    }
    else {
      warning ("Built-in function `%s' is not known; valid values: log, log_st, log_p, log_nl, assert, warn",
	       string_char (c->u.func.name));
    }
    break;
    
  case ACT_CHP_ASSIGN:
    {
      InstType *it = sc->cursi()->bnl->cur->FullLookup (c->u.assign.id, NULL);
      int type, width;
      int flags = 0;

      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);

      if (annotate_mode) {
        _dump_debug (c);
        _get_detailed_costs (dda_pos, sc->cursi(), ret->stmt);
      }
      else {
        _get_costs (sc->cursi(), c->u.assign.id, ret->stmt);
      }
      ret->stmt->type = CHPSIM_ASSIGN;
      if (TypeFactory::isStructure (it)) {
	ret->stmt->u.assign.is_struct = 1;
      }
      else {
	ret->stmt->u.assign.is_struct = 0;
      }
      ret->stmt->u.assign.e = expr_to_chp_expr (c->u.assign.e, sc, &flags);

      if (ret->stmt->u.assign.is_struct) {
	if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, c->u.assign.id))  {
	  struct chpsimderef *d = _mk_deref_struct (c->u.assign.id, sc);
	  type = 1;
	  ret->stmt->u.assign.d = *d;
	  FREE (d);
	}
	else {
	  struct chpsimderef *d =
	    _mk_std_deref_struct (c->u.assign.id, it, sc);
	  ret->stmt->u.assign.d = *d;
	  FREE (d);
	}
      }
      else {
	if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, c->u.assign.id)) {
	  if (TypeFactory::isBaseBoolType (it)) {
	    type = 0;
	  }
	  else {
	    type = TypeFactory::bitWidth (it);
	  }

	  struct chpsimderef *d = _mk_deref (c->u.assign.id, sc, &type, &width);
	  ret->stmt->u.assign.d = *d;
	  FREE (d);
	}
	else {
	  ret->stmt->u.assign.d.stride = 1;
	  ret->stmt->u.assign.d.range = NULL;
	  ret->stmt->u.assign.d.idx = NULL;
	  ret->stmt->u.assign.d.offset =
	    sc->getLocalOffset (c->u.assign.id, sc->cursi(), &type, &width);
	  ret->stmt->u.assign.d.cx =
	    c->u.assign.id->Canonical (sc->cursi()->bnl->cur);
	  ret->stmt->u.assign.d.isbool = 0;
	  ret->stmt->u.assign.d.isenum = 0;
	}
	if (type == 1) {
	  Assert (width > 0, "zero-width int?");
	  ret->stmt->u.assign.isint = width;

	  if (TypeFactory::isEnum (it)) {
	    ret->stmt->u.assign.d.isenum = 1;
	    ret->stmt->u.assign.d.enum_sz = TypeFactory::enumNum (it);
	  }
	}
	else {
	  Assert (type == 0, "Typechecking?!");
	  ret->stmt->u.assign.isint = 0;
	  ret->stmt->u.assign.d.isbool = 1;
	}
      }
    }
    (*stop) = ret;
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
  _update_label (&labels, c->label, ret);
  return ret;
}

void ChpSimGraph::checkFragmentation (ActSimCore *sc, ChpSim *c, ActId *id, int read_only)
{
  sc->checkFragmentation (id, c, sc->cursi(), read_only);
}

void ChpSimGraph::recordChannel (ActSimCore *sc, ChpSim *c, ActId *id)
{
  sim_recordChannel (sc, c, id);
}

void ChpSimGraph::checkFragmentation (ActSimCore *sc, ChpSim *c, Expr *e)
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
    checkFragmentation (sc, c, e->u.e.l);
    checkFragmentation (sc, c, e->u.e.r);
    break;
    
  case E_UMINUS:
  case E_COMPLEMENT:
  case E_NOT:
    checkFragmentation (sc, c, e->u.e.l);
    break;

  case E_QUERY:
    checkFragmentation (sc, c, e->u.e.l);
    checkFragmentation (sc, c, e->u.e.r->u.e.l);
    checkFragmentation (sc, c, e->u.e.l->u.e.l);
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    do {
      checkFragmentation (sc, c, e->u.e.l);
      e = e->u.e.r;
    } while (e);
    break;

  case E_BITFIELD:
    checkFragmentation (sc, c, (ActId *)e->u.e.l, 1);
    break;

  case E_VAR:
    checkFragmentation (sc, c, (ActId *)e->u.e.l, 1);
    break;

  case E_PROBE:
    recordChannel (sc, c, (ActId *)e->u.e.l);
    break;

  case E_BUILTIN_BOOL:
  case E_BUILTIN_INT:
    checkFragmentation (sc, c, e->u.e.l);
    break;

  case E_FUNCTION:
    e = e->u.fn.r;
    while (e) {
      checkFragmentation (sc, c, e->u.e.l);
      e = e->u.e.r;
    }
    break;
    
  case E_SELF:
  case E_SELF_ACK:
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
}

  
void ChpSimGraph::checkFragmentation (ActSimCore *sc, ChpSim *cc,
				      act_chp_lang_t *c)
{
  listitem_t *li;

  if (!c) return;
  
  switch (c->type) {
  case ACT_HSE_FRAGMENTS:
    while (c) {
      checkFragmentation (sc, cc, c->u.frag.body);
      c = c->u.frag.next;
    }
    break;
    
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (li = list_first (c->u.semi_comma.cmd); li; li = list_next (li)) {
      checkFragmentation (sc, cc, (act_chp_lang_t *) list_value (li));
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
  case ACT_CHP_DOLOOP:
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      checkFragmentation (sc, cc, gc->g);
      checkFragmentation (sc, cc, gc->s);
    }
    break;

  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
    recordChannel (sc, cc, c->u.comm.chan);
    if (c->u.comm.e) {
      checkFragmentation (sc, cc, c->u.comm.e);
    }
    if (c->u.comm.var) {
      checkFragmentation (sc, cc,c->u.comm.var, 0);
    }
    break;
    
  case ACT_CHP_FUNC:
    break;
    
  case ACT_CHP_ASSIGN:
    checkFragmentation (sc, cc, c->u.assign.id, 1);
    checkFragmentation (sc, cc, c->u.assign.e);
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
}

void ChpSimGraph::recordChannel (ActSimCore *sc, ChpSim *cc,
				 act_chp_lang_t *c)
{
  listitem_t *li;

  if (!c) return;
  
  switch (c->type) {
  case ACT_HSE_FRAGMENTS:
    break;
    
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (li = list_first (c->u.semi_comma.cmd); li; li = list_next (li)) {
      recordChannel (sc, cc, (act_chp_lang_t *) list_value (li));
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
  case ACT_CHP_DOLOOP:
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      recordChannel (sc, cc, gc->s);
    }
    break;

  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
    recordChannel (sc, cc, c->u.comm.chan);
    break;
    
  case ACT_CHP_RECV:
    recordChannel (sc, cc, c->u.comm.chan);
    break;

  case ACT_CHP_FUNC:
    break;
    
  case ACT_CHP_ASSIGN:
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
}


ChpSimGraph::~ChpSimGraph ()
{
#if 0  
  printf ("del %p:: ", this);
  printStmt (stdout, NULL);
  printf ("\n");
#endif  
  
  wait = -1;
  if (stmt) {
    switch (stmt->type) {
    case CHPSIM_COND:
    case CHPSIM_CONDARB:
    case CHPSIM_LOOP:
      {
	struct chpsimcond *x;
	int nguards = 1;
	int nw;
	_free_chp_expr (stmt->u.cond.c.g);
	x = stmt->u.cond.c.next;
	while (x) {
	  struct chpsimcond *t;
	  _free_chp_expr (x->g);
	  t = x->next;
	  FREE (x);
	  x = t;
	  nguards++;
	}
	Assert (next, "What?");
	nw = next->wait;
	next->wait = -1;
	for (int i=0; i < nguards; i++) {
	  if (all[i] && all[i]->wait != -1) {
	    delete all[i];
	  }
	}
	if (all) {
	  FREE (all);
	}
	next->wait = nw;
      }
      break;
      
    case CHPSIM_FORK:
      Assert (all, "What?");
      for (int i=0; i < stmt->u.fork; i++) {
	if (all[i] && all[i]->wait != -1) {
	  delete all[i];
	}
      }
      FREE (all);
      next = NULL; // no need to use next, it will be handled by one
		   // of the forked branches
      break;
      
    case CHPSIM_ASSIGN:
      _free_deref (&stmt->u.assign.d);
      _free_chp_expr (stmt->u.assign.e);
      break;

    case CHPSIM_NOP:
      break;

    case CHPSIM_RECV:
    case CHPSIM_SEND:
      if (stmt->u.sendrecv.e) {
	_free_chp_expr (stmt->u.sendrecv.e);
      }
      if (stmt->u.sendrecv.d) {
	_free_deref (stmt->u.sendrecv.d);
      }
      break;
      
    case CHPSIM_FUNC:
      for (listitem_t *li = list_first (stmt->u.fn.l); li;
	   li = list_next (li)) {
	act_func_arguments_t *tmp = (act_func_arguments_t *)list_value (li);
	if (!tmp->isstring) {
	  _free_chp_expr (tmp->u.e);
	}
	FREE (tmp);
      }
      list_free (stmt->u.fn.l);
      break;

    default:
      fatal_error ("What type is this (%d) in ~ChpSimGraph()?", stmt->type);
      break;
    }
    FREE (stmt);
  }
  if (next) {
    if (next->wait > 1) {
      next->wait--;
    }
    else {
      if (next->wait != -1) {
	delete next;
      }
    }
  }
  next = NULL;
}

chpsimgraph_info::~chpsimgraph_info()
{
  if (g) { delete g; }
}
