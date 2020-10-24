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
#include <stdio.h>
#include "config.h"
#include <simdes.h>
#include "chpsim.h"
#include <dlfcn.h>

class ChpSim;

#ifndef MAX
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#endif

//#define DUMP_ALL

#define WAITING_SENDER(c)  ((c)->send_here != 0 && (c)->sender_probe == 0)
#define WAITING_SEND_PROBE(c)  ((c)->send_here != 0 && (c)->sender_probe == 1)

#define WAITING_RECEIVER(c)  ((c)->recv_here != 0 && (c)->receiver_probe == 0)
#define WAITING_RECV_PROBE(c)  ((c)->recv_here != 0 && (c)->receiver_probe == 1)


ChpSim::ChpSim (ChpSimGraph *g, act_chp_lang_t *c, ActSimCore *sim)
: ActSimObj (sim)
{
  /*
    Analyze the chp body to find out the maximum number of concurrent
    threads; those are the event types.
  */
  _npc = _max_program_counters (c);
  _pcused = 1;
  Assert (_npc >= 1, "What?");

  _pc = (ChpSimGraph **)
    sim->getState()->allocState (sizeof (ChpSimGraph *)*_npc);
  for (int i=0; i < _npc; i++) {
    _pc[i] = NULL;
  }

  Assert (_npc >= 1, "What?");
  _pc[0] = g;

  _stalled_pc = -1;
  _statestk = list_new ();
  _probe = NULL;
  _savedc = c;

  new Event (this, SIM_EV_MKTYPE (0,0) /* pc */, 10);
}


void ChpSim::computeFanout ()
{
  _compute_used_variables (_savedc);
}

int ChpSim::_updatepc (int pc)
{
  int joined;
  
  _pc[pc] = _pc[pc]->completed(pc, &joined);
  if (joined) {
#ifdef DUMP_ALL
    printf (" [joined]");
#endif    
    _pcused = _pcused - (_pc[pc]->wait - 1);
  }
  if (pc >= _pcused) {
    ChpSimGraph *tmp = _pc[pc];
    _pc[pc] = NULL;
    pc = _pcused - 1;
    _pc[pc] = tmp;
#if 0    
    printf (" [pc-adjust]");
#endif    
  }
  return pc;
}

int ChpSim::_collect_sharedvars (Expr *e, int pc, int undo)
{
  int ret = 0;
  if (!e) return ret;

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
    ret = ret | _collect_sharedvars (e->u.e.l, pc, undo);
    ret = ret | _collect_sharedvars (e->u.e.r, pc, undo);
    break;
    
  case E_UMINUS:
  case E_COMPLEMENT:
  case E_NOT:
    ret = ret | _collect_sharedvars (e->u.e.l, pc, undo);
    break;

  case E_QUERY:
    ret = ret | _collect_sharedvars (e->u.e.l, pc, undo);
    ret = ret | _collect_sharedvars (e->u.e.r->u.e.l, pc, undo);
    ret = ret | _collect_sharedvars (e->u.e.r->u.e.r, pc, undo);
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    do {
      ret = ret | _collect_sharedvars (e->u.e.l, pc, undo);
      e = e->u.e.r;
    } while (e);
    break;

  case E_BITFIELD:
    ret = ret | _collect_sharedvars (e->u.e.l, pc, undo);
    break;

  case E_CHP_VARBOOL:
  case E_CHP_VARINT:
  case E_VAR:
    if (e->u.x.val < 0) {
      ret = 1;
    }
    break;

  case E_CHP_VARCHAN:
    break;
    
  case E_PROBE:
    fatal_error ("What?");
    break;
    
  case E_PROBEIN:
  case E_PROBEOUT:
    {
      int off = getGlobalOffset (e->u.x.val, 2);
      act_channel_state *c = _sc->getChan (off);
      if (undo) {
	if (c->probe) {
#ifdef DUMP_ALL	  
	  printf (" [clr %d]", off);
#endif	  
	  if (!_probe) {
	    _probe = c->probe;
	  }
	  else {
	    if (_probe != c->probe) {
	      fatal_error ("Weird!");
	    }
	  }
	  if (c->probe->isWaiting (this)) {
	    c->probe->DelObject (this);
	  }
	  c->probe = NULL;
	}
	if (e->type == E_PROBEIN) {
	  if (c->receiver_probe) {
	    c->recv_here = 0;
	    c->receiver_probe = 0;
	  }
	}
	else {
	  if (c->sender_probe) {
	    c->send_here = 0;
	    c->sender_probe = 0;
	  }
	}
      }
      else {
	if (e->type == E_PROBEIN && !WAITING_SENDER(c)) {
#ifdef DUMP_ALL	  
	  printf (" [add %d]", off);
#endif	  
	  if (!_probe) {
	    _probe = new WaitForOne (0);
	  }
	  if (c->probe && c->probe != _probe) {
	    fatal_error ("Channel is being probed by multiple processes!");
	  }
	  c->probe = _probe;
	  if (!c->probe->isWaiting (this)) {
	    c->probe->AddObject (this);
	  }
	  c->recv_here = (pc+1);
	  c->receiver_probe = 1;
	}
	else if (e->type == E_PROBEOUT && !WAITING_RECEIVER(c)) {
#ifdef DUMP_ALL	  
	  printf (" [add %d]", off);
#endif	  
	  if (!_probe) {
	    _probe = new WaitForOne (0);
	  }
	  if (c->probe && c->probe != _probe) {
	    fatal_error ("Channel is being probed by multiple processes!");
	  }
	  c->probe = _probe;
	  if (!c->probe->isWaiting (this)) {
	    c->probe->AddObject (this);
	  }
	  c->send_here = (pc+1);
	  c->sender_probe = 1;
	}
      }
    }
    break;

  case E_BUILTIN_BOOL:
  case E_BUILTIN_INT:
    ret = ret | _collect_sharedvars (e->u.e.l, pc, undo);
    break;

  case E_FUNCTION:
  case E_SELF:
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  return ret;
}

