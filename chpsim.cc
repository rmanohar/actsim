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
#include <common/config.h>
#include "chpsim.h"
#include <common/simdes.h>
#include <dlfcn.h>

class ChpSim;

#ifndef MAX
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#endif

int ChpSimGraph::cur_pending_count = 0;
int ChpSimGraph::max_pending_count = 0;

//#define DUMP_ALL

#define WAITING_SENDER(c)  ((c)->send_here != 0 && (c)->sender_probe == 0)
#define WAITING_SEND_PROBE(c)  ((c)->send_here != 0 && (c)->sender_probe == 1)

#define WAITING_RECEIVER(c)  ((c)->recv_here != 0 && (c)->receiver_probe == 0)
#define WAITING_RECV_PROBE(c)  ((c)->recv_here != 0 && (c)->receiver_probe == 1)


static void _get_costs (stateinfo_t *si, ActId *id, chpsimstmt *stmt)
{
  char buf[1024];

  snprintf (buf, 1024, "sim.chp.%s.%s.D", si->bnl->p->getName(),
	    id->getName());
  if (config_exists (buf)) {
    stmt->delay_cost = config_get_int (buf);
  }
  else {
    // default delay is 10
    stmt->delay_cost = config_get_int ("sim.chp.default_delay");
  }

  snprintf (buf, 1024, "sim.chp.%s.%s.E", si->bnl->p->getName(),
	    id->getName());
  if (config_exists (buf)) {
    stmt->energy_cost = config_get_int (buf);
  }
  else {
    // default energy is 0
    stmt->energy_cost = 0;
  }
}


ChpSim::ChpSim (ChpSimGraph *g, int max_cnt, act_chp_lang_t *c, ActSimCore *sim, Process *p)
: ActSimObj (sim, p)
{
  char buf[1024];

  _stalled_pc = -1;
  _probe = NULL;
  _savedc = c;
  _energy_cost = 0;
  _leakage_cost = 0.0;
  _area_cost = 0;
  _statestk = NULL;

  if (p) {
    snprintf (buf, 1024, "sim.chp.%s.leakage", p->getName());
    if (config_exists (buf)) {
      _leakage_cost = config_get_real (buf);
    }
    snprintf (buf, 1024, "sim.chp.%s.area", p->getName());
    if (config_exists (buf)) {
      _area_cost = config_get_int (buf);
    }
  }
  
  if (c) {
    /*
      Analyze the chp body to find out the maximum number of concurrent
      threads; those are the event types.
    */
    _npc = _max_program_counters (c);
    _pcused = 1;
    Assert (_npc >= 1, "What?");

    if (_npc > 32) {
      fatal_error ("Currently there is a hard limit of 32 concurrent modules within a single CHP block. Your program requires %d.", _npc);
    }

    _pc = (ChpSimGraph **)
      sim->getState()->allocState (sizeof (ChpSimGraph *)*_npc);
    for (int i=0; i < _npc; i++) {
      _pc[i] = NULL;
    }

    if (max_cnt > 0) {
      _tot = (int *)sim->getState()->allocState (sizeof (int)*max_cnt);
      for (int i=0; i < max_cnt; i++) {
	_tot[i] = 0;
      }
    }
    else {
      _tot = NULL;
    }

    Assert (_npc >= 1, "What?");
    _pc[0] = g;
    _statestk = list_new ();
    _nextEvent (0);
  }
  else {
    _pc = NULL;
    _npc = 0;
  }
}

void ChpSim::reStart (ChpSimGraph *g, int max_cnt)
{
  for (int i=0; i < _npc; i++) {
    _pc[i] = NULL;
  }
  for (int i=0; i < max_cnt; i++) {
    _tot[i] = 0;
  }
  _pc[0] = g;
  _nextEvent (0);
}
		     

ChpSim::~ChpSim()
{
  if (_statestk) {
    list_free (_statestk);
  }
}