/*-- returns 1 if there's a shared variable in the guard --*/
int ChpSim::_add_waitcond (chpsimcond *gc, int pc, int undo)
{
  int ret = 0;

#ifdef DUMP_ALL
  if (undo) {
    printf (" [del-waits]");
  }
  else {
    printf (" [add-waits]");
  }
#endif
  _probe = NULL;
  while (gc) {
    if (gc->g) {
      ret = ret | _collect_sharedvars (gc->g, pc, undo);
    }
    gc = gc->next;
  }
  if (undo) {
    if (_probe) {
      delete _probe;
    }
  }
#if 0
  printf (" sh-var:%d", ret);
#endif
  return ret;
}

int ChpSim::computeOffset (struct chpsimderef *d)
{
  if (!d->range) {
    return d->offset;
  }
  for (int i=0; i < d->range->nDims(); i++) {
    expr_res res = exprEval (d->chp_idx[i]);
    d->idx[i] = res.v;
  }
  int x = d->range->Offset (d->idx);
  if (x == -1) {
    fatal_error ("Array out of bounds!");
  }
  return d->offset + x;
}

void ChpSim::Step (int ev_type)
{
  int pc = SIM_EV_TYPE (ev_type);
  int flag = SIM_EV_FLAGS (ev_type);
  int forceret = 0;
  int joined;
  expr_res v;
  int off;
  int delay = 10;

  if (pc == MAX_LOCAL_PCS) {
    pc = _stalled_pc;
    _stalled_pc = -1;
  }

  if (!_pc[pc]) {
    return;
  }

  /*-- go forward through sim graph until there's some work --*/
  while (_pc[pc] && !_pc[pc]->stmt) {
    pc = _updatepc (pc);
    /* if you go forward, then you're not waking up any more */
    flag = 0;
  }
  if (!_pc[pc]) return;

  chpsimstmt *stmt = _pc[pc]->stmt;

#ifdef DUMP_ALL  
  printf ("[%8lu %d; pc:%d(%d)] <", CurTimeLo(), flag, pc, _pcused);
  name->Print (stdout);
  printf ("> ");
#endif

  /*--- simulate statement until there's a blocking scenario ---*/
  switch (stmt->type) {
  case CHPSIM_FORK:
#ifdef DUMP_ALL
    printf ("fork");
#endif    
    forceret = 1;
    {
      int first = 1;
      int count = 0;
      int idx;
      ChpSimGraph *g = _pc[pc];
      for (int i=0; i < stmt->u.fork; i++) {
	if (first) {
	  idx = pc;
	  first = 0;
	}
	else {
	  idx = count + _pcused;
	  count++;
	}
	_pc[idx] = g->all[i];
	if (g->all[i]) {
#if 0	  
	  printf (" idx:%d", idx);
#endif
	  new Event (this, SIM_EV_MKTYPE (idx,0), 10);
	}
      }
      _pcused += stmt->u.fork - 1;
#if 0
      printf (" _used:%d", _pcused);
#endif      
    }
    break;

  case CHPSIM_ASSIGN:
#ifdef DUMP_ALL
    printf ("assign v[%d] := ", stmt->u.assign.var);
#endif
    v = exprEval (stmt->u.assign.e);
#ifdef DUMP_ALL    
    printf ("%d (w=%d)", v.v, v.width);
#endif
    pc = _updatepc (pc);
    off = computeOffset (&stmt->u.assign.d);
    if (stmt->u.assign.isbool) {
      off = getGlobalOffset (off, 0);
#if 0      
      printf (" [glob=%d]", off);
#endif      
      _sc->setBool (off, v.v);
    }
    else {
      off = getGlobalOffset (off, 1);
#if 0      
      printf (" [glob=%d]", off);
#endif
      _sc->setInt (off, v.v);
    }
    break;

  case CHPSIM_SEND:
    if (!flag) {
      listitem_t *li;
      li = list_first (stmt->u.send.el);
      if (li) {
	v = exprEval ((Expr *)list_value (li));
      }
      else {
	/* data-less */
	v.v = 0;
	v.width = 0;
      }
#ifdef DUMP_ALL      
      printf ("send val=%d", v.v);
#endif      
      if (varSend (pc, flag, stmt->u.send.chvar, v)) {
	/* blocked */
	forceret = 1;
      }
      else {
	pc = _updatepc (pc);
      }
    }
    else {
      if (!varSend (pc, flag, stmt->u.send.chvar, v)) {
	pc = _updatepc (pc);
      }
#ifdef DUMP_ALL      
      printf ("send done");
#endif      
    }
    break;

  case CHPSIM_RECV:
    {
      listitem_t *li;
      struct chpsimderef *d;
      int id;
      int type;
      li = list_first (stmt->u.recv.vl);
      if (li) {
	type = (long)list_value (li);
	li = list_next (li);
	Assert (li, "What?");
	d = (struct chpsimderef *)list_value (li);
	id = computeOffset (d);
      }
      else {
	type = -1;
      }

      if (varRecv (pc, flag, stmt->u.recv.chvar, &v)) {
#ifdef DUMP_ALL	
	printf ("recv blocked");
#endif	
	forceret = 1;
      }
      else {
#ifdef DUMP_ALL	
	printf ("recv got %d!", v.v);
#endif	
	if (type != -1) {
	  if (type == 0) {
	    off = getGlobalOffset (id, 0);
#if 0	    
	    printf (" [glob=%d]", off);
#endif	    
	    _sc->setBool (off, v.v);
	  }
	  else {
	    off = getGlobalOffset (id, 1);
#if 0	    
	    printf (" [glob=%d]", off);
#endif	    
	    _sc->setInt (off, v.v);
	  }
	}
	pc = _updatepc (pc);
      }
    }
    break;

  case CHPSIM_FUNC:
    printf ("[%8lu t#:%d] <", CurTimeLo(), pc);
    name->Print (stdout);
    printf ("> ");
    for (listitem_t *li = list_first (stmt->u.fn.l); li; li = list_next (li)) {
      act_func_arguments_t *arg = (act_func_arguments_t *) list_value (li);
      if (arg->isstring) {
	printf ("%s", string_char (arg->u.s));
      }
      else {
	v = exprEval (arg->u.e);
	printf ("%d", v.v);
      }
    }
    printf ("\n");
    pc = _updatepc (pc);
    delay = 0;
    break;

  case CHPSIM_COND:
    {
      chpsimcond *gc;
      int cnt = 0;
      expr_res res;

#ifdef DUMP_ALL
      if (_pc[pc]->next == NULL) {
	printf ("cond");
      }
      else {
	printf ("loop");
      }
#endif
      
      if (flag) {
	/*-- release wait conditions in case there are multiple --*/
        if (_add_waitcond (&stmt->u.c, pc, 1)) {
	  if (_stalled_pc != -1) {
	    _sc->gRemove (this);
	    _stalled_pc = -1;
	  }
	}
      }

      gc = &stmt->u.c;
      while (gc) {
	if (gc->g) {
	  res = exprEval (gc->g);
	}
	if (!gc->g || res.v) {
#ifdef DUMP_ALL	  
	  printf (" gd#%d true", cnt);
#endif	  
	  _pc[pc] = _pc[pc]->all[cnt];
	  break;
	}
	cnt++;
	gc = gc->next;
      }
      /* all guards false */
      if (!gc) {
	if (_pc[pc]->next == NULL) {
	  /* selection: we just try again later: add yourself to
	     probed signals */
	  forceret = 1;
	  if (_add_waitcond (&stmt->u.c, pc)) {
	    if (_stalled_pc == -1) {
#ifdef DUMP_ALL	      
	      printf (" [stall-sh]");
#endif	      
	      _sc->gStall (this);
	      _stalled_pc = pc;
	    }
	    else {
	      forceret = 0;
	    }
	  }
	}
	else {
	  /* loop: we're done! */
	  pc = _updatepc (pc);
	}
      }
    }
    break;

  default:
    fatal_error ("What?");
    break;
  }
  if (forceret || !_pc[pc]) {
#ifdef DUMP_ALL  
  printf (" [f-ret %d]\n", forceret);
#endif
    return;
  }
  else {
#ifdef DUMP_ALL  
    printf (" [NEXT!]\n");
#endif
    new Event (this, SIM_EV_MKTYPE (pc,0), delay);
  }
}

void ChpSim::varSet (int id, int type, expr_res v)
{
  int off = getGlobalOffset (id, type);
  if (type == 0) {
    _sc->setBool (off, v.v);
  }
  else if (type == 1) {
    _sc->setInt (off, v.v);
  }
  else {
    fatal_error ("Use channel send/recv!");
    /* channel! */
  }
  if (id < 0) {
    /*-- non-local variable --*/
    _sc->gWakeup ();
  }
}

/* returns 1 if blocked */
int ChpSim::varSend (int pc, int wakeup, int id, expr_res v)
{
  act_channel_state *c;
  int off = getGlobalOffset (id, 2);
  c = _sc->getChan (off);

  if (c->fragmented) {
    warning ("Need to implement the fragmented channel protocol");
  }

#ifdef DUMP_ALL  
  printf (" [s=%d]", off);
#endif  

  if (wakeup) {
#ifdef DUMP_ALL    
    printf (" [send-wake %d]", pc);
#endif    
    c->send_here = 0;
    Assert (c->sender_probe == 0, "What?");
    return 0;
  }

  if (WAITING_RECEIVER (c)) {
#ifdef DUMP_ALL    
    printf (" [waiting-recv %d]", pc);
#endif    
    // blocked receive, because there was no data
    c->data = v.v;
    c->w->Notify (c->recv_here-1);
    c->recv_here = 0;
    c->send_here = 0;
    c->sender_probe = 0;
    c->receiver_probe = 0;
    return 0;
  }
  else {
#ifdef DUMP_ALL
    printf (" [send-blk %d]", pc);
#endif    
    if (WAITING_RECV_PROBE (c)) {
#ifdef DUMP_ALL      
      printf (" [waiting-recvprobe %d]", pc);
#endif      
      c->probe->Notify (c->recv_here-1);
      c->recv_here = 0;
      c->receiver_probe = 0;
    }
    // we need to wait for the receive to show up
    c->data2 = v.v;
    c->send_here = (pc+1);
    if (!c->w->isWaiting (this)) {
      c->w->AddObject (this);
    }
    return 1;
  }
}

/* returns 1 if blocked */
int ChpSim::varRecv (int pc, int wakeup, int id, expr_res *v)
{
  act_channel_state *c;
  int off = getGlobalOffset (id, 2);
  c = _sc->getChan (off);

  if (c->fragmented) {
    warning ("Need to implement the fragmented channel protocol");
  }
  
#ifdef DUMP_ALL  
  printf (" [r=%d]", off);
#endif
  
  if (wakeup) {
#ifdef DUMP_ALL    
    printf (" [recv-wakeup %d]", pc);
#endif    
    v->v = c->data;
    c->recv_here = 0;
    return 0;
  }
  
  if (WAITING_SENDER (c)) {
#ifdef DUMP_ALL    
    printf (" [waiting-send %d]", pc);
#endif    
    v->v = c->data2;
    c->w->Notify (c->send_here-1);
    c->send_here = 0;
    c->recv_here = 0;
    c->sender_probe = 0;
    c->receiver_probe = 0;
    return 0;
  }
  else {
#ifdef DUMP_ALL
    printf (" [recv-blk %d]", pc);
#endif    
    if (WAITING_SEND_PROBE (c)) {
#ifdef DUMP_ALL      
      printf (" [waiting-sendprobe %d]", pc);
#endif      
      c->probe->Notify (c->send_here-1);
      c->send_here = 0;
      c->sender_probe = 0;
    }
    c->recv_here = (pc+1);
    if (!c->w->isWaiting (this)) {
      c->w->AddObject (this);
    }
    return 1;
  }
  return 0;
}