int ChpSim::_nextEvent (int pc)
{
  while (_pc[pc] && !_pc[pc]->stmt) {
    pc = _updatepc (pc);
  }
  if (_pc[pc]) {
    new Event (this, SIM_EV_MKTYPE (pc,0) /* pc */, _pc[pc]->stmt->delay_cost);
    return 1;
  }
  return 0;
}

void ChpSim::computeFanout ()
{
  if (_savedc) {
    _compute_used_variables (_savedc);
  }
}

int ChpSim::_updatepc (int pc)
{
  int joined;
  
  _pc[pc] = _pc[pc]->completed(pc, _tot, &joined);
  if (joined) {
#ifdef DUMP_ALL
    printf (" [joined #%d / %d %d]", pc, _pcused, _pc[pc]->wait);
#endif    
    _pcused = _pcused - (_pc[pc]->wait - 1);
  }
  if (pc >= _pcused) {
    ChpSimGraph *tmp = _pc[pc];
    _pc[pc] = NULL;
    pc = _pcused - 1;
    _pc[pc] = tmp;
#ifdef DUMP_ALL
    printf (" [pc-adjust %d]", pc);
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

  case E_CHP_BITFIELD:
    if (((struct chpsimderef *)e->u.e.l)->offset < 0) {
      ret = 1;
    }
    break;

  case E_CHP_VARBOOL:
  case E_CHP_VARINT:
  case E_VAR:
    if ((signed)e->u.x.val < 0) {
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
#ifdef DUMP_ALL
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
    fprintf (stderr, "In: ");
    getName()->Print (stderr);
    fprintf (stderr, "  [ %s ]\n", _proc ? _proc->getName() : "-global-");
    fprintf (stderr, "\tAccessing index ");
    for (int i=0; i < d->range->nDims(); i++) {
      fprintf (stderr, "[%d]", d->idx[i]);
    }
    fprintf (stderr, " from ");
    d->cx->toid()->Print (stderr);
    fprintf (stderr, "\n");
    fatal_error ("Array out of bounds!");
  }
  return d->offset + x;
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
    fprintf (fp, "log");
    break;

  case CHPSIM_COND:
    fprintf (fp, "cond: ");
    break;

  case CHPSIM_LOOP:
    fprintf (fp, "loop: ");
    break;

  default:
    fatal_error ("What?");
    break;
  }
}


void ChpSim::Step (int ev_type)
{
  int pc = SIM_EV_TYPE (ev_type);
  int flag = SIM_EV_FLAGS (ev_type);
  int forceret = 0;
  int joined;
  expr_res v;
  int off;

  Assert (0 <= pc && pc < _pcused, "What?");

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
    _energy_cost += stmt->energy_cost;
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
	  _nextEvent (idx);
	}
      }
      _pcused += stmt->u.fork - 1;
#ifdef DUMP_ALL
      printf (" _used:%d", _pcused);
#endif      
    }
    break;

  case CHPSIM_ASSIGN:
    _energy_cost += stmt->energy_cost;
#ifdef DUMP_ALL
    printf ("assign v[%d] := ", stmt->u.assign.d.offset);
#endif
    v = exprEval (stmt->u.assign.e);
#ifdef DUMP_ALL    
    printf ("%lu (w=%d)", v.v, v.width);