expr_res ChpSim::varEval (int id, int type)
{
  expr_res r;
  int off;

  off = getGlobalOffset (id, type == 3 ? 2 : type);
  if (type == 0) {
    r.width = 1;
    r.v = _sc->getBool (off);
    if (r.v == 2) {
      warning ("Boolean variable is X");
    }
  }
  else if (type == 1) {
    r.width = 32; /* XXX: need bit-widths */
    r.v = _sc->getInt (off);
  }
  else if (type == 2) {
    act_channel_state *c = _sc->getChan (off);
    if (WAITING_SENDER (c)) {
      r.width = 32;
      r.v = c->data2;
    }
    else {
      /* value probe */
      r.width = 1;
      r.v = 0;
    }
  }
  else {
    /* probe */
    act_channel_state *c = _sc->getChan (off);
    if (WAITING_SENDER (c) || WAITING_RECEIVER (c)) {
      r.width = 1;
      r.v = 1;
    }
    else {
      r.width = 1;
      r.v = 0;
    }
  }
  return r;
}

void ChpSim::_run_chp (act_chp_lang_t *c)
{
  listitem_t *li;
  hash_bucket_t *b;
  expr_res *x, res;
  act_chp_gc_t *gc;
  int rep;
  struct Hashtable *state = ((struct Hashtable *)stack_peek (_statestk));
  
  if (!c) return;
  switch (c->type) {
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (li = list_first (c->u.semi_comma.cmd); li; li = list_next (li)) {
      _run_chp ((act_chp_lang_t *) list_value (li));
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
    gc = c->u.gc;
    if (gc->id) {
      fatal_error ("No replication in functions!");
    }
    while (gc) {
      if (!gc->g) {
	_run_chp (gc->s);
	return;
      }
      res = exprEval (gc->g);
      if (res.v) {
	_run_chp (gc->s);
	return;
      }
      gc = gc->next;
    }
    fatal_error ("All guards false in function selection statement!");
    break;
    
  case ACT_CHP_LOOP:
    gc = c->u.gc;
    if (gc->id) {
      fatal_error ("No replication in functions!");
    }
    while (1) {
      gc = c->u.gc;
      while (gc) {
	if (!gc->g) {
	  _run_chp (gc->s);
	  break;
	}
	res = exprEval (gc->g);
	if (res.v) {
	  _run_chp (gc->s);
	  break;
	}
	gc = gc->next;
      }
      if (!gc) {
	break;
      }
    }
    break;
    
  case ACT_CHP_DOLOOP:
    gc = c->u.gc;
    if (gc->id) {
      fatal_error ("No replication in functions!");
    }
    Assert (gc->next == NULL, "What?");
    do {
      _run_chp (gc->s);
      res = exprEval (gc->g);
    } while (res.v);
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
  case ACT_CHP_FUNC:
    fatal_error ("Functions cannot use send/receive or log!");
    break;
    
  case ACT_CHP_ASSIGN:
    if (c->u.assign.id->Rest()) {
      fatal_error ("Dots not permitted in functions!");
    }
    b = hash_lookup (state, c->u.assign.id->getName());
    if (!b) {
      fatal_error ("Variable `%s' not found?!", c->u.assign.id->getName());
    }
    x = (expr_res *) b->v;
    res = exprEval (c->u.assign.e);
    /* bit-width conversion */
    if (res.width > x->width) {
      x->v = res.v & ((1UL << x->width)-1);
    }
    else {
      x->v = res.v;
    }
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
}

static void *dl_extern_files;

typedef expr_res (*EXTFUNC) (int nargs, expr_res *args);

expr_res ChpSim::funcEval (Function *f, int nargs, expr_res *args)
{
  struct Hashtable *lstate;
  hash_bucket_t *b;
  hash_iter_t iter;
  expr_res *x;
  expr_res ret;
  ActInstiter it(f->CurScope());


  /*-- allocate state and bindings --*/
  lstate = hash_new (4);

  for (it = it.begin(); it != it.end(); it++) {
    ValueIdx *vx = (*it);

    b = hash_add (lstate, vx->getName());
    NEW (x, expr_res);
    x->v = 0;
    x->width = TypeFactory::bitWidth (vx->t);
    if (vx->t->arrayInfo()) {
      warning ("Ignoring arrays for now...");
    }
    b->v = x;
  }

  if (nargs != f->getNumPorts()) {
    fatal_error ("Function `%s': invalid number of arguments", f->getName());
  }

  for (int i=0; i < f->getNumPorts(); i++) {
    b = hash_lookup (lstate, f->getPortName (i));
    Assert (b, "What?");
    x = (expr_res *) b->v;

    x->v = args[i].v;
    if (args[i].width > x->width) {
      x->v &= ((1 << x->width) - 1);
    }
  }

  /* --- run body -- */
  if (f->isExternal()) {
    char buf[10240];
    EXTFUNC extcall = NULL;

    if (!dl_extern_files) {
      if (config_exists ("actsim.extern")) {
	dl_extern_files = dlopen (config_get_string ("actsim.extern"),
				  RTLD_LAZY);
      }
    }
    extcall = NULL;
    snprintf (buf, 10240, "actsim.%s", f->getName());
    buf[strlen(buf)-2] = '\0';

    if (dl_extern_files && config_exists (buf)) {
      extcall = (EXTFUNC) dlsym (dl_extern_files, config_get_string (buf));
      if (!extcall) {
	fatal_error ("Could not find external function `%s'",
		     config_get_string (buf));
      }
    }
    if (!extcall) {
      fatal_error ("Function `%s' missing chp body as well as external definition.", f->getName());
    }
    return (*extcall) (nargs, args);
  }
  
  act_chp *c = f->getlang()->getchp();
  stack_push (_statestk, lstate);
  _run_chp (c->c);
  stack_pop (_statestk);

  /* -- return result -- */
  b = hash_lookup (lstate, "self");
  x = (expr_res *)b->v;
  ret = *x;
  
  hash_iter_init (lstate, &iter);
  while ((b = hash_iter_next (lstate, &iter))) {
    FREE (b->v);
  }
  hash_free (lstate);

  return ret;
}
  
expr_res ChpSim::exprEval (Expr *e)
{
  expr_res l, r;
  Assert (e, "What?!");

  switch (e->type) {

  case E_TRUE:
    l.v = 1;
    l.width = 1;
    return l;
    break;
    
  case E_FALSE:
    l.v = 0;
    l.width = 1;
    return l;
    break;
    
  case E_INT:
    l.v = e->u.v;
    {
      unsigned long x = e->u.v;
      l.width = 0;
      while (x) {
	x = x >> 1;
	l.width++;
      }
      if (l.width == 0) {
	l.width = 1;
      }
    }
    return l;
    break;

  case E_REAL:
    fatal_error ("No real expressions permitted in CHP!");
    return l;
    break;

    /* binary */
  case E_AND:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = MAX(l.width, r.width);
    l.v = l.v & r.v;
    break;

  case E_OR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = MAX(l.width, r.width);
    l.v = l.v | r.v;
    break;

  case E_PLUS:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1 + MAX(l.width, r.width);
    l.v = l.v + r.v;
    break;

  case E_MINUS:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1 + MAX(l.width, r.width);
    l.v = l.v - r.v;
    break;

  case E_MULT:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = l.width + r.width;
    l.v = l.v * r.v;
    break;
    
  case E_DIV:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if (r.v == 0) {
      warning("Division by zero");
    }
    l.v = l.v / r.v;
    break;
      
  case E_MOD:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = r.width;
    l.v = l.v % r.v;
    break;
    
  case E_LSL:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = l.width + (1 << r.width) - 1;
    l.v = l.v << r.v;
    break;
    
  case E_LSR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.v = l.v >> r.v;
    break;
    
  case E_ASR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if ((l.v >> (l.width-1)) & 1) {
      l.v = l.v >> r.v;
      l.v = l.v | (((1U << r.v)-1) << (l.width-r.v));
    }
    else {
      l.v = l.v >> r.v;
    }
    break;
    
  case E_XOR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = MAX(l.width, r.width);
    l.v = l.v ^ r.v;
    break;
    
  case E_LT:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1;
    l.v = (l.v < r.v) ? 1 : 0;
    break;
    
  case E_GT:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1;
    l.v = (l.v > r.v) ? 1 : 0;
    break;

  case E_LE:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1;
    l.v = (l.v <= r.v) ? 1 : 0;
    break;
    
  case E_GE:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1;
    l.v = (l.v >= r.v) ? 1 : 0;
    break;
    
  case E_EQ:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1;
    l.v = (l.v == r.v) ? 1 : 0;
    break;
    
  case E_NE:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.width = 1;
    l.v = (l.v != r.v) ? 1 : 0;
    break;
    
  case E_NOT:
    l = exprEval (e->u.e.l);
    l.v = !l.v;
    break;
    
  case E_UMINUS:
    l = exprEval (e->u.e.l);
    l.v = (1 << l.width) - l.v;
    break;

  case E_COMPLEMENT:
    l = exprEval (e->u.e.l);
    l.v = ((1 << l.width)-1) - l.v;
    break;

  case E_QUERY:
    l = exprEval (e->u.e.l);
    if (l.v) {
      l = exprEval (e->u.e.r->u.e.l);
    }
    else {
      l = exprEval (e->u.e.r->u.e.r);
    }
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    l.v = 0;
    l.width = 0;
    do {
      r = exprEval (e->u.e.l);
      l.width += r.width;
      l.v = (l.v << r.width) | r.v;
      e = e->u.e.r;
    } while (e);
    break;

  case E_BITFIELD:
    {
      int lo, hi;
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);
      
      lo = (long)e->u.e.r->u.e.l;
      hi = (long)e->u.e.r->u.e.r;

      /* is an int */
      l = varEval (off, 1);

      if (hi >= l.width) {
	warning ("Bit-width is less than the width specifier");
      }
      l.width = hi - lo + 1;
      l.v = l.v >> lo;
      l.v = l.v & ((1 << l.width)-1);
    }
    break;
    
  case E_CHP_VARBOOL:
    l = varEval (e->u.x.val, 0);
    break;

  case E_CHP_VARINT:
    l = varEval (e->u.x.val, 1);
    break;

  case E_CHP_VARCHAN:
    l = varEval (e->u.x.val, 2);
    break;
    
  case E_CHP_VARBOOL_DEREF:
    {
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);
      l = varEval (off, 0);
    }
    break;

  case E_CHP_VARINT_DEREF:
    {
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);
      l = varEval (off, 1);
    }
    break;

  case E_VAR:
    {
      Assert (!list_isempty (_statestk), "What?");
      struct Hashtable *state = ((struct Hashtable *)stack_peek (_statestk));
      Assert (state,"what?");
      hash_bucket_t *b = hash_lookup (state, ((ActId*)e->u.e.l)->getName());
      Assert (b, "what?");
      l = *((expr_res *)b->v);
    }
    break;

  case E_PROBE:
    fatal_error ("E_PROBE-2");
  case E_PROBEIN:
  case E_PROBEOUT:
    l = varEval (e->u.x.val, 3);
    break;

  case E_BUILTIN_BOOL:
    l = exprEval (e->u.e.l);
    if (l.v) {
      l.v = 1;
      l.width = 1;
    }
    else {
      l.v = 0;
      l.width = 1;
    }
    break;
    
  case E_BUILTIN_INT:
    l = exprEval (e->u.e.l);
    if (e->u.e.r) {
      r = exprEval (e->u.e.r);
    }
    else {
      r.v = 1;
    }
    l.width = r.v;
    l.v = l.v & ((1 << r.v) - 1);
    break;

  case E_FUNCTION:
    /* function is e->u.fn.s */
    {
      Expr *tmp = NULL;
      int nargs = 0;
      int i;
      expr_res *args;

      /* first: evaluate arguments */
      tmp = e->u.fn.r;
      while (tmp) {
	nargs++;
	tmp = tmp->u.e.r;
      }
      MALLOC (args, expr_res, nargs);
      tmp = e->u.fn.r;
      i = 0;
      while (tmp) {
	args[i] = exprEval (tmp->u.e.l);
	i++;
	tmp = tmp->u.e.r;
      }
      l = funcEval ((Function *)e->u.fn.s, nargs, args);
    }
    break;

  case E_SELF:
  default:
    l.v = 0;
    l.width = 1;
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  if (l.width <= 0) {
    warning ("Negative width?");
    l.width = 1;
    l.v = 0;
  }
  return l;
}