#endif
    pc = _updatepc (pc);
    off = computeOffset (&stmt->u.assign.d);
    if (stmt->u.assign.isint == 0) {
      off = getGlobalOffset (off, 0);
#if 0      
      printf (" [glob=%d]", off);
#endif      
      _sc->setBool (off, v.v);

      SimDES **arr;
      arr = _sc->getFO (off, 0);
      for (int i=0; i < _sc->numFanout (off, 0); i++) {
	ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
	Assert (p, "What?");
	p->propagate ();
      }
    }
    else {
      off = getGlobalOffset (off, 1);
#if 0      
      printf (" [glob=%d]", off);
#endif
      if (v.width > stmt->u.assign.isint) {
	if (stmt->u.assign.isint < 64) {
	  v.v = ((unsigned long)v.v & ((1UL << stmt->u.assign.isint)-1));
	}
      }
      _sc->setInt (off, v.v);
    }
    break;

  case CHPSIM_SEND:
    if (!flag) {
      /*-- this is the first time we are at the send, so evaluate the
           expression being sent over the channel --*/
      
      listitem_t *li;
      if (stmt->u.sendrecv.e) {
	v = exprEval (stmt->u.sendrecv.e);
      }
      else {
	/* data-less */
	v.v = 0;
	v.width = 0;
      }
#ifdef DUMP_ALL      
      printf ("send val=%lu", v.v);
#endif
    }
    /*-- attempt to send; suceeds if there is a receiver waiting,
      otherwise we have to wait for the receiver --*/
    if (varSend (pc, flag, stmt->u.sendrecv.chvar, v)) {
      /* blocked */
      forceret = 1;
#ifdef DUMP_ALL      
      printf ("send blocked");
#endif      
    }
    else {
      /* no blocking, so we move forward */
      pc = _updatepc (pc);
#ifdef DUMP_ALL      
      printf ("send done");
#endif      
      _energy_cost += stmt->energy_cost;
    }
    break;

  case CHPSIM_RECV:
    {
      listitem_t *li;
      struct chpsimderef *d;
      int id;
      int type;
      int width;
      if (stmt->u.sendrecv.d) {
	type = stmt->u.sendrecv.d_type;
	d = stmt->u.sendrecv.d;
	id = computeOffset (d);
	width = d->width;
      }
      else {
	type = -1;
      }

      /*-- attempt to receive value --*/
      if (varRecv (pc, flag, stmt->u.sendrecv.chvar, &v)) {
	/*-- blocked, we have to wait for the sender --*/
#ifdef DUMP_ALL	
	printf ("recv blocked");
#endif	
	forceret = 1;
      }
      else {
	/*-- success! --*/
#ifdef DUMP_ALL	
	printf ("recv got %lu!", v.v);
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
	    if (width < 64) {
	      v.v = ((unsigned long)v.v) & ((1UL << width)-1);
	    }
	    _sc->setInt (off, v.v);
	  }
	}
	pc = _updatepc (pc);
	_energy_cost += stmt->energy_cost;
      }
    }
    break;

  case CHPSIM_FUNC:
    _energy_cost += stmt->energy_cost;
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
	if (v.width < 64) {
	  v.v = v.v << (64-v.width);
	  v.v = v.v >> (64-v.width);
	}
	printf (ACT_EXPR_RES_PRINTF, v.v);
      }
    }
    printf ("\n");
    pc = _updatepc (pc);
    break;

  case CHPSIM_COND:
  case CHPSIM_LOOP:
    {
      chpsimcond *gc;
      int cnt = 0;
      expr_res res;

#ifdef DUMP_ALL
      if (stmt->type == CHPSIM_COND) {
	printf ("cond");
      }
      else {
	printf ("loop");
      }
#endif
      _energy_cost += stmt->energy_cost;
      
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
	if (stmt->type == CHPSIM_COND) {
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
    _nextEvent (pc);
  }
}