int ChpSim::_max_program_counters (act_chp_lang_t *c)
{
  int ret, val;

  if (!c) return 1;
  
  switch (c->type) {
  case ACT_CHP_SEMI:
    ret = 1;
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      val = _max_program_counters (t);
      if (val > ret) {
	ret = val;
      }
    }
    break;

  case ACT_CHP_COMMA:
    ret = 0;
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ret += _max_program_counters (t);
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
  case ACT_CHP_DOLOOP:
    ret = 0;
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      val = _max_program_counters (gc->s);
      if (val > ret) {
	ret = val;
      }
    }
    break;

  case ACT_CHP_SKIP:
  case ACT_CHP_ASSIGN:
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
  case ACT_CHP_FUNC:
    ret = 1;
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
  return ret;
}

void ChpSim::_compute_used_variables (act_chp_lang_t *c)
{
  ihash_iter_t iter;
  ihash_bucket_t *b;
  _tmpused = ihash_new (4);
  _compute_used_variables_helper (c);
  ihash_iter_init (_tmpused, &iter);
  //printf ("going through...\n");
  while ((b = ihash_iter_next (_tmpused, &iter))) {
    if (b->i == 0 || b->i == 1) {
      int off = getGlobalOffset (b->key, b->i);
      _sc->incFanout (off, b->i, this);
    }
  }
  ihash_free (_tmpused);
  //printf ("done.\n");
  _tmpused = NULL;
}