/* returns 1 if blocked */
int ChpSim::varSend (int pc, int wakeup, int id, expr_res v)
{
  act_channel_state *c;
  int off = getGlobalOffset (id, 2);
  c = _sc->getChan (off);

  if (c->fragmented) {
    switch (c->frag_st) {
    case 0:
      c->frag_st = 1;
      c->ufrag_st = 0;
    case 1:
      /* send */
      

      break;
    case 2:
      /* send_up */

      break;
    case 3:
      /* in send_rest, advance through */
      break;

    default:
      fatal_error ("What is this?");
      break;
    }
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
    if (c->send_here != 0) {
      act_connection *x;
      int dy;
      fprintf (stderr, "Process %s: concurrent access to channel `",
	       _proc ? _proc->getName() : "-global-");
      x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "'\n");
      fprintf (stderr, "Instance: ");
      getName()->Print (stderr);
      fprintf (stderr, "\n");
    }
    Assert (c->send_here == 0 && c->sender_probe == 0 &&
	    c->receiver_probe == 0,"What?");
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
    if (c->send_here != 0) {
      act_connection *x;
      int dy;
      fprintf (stderr, "Process %s: concurrent access to channel `",
	       _proc ? _proc->getName() : "-global-");
      x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "'\n");
      fprintf (stderr, "Instance: ");
      getName()->Print (stderr);
      fprintf (stderr, "\n");
    }
    Assert (c->send_here == 0, "What?");
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
    if (c->recv_here != 0) {
      act_connection *x;
      int dy;
      fprintf (stderr, "Process %s: concurrent access to channel `",
	       _proc ? _proc->getName() : "-global-");
      x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "'\n");
      fprintf (stderr, "Instance: ");
      getName()->Print (stderr);
      fprintf (stderr, "\n");
    }
    Assert (c->recv_here == 0, "What?");
    return 0;
  }
  
  if (WAITING_SENDER (c)) {
#ifdef DUMP_ALL    
    printf (" [waiting-send %d]", pc);
#endif    
    v->v = c->data2;
    c->w->Notify (c->send_here-1);
    c->send_here = 0;
    Assert (c->recv_here == 0 && c->receiver_probe == 0 &&
	    c->sender_probe == 0, "What?");
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
    if (c->recv_here != 0) {
      act_connection *x;
      int dy;
      fprintf (stderr, "Process %s: concurrent access to channel `",
	       _proc ? _proc->getName() : "-global-");
      x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "'\n");
      fprintf (stderr, "Instance: ");
      getName()->Print (stderr);
      fprintf (stderr, "\n");
    }
    Assert (c->recv_here == 0, "What?");
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
#if 0      
      fprintf (stderr, "[%8lu] <", CurTimeLo());
      name->Print (stderr);
      fprintf (stderr, "> ");