static void _mark_vars_used (ActSimCore *_sc, ActId *id, struct iHashtable *H)
{
  int sz, loff;
  int type;
  ihash_bucket_t *b;

  if (id->isDynamicDeref()) {
    /* mark the entire array as used */
    ValueIdx *vx = id->rootVx (_sc->cursi()->bnl->cur);
    int sz;
    Assert (vx->connection(), "Hmm");
    loff = _sc->getLocalOffset (vx->connection()->primary(),
				_sc->cursi(), &type);
    sz = vx->t->arrayInfo()->size();
    while (sz > 0) {
      sz--;
      if (!ihash_lookup (H, loff+sz)) {
	b = ihash_add (H, loff+sz);
	b->i = type;
      }
    }
  }
  else {
    loff = _sc->getLocalOffset (id, _sc->cursi(), &type);
    if (!ihash_lookup (H, loff)) {
      b = ihash_add (H, loff);
      b->i = type;
    }
  }
}

void ChpSim::_compute_used_variables_helper (Expr *e)
{
  int loff, type;
  ihash_bucket_t *b;
      
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
    _compute_used_variables_helper (e->u.e.l);
    _compute_used_variables_helper (e->u.e.r);
    break;
    
  case E_NOT:
  case E_UMINUS:
  case E_COMPLEMENT:
    _compute_used_variables_helper (e->u.e.l);
    break;

  case E_QUERY:
    _compute_used_variables_helper (e->u.e.l);
    _compute_used_variables_helper (e->u.e.r->u.e.l);
    _compute_used_variables_helper (e->u.e.r->u.e.r);
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    while (e) {
      _compute_used_variables_helper (e->u.e.l);
      e = e->u.e.r;
    }
    break;

  case E_BITFIELD:
    /* l is an Id */
    _mark_vars_used (_sc, (ActId *)e->u.e.l, _tmpused);
    break;

  case E_TRUE:
  case E_FALSE:
  case E_INT:
  case E_REAL:
    break;

  case E_VAR:
    _mark_vars_used (_sc, (ActId *)e->u.e.l, _tmpused);
    break;

  case E_PROBE:
    break;
    
  case E_FUNCTION:
    {
      Expr *tmp = NULL;
      e = e->u.fn.r;
      while (e) {
	_compute_used_variables_helper (e->u.e.l);
	e = e->u.e.r;
      }
    }
    break;

  case E_SELF:
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
}

void ChpSim::_compute_used_variables_helper (act_chp_lang_t *c)
{
  int loff, type;
  ihash_bucket_t *b;

  if (!c) return;
  
  switch (c->type) {
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      _compute_used_variables_helper (t);
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
  case ACT_CHP_DOLOOP:
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      _compute_used_variables_helper (gc->g);
      _compute_used_variables_helper (gc->s);
    }
    break;

  case ACT_CHP_SKIP:
  case ACT_CHP_FUNC:
    break;

  case ACT_CHP_ASSIGN:
    _mark_vars_used (_sc, c->u.assign.id, _tmpused);
    _compute_used_variables_helper (c->u.assign.e);
    break;
    
  case ACT_CHP_SEND:
    for (listitem_t *li = list_first (c->u.comm.rhs); li; li = list_next (li)) {
      _compute_used_variables_helper ((Expr *)list_value (li));
    }
    break;
    
  case ACT_CHP_RECV:
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
}




ChpSimGraph::ChpSimGraph (ActSimCore *s)
{
  state = s;
  stmt = NULL;
  next = NULL;
  all = NULL;
  wait = 0;
  tot = 0;
}