#endif      
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
    fatal_error ("Functions cannot use send/receive");
    break;
    
  case ACT_CHP_FUNC:
    if (strcmp (string_char (c->u.func.name), "log") != 0) {
      warning ("Built-in function `%s' is not known; valid values: log",
	       string_char (c->u.func.name));
    }
    else {
      listitem_t *li;
      for (li = list_first (c->u.func.rhs); li; li = list_next (li)) {
	act_func_arguments_t *tmp = (act_func_arguments_t *)list_value (li);
	if (tmp->isstring) {
	  printf ("%s", string_char (tmp->u.s));
	}
	else {
	  expr_res v = exprEval (tmp->u.e);
	  printf (ACT_EXPR_RES_PRINTF, v.v);
	}
      }
      printf ("\n");
    }
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
    if (TypeFactory::isParamType (vx->t)) continue;

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
    l.v = ~l.v;
    break;
    
  case E_UMINUS:
    l = exprEval (e->u.e.l);
    l.v = (1 << l.width) - l.v;
    break;

  case E_COMPLEMENT:
    l = exprEval (e->u.e.l);
    if (l.width < 64) {
      l.v = ((1UL << l.width)-1) - l.v;
    }
    else {
      l.v = ~l.v;
    }
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

  case E_CHP_BITFIELD:
    {
      int lo, hi;
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);

      hi = (long)e->u.e.r->u.e.r->u.v;
      if (e->u.e.r->u.e.l) {
	lo = (long)e->u.e.r->u.e.l->u.v;
      }
      else {
	lo = hi;
      }

      /* is an int */
      l = varEval (off, 1);
      l.width = ((struct chpsimderef *)e->u.e.l)->width;

      if (hi >= l.width) {
	warning ("Bit-width is less than the width specifier");
      }
      l.width = hi - lo + 1;
      l.v = l.v >> lo;
      l.v = l.v & ((1UL << l.width)-1);
    }
    break;
    
  case E_CHP_VARBOOL:
    l = varEval (e->u.x.val, 0);
    l.width = 1;
    break;

  case E_CHP_VARINT:
    l = varEval (e->u.x.val, 1);
    l.width = e->u.x.extra;
    break;

  case E_CHP_VARCHAN:
    l = varEval (e->u.x.val, 2);
    l.width = e->u.x.extra;
    break;
    
  case E_CHP_VARBOOL_DEREF:
    {
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);
      l = varEval (off, 0);
      l.width = ((struct chpsimderef *)e->u.e.l)->width;
    }
    break;

  case E_CHP_VARINT_DEREF:
    {
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);
      l = varEval (off, 1);
      l.width = ((struct chpsimderef *)e->u.e.l)->width;
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

  case E_BITFIELD:
    {
      int lo, hi;

      Assert (!list_isempty (_statestk), "What?");
      struct Hashtable *state = ((struct Hashtable *)stack_peek (_statestk));
      Assert (state,"what?");
      hash_bucket_t *b = hash_lookup (state, ((ActId*)e->u.e.l)->getName());
      Assert (b, "what?");
      l = *((expr_res *)b->v);

      hi = (long)e->u.e.r->u.e.r->u.v;
      if (e->u.e.r->u.e.l) {
	lo = (long)e->u.e.r->u.e.l->u.v;
      }
      else {
	lo = hi;
      }

      if (hi >= l.width) {
	warning ("Bit-width is less than the width specifier");
      }
      l.width = hi - lo + 1;
      l.v = l.v >> lo;
      l.v = l.v & ((1UL << l.width)-1);
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
    int off = getGlobalOffset (b->key, b->i < 2 ? b->i : 2);
    if (b->i == 0 || b->i == 1) {
      _sc->incFanout (off, b->i, this);
    }
    else {
      act_channel_state *ch = _sc->getChan (off);
      Assert (ch, "Hmm");
      if (ch->fragmented) {
	ihash_bucket_t *ib;
	ihash_iter_t ih;
	ihash_iter_init (ch->fH, &ih);
	while ((ib = ihash_iter_next (ch->fH, &ih))) {
	  _sc->incFanout (ib->i, 0, this);
	}
      }
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

  if (ActBooleanizePass::isDynamicRef (_sc->cursi()->bnl, id)) {
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
    _mark_vars_used (_sc, c->u.comm.chan, _tmpused);
    if (c->u.comm.e) {
      _compute_used_variables_helper (c->u.comm.e);
    }
    if (c->u.comm.var) {
      _mark_vars_used (_sc, c->u.comm.var, _tmpused);
    }
    break;
    
  case ACT_CHP_RECV:
    _mark_vars_used (_sc, c->u.comm.chan, _tmpused);
    if (c->u.comm.e) {
      _compute_used_variables_helper (c->u.comm.e);
    }
    if (c->u.comm.var) {
      _mark_vars_used (_sc, c->u.comm.var, _tmpused);
    }
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
  totidx = 0;
#if 0  
  printf ("alloc %p\n", this);
#endif  
}




ChpSimGraph *ChpSimGraph::completed (int pc, int *tot, int *done)
{
  *done = 0;
  if (!next || (stmt && stmt->type == CHPSIM_COND)) {
    return NULL;
  }
  if (next->wait > 0) {
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

static Expr *expr_to_chp_expr (Expr *e, ActSimCore *s);
static void _free_chp_expr (Expr *e);

static void _free_deref (struct chpsimderef *d)
{
  if (d->range) {
    for (int i=0; i < d->range->nDims(); i++) {
      _free_chp_expr (d->chp_idx[i]);
    }
    FREE (d->chp_idx);
    FREE (d->idx);
  }
}

static struct chpsimderef *_mk_deref (ActId *id, ActSimCore *s, int *type,
				      int *width = NULL)
{
  struct chpsimderef *d;
  Scope *sc = s->cursi()->bnl->cur;

  ValueIdx *vx = id->rootVx (sc);
  Assert (vx->connection(), "What?");

  NEW (d, struct chpsimderef);

  d->cx = vx->connection();
  d->offset = s->getLocalOffset (vx->connection()->primary(),
				 s->cursi(), type, &d->width);
  if (width) {
    *width = d->width;
  }

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

      if (ActBooleanizePass::isDynamicRef (s->cursi()->bnl,
					   ((ActId *)e->u.e.l))) {
	d = _mk_deref ((ActId *)e->u.e.l, s, &type);
      }
      else {
	NEW (d, struct chpsimderef);
	d->range = NULL;
	d->offset = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), &type,
				       &d->width);
	d->cx = ((ActId *)e->u.e.l)->Canonical (s->cursi()->bnl->cur);
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
      int type;

      if (ActBooleanizePass::isDynamicRef (s->cursi()->bnl,
					   ((ActId *)e->u.e.l))) {
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
	int w;
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

  case E_BUILTIN_INT:
  case E_BUILTIN_BOOL:
    ret->u.e.l = expr_to_chp_expr (e->u.e.l, s);
    ret->u.e.r = expr_to_chp_expr (e->u.e.r, s);
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
  ret->delay_cost = 0;
  ret->energy_cost = 0;
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
  cur_pending_count = 0;
  max_pending_count = 0;
  return _buildChpSimGraph (sc, c, stop);
}

ChpSimGraph *ChpSimGraph::_buildChpSimGraph (ActSimCore *sc,
					    act_chp_lang_t *c,
					    ChpSimGraph **stop)
{
  ChpSimGraph *ret = NULL;
  ChpSimGraph *tmp2;
  int i, count;
  int tmp;
  int width;
  
  if (!c) return NULL;

  switch (c->type) {
  case ACT_CHP_SEMI:
    if (list_length (c->u.semi_comma.cmd)== 1) {
      return _buildChpSimGraph
	(sc,
	 (act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop);
    }
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ChpSimGraph *tmp = _buildChpSimGraph (sc, t, &tmp2);
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
      return _buildChpSimGraph
	(sc,
	 (act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop);
    }
    ret = new ChpSimGraph (sc);
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
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ret->all[i] = _buildChpSimGraph (sc,
				      (act_chp_lang_t *)list_value (li), &tmp2);
      if (ret->all[i]) {
	tmp2->next = *stop;
	count++;
      }
      i++;
    }
    if (count > 0) {
      NEW (ret->stmt, chpsimstmt);
      ret->stmt->delay_cost = 0;
      ret->stmt->energy_cost = 0;
      ret->stmt->type = CHPSIM_FORK;
      ret->stmt->u.fork = count;
      (*stop)->wait = count;
      (*stop)->totidx = tmp;
    }
    cur_pending_count--;
    break;

  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
  case ACT_CHP_LOOP:
    ret = new ChpSimGraph (sc);
    ret->stmt = gc_to_chpsim (c->u.gc, sc);
    if (c->type == ACT_CHP_LOOP) {
      ret->stmt->type = CHPSIM_LOOP;
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
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      ret->all[i] = _buildChpSimGraph (sc, gc->s, &tmp2);
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
    ret->stmt->type = CHPSIM_LOOP;
    (*stop) = new ChpSimGraph (sc);
    ret->next = (*stop);
    MALLOC (ret->all, ChpSimGraph *, 1);
    ret->all[0] = _buildChpSimGraph (sc, c->u.gc->s, &tmp2);
    tmp2->next = ret;
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
    {
      InstType *it = sc->cursi()->bnl->cur->FullLookup (c->u.comm.chan, NULL);
      Chan *ch;
      if (TypeFactory::isUserType (it)) {
	Channel *x = dynamic_cast<Channel *> (it->BaseType());
	ch = dynamic_cast<Chan *> (x->root()->BaseType());
      }
      else {
	ch = dynamic_cast<Chan *> (it->BaseType());
      }
      if (TypeFactory::isStructure (ch->datatype()) ||
	  ch->acktype() && TypeFactory::isStructure (ch->acktype())) {
	fatal_error ("CHP structure send. FIXME!");
      }
    }
    ret = new ChpSimGraph (sc);
    NEW (ret->stmt, chpsimstmt);
    _get_costs (sc->cursi(), c->u.comm.chan, ret->stmt);
    ret->stmt->type = CHPSIM_SEND;

    ret->stmt->u.sendrecv.e = NULL;
    ret->stmt->u.sendrecv.d = NULL;

    if (c->u.comm.e) {
      ret->stmt->u.sendrecv.e = expr_to_chp_expr (c->u.comm.e, sc);
    }
    if (c->u.comm.var) {
      ActId *id = c->u.comm.var;
      int type;
      struct chpsimderef *d;

      if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, id)) {
	d = _mk_deref (id, sc, &type);
      }
      else {
	NEW (d, struct chpsimderef);
	d->range = NULL;
	d->offset = sc->getLocalOffset (id, sc->cursi(), &type, &width);
	d->width = width;
	d->cx = id->Canonical (sc->cursi()->bnl->cur);
      }
      if (type == 3) {
	type = 2;
      }
      ret->stmt->u.sendrecv.d = d;
      ret->stmt->u.sendrecv.d_type = type;
    }
    ret->stmt->u.sendrecv.chvar = sc->getLocalOffset (c->u.comm.chan, sc->cursi(), NULL);
    ret->stmt->u.sendrecv.vc = c->u.comm.chan->Canonical (sc->cursi()->bnl->cur);
    (*stop) = ret;
    break;
    
    
  case ACT_CHP_RECV:
    {
      InstType *it = sc->cursi()->bnl->cur->FullLookup (c->u.comm.chan, NULL);
      Chan *ch;
      if (TypeFactory::isUserType (it)) {
	Channel *x = dynamic_cast<Channel *> (it->BaseType());
	ch = dynamic_cast<Chan *> (x->root()->BaseType());
      }
      else {
	ch = dynamic_cast<Chan *> (it->BaseType());
      }
      if (TypeFactory::isStructure (ch->datatype()) ||
	  ch->acktype() && TypeFactory::isStructure (ch->acktype())) {
	fatal_error ("CHP structure recv. FIXME!");
      }
    }
    ret = new ChpSimGraph (sc);
    NEW (ret->stmt, chpsimstmt);
    _get_costs (sc->cursi(), c->u.comm.chan, ret->stmt);
    ret->stmt->type = CHPSIM_RECV;

    ret->stmt->u.sendrecv.e = NULL;
    ret->stmt->u.sendrecv.d = NULL;

    if (c->u.comm.e) {
      ret->stmt->u.sendrecv.e = expr_to_chp_expr (c->u.comm.e, sc);
    }
    if (c->u.comm.var) {
      ActId *id = c->u.comm.var;
      int type;
      struct chpsimderef *d;

      /*-- if this is a structure, unravel the structure! --*/

      if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, id)) {
	d = _mk_deref (id, sc, &type);
      }
      else {
	NEW (d, struct chpsimderef);
	d->range = NULL;
	d->offset = sc->getLocalOffset (id, sc->cursi(), &type, &width);
	d->width = width;
	d->cx = id->Canonical (sc->cursi()->bnl->cur);
      }
      if (type == 3) {
	type = 2;
      }
      ret->stmt->u.sendrecv.d = d;
      ret->stmt->u.sendrecv.d_type = type;
    }

    ret->stmt->u.sendrecv.chvar = sc->getLocalOffset (c->u.comm.chan, sc->cursi(), NULL);
    ret->stmt->u.sendrecv.vc = c->u.comm.chan->Canonical (sc->cursi()->bnl->cur);
    (*stop) = ret;
    break;

  case ACT_CHP_FUNC:
    if (strcmp (string_char (c->u.func.name), "log") != 0) {
      warning ("Built-in function `%s' is not known; valid values: log",
	       string_char (c->u.func.name));
    }
    else {
      listitem_t *li;
      ret = new ChpSimGraph (sc);
      NEW (ret->stmt, chpsimstmt);
      ret->stmt->delay_cost = 0;
      ret->stmt->energy_cost = 0;
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
    {
      InstType *it = sc->cursi()->bnl->cur->FullLookup (c->u.assign.id, NULL);
      if (TypeFactory::isStructure (it)) {
	fatal_error ("CHP structure assignment. FIXME!");
      }
    }
    ret = new ChpSimGraph (sc);
    NEW (ret->stmt, chpsimstmt);
    _get_costs (sc->cursi(), c->u.assign.id, ret->stmt);
    ret->stmt->type = CHPSIM_ASSIGN;
    ret->stmt->u.assign.e = expr_to_chp_expr (c->u.assign.e, sc);
    {
      int type, width;

      if (ActBooleanizePass::isDynamicRef (sc->cursi()->bnl, c->u.assign.id)) {
	struct chpsimderef *d = _mk_deref (c->u.assign.id, sc, &type, &width);
	ret->stmt->u.assign.d = *d;
	FREE (d);
      }
      else {
	ret->stmt->u.assign.d.range = NULL;
	ret->stmt->u.assign.d.offset =
	  sc->getLocalOffset (c->u.assign.id, sc->cursi(), &type, &width);
	ret->stmt->u.assign.d.cx =
	  c->u.assign.id->Canonical (sc->cursi()->bnl->cur);
      }
      if (type == 1) {
	Assert (width > 0, "zero-width int?");
	ret->stmt->u.assign.isint = width;
      }
      else {
	Assert (type == 0, "Typechecking?!");
	ret->stmt->u.assign.isint = 0;
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

    if (type == 2 || type == 3) {
      loff = c->getGlobalOffset (loff, 2);
      act_channel_state *ch = sc->getChan (loff);
      ch->fragmented = 1;
    }
    delete tmp;
  }
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
    checkFragmentation (sc, c, (ActId *)e->u.e.l);
    break;

  case E_VAR:
    checkFragmentation (sc, c, (ActId *)e->u.e.l);
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
  case ACT_CHP_RECV:
    recordChannel (sc, cc, c->u.comm.chan);
    if (c->u.comm.e) {
      checkFragmentation (sc, cc, c->u.comm.e);
    }
    if (c->u.comm.var) {
      checkFragmentation (sc, cc,c->u.comm.var);
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

void ChpSimGraph::recordChannel (ActSimCore *sc, ChpSim *cc,
				 act_chp_lang_t *c)
{
  listitem_t *li;

  if (!c) return;
  
  switch (c->type) {
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


void ChpSim::dumpState (FILE *fp)
{
  int found = 0;
  fprintf (fp, "--- Process: ");
  getName()->Print (fp);
  fprintf (fp, " [ %s ] ---\n", _proc ? _proc->getName() : "-global-");

  for (int i=0; i < _npc; i++) {
    if (_pc[i]) {
      found = 1;
      fprintf (fp, "t#%02d: ", i);
      _pc[i]->printStmt (fp, _proc);
      fprintf (fp, "\n");
    }
  }
  if (!found) {
    fprintf (fp, "Terminated.\n");
  }

  fprintf (fp, "Energy cost: %lu\n", _energy_cost);
  if (_leakage_cost > 1e-3) {
    fprintf (fp, "Leakage: %g mW\n", _leakage_cost*1e3);
  }
  else if (_leakage_cost > 1e-6) {
    fprintf (fp, "Leakage: %g uW\n", _leakage_cost*1e6);
  }
  else if (_leakage_cost > 1e-9) {
    fprintf (fp, "Leakage: %g nW\n", _leakage_cost*1e9);
  }
  else {
    fprintf (fp, "Leakage: %g pW\n", _leakage_cost*1e12);
  }
  fprintf (fp, "Area: %lu\n", _area_cost);
  fprintf (fp, "\n");
}

unsigned long ChpSim::getEnergy (void)
{
  return _energy_cost;
}

double ChpSim::getLeakage (void)
{
  return _leakage_cost;
}

unsigned long ChpSim::getArea (void)
{
  return _area_cost;
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
    case CHPSIM_LOOP:
      {
	struct chpsimcond *x;
	int nguards = 1;
	int nw;
	_free_chp_expr (stmt->u.c.g);
	x = stmt->u.c.next;
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