ChpSimGraph *ChpSimGraph::completed (int pc, int *done)
{
  *done = 0;
  if (!next) {
    return NULL;
  }
  if (next->wait > 0) {
    next->tot++;
    if (next->wait == next->tot) {
      *done = 1;
      next->tot = 0;
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

static Expr *expr_to_chp_expr (Expr *e, ActSimCore *s);

static struct chpsimderef *_mk_deref (ActId *id, ActSimCore *s, int *type)
{
  struct chpsimderef *d;
  Scope *sc = s->cursi()->bnl->cur;

  ValueIdx *vx = id->rootVx (sc);
  Assert (vx->connection(), "What?");

  NEW (d, struct chpsimderef);

  d->cx = vx->connection();
  d->offset = s->getLocalOffset (vx->connection()->primary(),
				 s->cursi(), type);

  d->range = vx->t->arrayInfo();
  Assert (d->range, "What?");
  Assert (d->range->nDims() > 0, "What?");
  MALLOC (d->idx, int, d->range->nDims());
  MALLOC (d->chp_idx, Expr *, d->range->nDims());
  
  /* now convert array deref into a chp array deref! */
  for (int i = 0; i < d->range->nDims(); i++) {
    d->chp_idx[i] = expr_to_chp_expr (id->arrayInfo()->getDeref(i), s);
    d->idx[i] = -1;
  }

  return d;
}



/*------------------------------------------------------------------------
 * The CHP simulation graph
 *------------------------------------------------------------------------
 */

static Expr *expr_to_chp_expr (Expr *e, ActSimCore *s)
{
  Expr *ret, *tmp;
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
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s);
    ret->u.e.r = expr_to_chp_expr (e->u.e.r, s);
    break;
    
  case E_NOT:
  case E_UMINUS:
  case E_COMPLEMENT:
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s);
    ret->u.e.r = NULL;
    break;

  case E_QUERY:
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s);
    NEW (ret->u.e.r, Expr);
    ret->u.e.r->type = e->u.e.r->type;
    ret->u.e.r->u.e.l = expr_to_chp_expr (e->u.e.r->u.e.l, s);
    ret->u.e.r->u.e.r = expr_to_chp_expr (e->u.e.r->u.e.r, s);
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
	tmp->u.e.l = expr_to_chp_expr (e->u.e.l, s);
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

      if (((ActId *)e->u.e.l)->isDynamicDeref()) {
	d = _mk_deref ((ActId *)e->u.e.l, s, &type);
      }
      else {
	NEW (d, struct chpsimderef);
	d->range = NULL;
	d->offset = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), &type);
	d->cx = ((ActId *)e->u.e.l)->Canonical (s->cursi()->bnl->cur);
      }
      ret->u.e.l = (Expr *) d;
      NEW (ret->u.e.r, Expr);
      ret->u.e.r->u.e.l = e->u.e.r->u.e.l;
      ret->u.e.r->u.e.r = e->u.e.r->u.e.r;
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
      int type;

      if (((ActId *)e->u.e.l)->isDynamicDeref()) {
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
      else {
	ret->u.x.val = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), &type);
	ret->u.x.extra = (unsigned long) ((ActId *)e->u.e.l)->Canonical (s->cursi()->bnl->cur);
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
	tmp->u.e.l = expr_to_chp_expr (e->u.e.l, s);
	e = e->u.e.r;
      }
    }
    break;

  case E_SELF:
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  return ret;
}

static chpsimstmt *gc_to_chpsim (act_chp_gc_t *gc, ActSimCore *s)
{
  chpsimcond *tmp;
  chpsimstmt *ret;
  
  ret = NULL;
  if (!gc) return ret;

  NEW (ret, chpsimstmt);
  ret->type = CHPSIM_COND;
  tmp = NULL;
  
  while (gc) {
    if (!tmp) {
      tmp = &ret->u.c;
    }
    else {
      NEW (tmp->next, chpsimcond);
      tmp = tmp->next;
    }
    tmp->next = NULL;
    tmp->g = expr_to_chp_expr (gc->g, s);
    gc = gc->next;
  }
  return ret;
}


ChpSimGraph *ChpSimGraph::buildChpSimGraph (ActSimCore *sc,
					    act_chp_lang_t *c,
					    ChpSimGraph **stop)
{
  ChpSimGraph *ret = NULL;
  ChpSimGraph *tmp2;
  int i, count;
  
  if (!c) return NULL;

  switch (c->type) {
  case ACT_CHP_SEMI:
    if (list_length (c->u.semi_comma.cmd)== 1) {
      return buildChpSimGraph
	(sc,
	 (act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop);
    }
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ChpSimGraph *tmp = buildChpSimGraph (sc, t, &tmp2);
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
    }
    break;

  case ACT_CHP_COMMA:
    if (list_length (c->u.semi_comma.cmd)== 1) {
      return buildChpSimGraph
	(sc,
	 (act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop);
    }
    ret = new ChpSimGraph (sc);
    *stop = new ChpSimGraph (sc);
    ret->next = *stop; // not sure we need this, but this is the fork/join
		       // connection

    count = 0;
    MALLOC (ret->all, ChpSimGraph *, list_length (c->u.semi_comma.cmd));
    i = 0;
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ret->all[i] = buildChpSimGraph (sc,
				      (act_chp_lang_t *)list_value (li), &tmp2);
      if (ret->all[i]) {
	tmp2->next = *stop;
	count++;
      }
      i++;
    }
    if (count > 0) {
      NEW (ret->stmt, chpsimstmt);
      ret->stmt->type = CHPSIM_FORK;
      ret->stmt->u.fork = count;
      (*stop)->wait = count;
    }
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
    ret = new ChpSimGraph (sc);
    ret->stmt = gc_to_chpsim (c->u.gc, sc);
    i = 0;
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      i++;
    }
    Assert (i >= 1, "What?");
    MALLOC (ret->all, ChpSimGraph *, i);
      
    (*stop) = new ChpSimGraph (sc);

    if (c->type == ACT_CHP_LOOP) {
      ret->next = (*stop);
    }
    i = 0;
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      ret->all[i] = buildChpSimGraph (sc, gc->s, &tmp2);
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
    }
    break;
    
  case ACT_CHP_DOLOOP:
    ret = new ChpSimGraph (sc);
    ret->stmt = gc_to_chpsim (c->u.gc, sc);
    (*stop) = new ChpSimGraph (sc);
    ret->next = (*stop);
    MALLOC (ret->all, ChpSimGraph *, 1);
    ret->all[0] = buildChpSimGraph (sc, c->u.gc->s, &tmp2);
    tmp2->next = ret;
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
    ret = new ChpSimGraph (sc);
    NEW (ret->stmt, chpsimstmt);
    ret->stmt->type = CHPSIM_SEND;
    {
      listitem_t *li;
      ret->stmt->u.send.el = list_new ();
      for (li = list_first (c->u.comm.rhs); li; li = list_next (li)) {
	Expr *e = (Expr *) list_value (li);
	list_append (ret->stmt->u.send.el, expr_to_chp_expr (e, sc));
      }
    }    
    ret->stmt->u.send.chvar = sc->getLocalOffset (c->u.comm.chan, sc->cursi(), NULL);
    ret->stmt->u.send.vc = c->u.comm.chan->Canonical (sc->cursi()->bnl->cur);
    (*stop) = ret;
    break;
    
    
  case ACT_CHP_RECV:
    ret = new ChpSimGraph (sc);
    NEW (ret->stmt, chpsimstmt);
    ret->stmt->type = CHPSIM_RECV;
    {
      listitem_t *li;
      ret->stmt->u.recv.vl = list_new ();
      for (li = list_first (c->u.comm.rhs); li; li = list_next (li)) {
	ActId *id = (ActId *) list_value (li);
	int type;
	struct chpsimderef *d;

	if (id->isDynamicDeref()) {
	  d = _mk_deref (id, sc, &type);
	}
	else {
	  NEW (d, struct chpsimderef);
	  d->range = NULL;
	  d->offset = sc->getLocalOffset (id, sc->cursi(), &type);
	  d->cx = id->Canonical (sc->cursi()->bnl->cur);
	}
	if (type == 3) {
	  type = 2;
	}
	list_append (ret->stmt->u.recv.vl, (void *)(long)type);
	//list_append (ret->stmt->u.recv.vl, (void *)(long)x);
	list_append (ret->stmt->u.recv.vl, (void *)d);
      }
    }
    ret->stmt->u.recv.chvar = sc->getLocalOffset (c->u.comm.chan, sc->cursi(), NULL);
    ret->stmt->u.recv.vc = c->u.comm.chan->Canonical (sc->cursi()->bnl->cur);
    (*stop) = ret;
    break;

  case ACT_CHP_FUNC:
    if (strcmp (string_char (c->u.func.name), "log") != 0) {
      warning ("Built-in function `%s' is not known; valid values: log",
	       c->u.func.name);
    }
    else {
      listitem_t *li;
      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);
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
	  x->u.e = expr_to_chp_expr (tmp->u.e, sc);
	}
	list_append (ret->stmt->u.fn.l, x);
      }
      (*stop) = ret;
    }
    break;
    
  case ACT_CHP_ASSIGN:
    ret = new ChpSimGraph (sc);
    NEW (ret->stmt, chpsimstmt);
    ret->stmt->type = CHPSIM_ASSIGN;
    ret->stmt->u.assign.e = expr_to_chp_expr (c->u.assign.e, sc);
    {
      int type;

      if (c->u.assign.id->isDynamicDeref()) {
	struct chpsimderef *d = _mk_deref (c->u.assign.id, sc, &type);
	ret->stmt->u.assign.d = *d;
	FREE (d);
      }
      else {
	ret->stmt->u.assign.d.range = NULL;
	ret->stmt->u.assign.d.offset =
	  sc->getLocalOffset (c->u.assign.id, sc->cursi(), &type);
	ret->stmt->u.assign.d.cx =
	  c->u.assign.id->Canonical (sc->cursi()->bnl->cur);
      }
      if (type == 1) {
	ret->stmt->u.assign.isbool = 0;
      }
      else {
	Assert (type == 0, "Typechecking?!");
	ret->stmt->u.assign.isbool = 1;
      }
    }
    (*stop) = ret;
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
  return ret;
}

void ChpSimGraph::checkFragmentation (ActSimCore *sc, ChpSim *c, ActId *id)
{
  if (id->isFragmented (sc->cursi()->bnl->cur)) {
    ActId *tmp = id->unFragment (sc->cursi()->bnl->cur);
    /*--  tmp is the unfragmented identifier --*/

    int type;
    int loff = sc->getLocalOffset (tmp, sc->cursi(), &type);

    if (type == 2) {
      /* fragmented channel */
      /* need the global offset here */

      loff = c->getGlobalOffset (loff, type);
      act_channel_state *ch = sc->getChan (loff);
      ch->fragmented = 1;
    }
    delete tmp;
  }
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
    checkFragmentation (sc, c, (ActId *)e->u.e.l);
    break;

  case E_VAR:
    checkFragmentation (sc, c, (ActId *)e->u.e.l);
    break;

  case E_PROBE:
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
    for (li = list_first (c->u.comm.rhs); li; li = list_next (li)) {
      Expr *e = (Expr *) list_value (li);
      checkFragmentation (sc, cc, e);
    }    
    break;
    
  case ACT_CHP_RECV:
    for (li = list_first (c->u.comm.rhs); li; li = list_next (li)) {
      ActId *id = (ActId *) list_value (li);
      checkFragmentation (sc, cc, id);
    }
    break;

  case ACT_CHP_FUNC:
    break;
    
  case ACT_CHP_ASSIGN:
    checkFragmentation (sc, cc, c->u.assign.id);
    checkFragmentation (sc, cc, c->u.assign.e);
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
}
