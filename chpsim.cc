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
#include <common/pp.h>
#include <sstream>

class ChpSim;

#ifndef MAX
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#endif

extern ActSim *glob_sim;

/*
 *
 * Dummy object used for adding an "idle" to the channel trace log
 *
 */
class ChanTraceDelayed : public SimDES {
 public:
  ChanTraceDelayed (const ActSim::watchpt_bucket *n) { _n = n; _has_val = 0; }
  ChanTraceDelayed (const ActSim::watchpt_bucket *n, const BigInt &x) { _n = n; _has_val = 1; _v = x; }
  ~ChanTraceDelayed () { _n = NULL; }

  int Step (Event *ev) {
    BigInt xtm = SimDES::CurTime();
    float tm = glob_sim->curTimeMetricUnits ();
    int len = xtm.getLen();
    unsigned long tval;
    unsigned long *ptm;
    if (len == 1) {
      tval = xtm.getVal (0);
      ptm = &tval;
    }
    else {
      MALLOC (ptm, unsigned long, len);
      for (int i=0; i < len; i++) {
	ptm[i] = xtm.getVal (i);
      }
    }
    unsigned long *value;
    value = NULL;
    if (_has_val) {
      if (_v.getLen() > 1) {
	MALLOC (value, unsigned long, _v.getLen());
	for (int i=0; i < _v.getLen(); i++) {
	  value[i] = _v.getVal (i);
	}
      }
    }
    
    for (int fmt=0; fmt < TRACE_NUM_FORMATS; fmt++) {
      act_trace_t *tr = glob_sim->getTrace (fmt);
      if (tr && !((_n->ignore_fmt >> fmt) & 1)) {
	if (act_trace_has_alt (tr->t)) {
	  if (_has_val) {
	    if (_v.getLen() == 1) {
	      act_trace_chan_change_alt (tr, _n->node[fmt], len, ptm,
					 ACT_CHAN_VALUE, _v.getVal (0));
	    }
	    else {
	      act_trace_wide_chan_change_alt (tr, _n->node[fmt], len, ptm,
					      ACT_CHAN_VALUE,
					      _v.getLen(), value);
	    }
	  }
	  else {
	    act_trace_chan_change_alt (tr, _n->node[fmt], len, ptm,
				       ACT_CHAN_IDLE, 0);
	  }
	}
	else {
	  if (_has_val) {
	    if (_v.getLen() == 1) {
	      act_trace_chan_change (tr, _n->node[fmt], tm,
				     ACT_CHAN_VALUE, _v.getVal (0));
	    }
	    else {
	      act_trace_wide_chan_change (tr, _n->node[fmt], tm,
					  ACT_CHAN_VALUE, _v.getLen(), value);
	    }
	  }
	  else {
	    act_trace_chan_change (tr, _n->node[fmt], tm, ACT_CHAN_IDLE, 0);
	  }
	}
      }
    }
    if (len > 1) {
      FREE (ptm);
    }

    if (_has_val && _v.getLen() > 1) {
      FREE (value);
    }
    
    delete this;
    return 1;
  }
 private:
  const ActSim::watchpt_bucket *_n;
  unsigned int _has_val:1;
  BigInt _v;
};  

//#define DUMP_ALL

static int stat_count = 0;

static void _chp_print (pp_t *pp, act_chp_lang_t *c, int prec = 0)
{
  int lprec = 0;
  
  if (!c) return;

  if (c->label) {
    pp_printf (pp, "%s: ", c->label);
    pp_setb (pp);
  }
  
  switch (c->type) {
  case ACT_CHP_COMMA:
  case ACT_CHP_SEMI:
    {
      listitem_t *li;

      for (li = list_first (c->u.semi_comma.cmd); li; li = list_next (li)) {
	_chp_print (pp, (act_chp_lang_t *)list_value (li), lprec);
      }

    }
    break;

  case ACT_CHP_LOOP:
  case ACT_CHP_DOLOOP:
  case ACT_CHP_SELECT:
  case ACT_CHP_SELECT_NONDET:
    if (c->type == ACT_CHP_LOOP || c->type == ACT_CHP_DOLOOP) {
      pp_printf (pp, "*");
    }
    pp_printf (pp, "[");
    if (c->type == ACT_CHP_SELECT_NONDET) {
      pp_printf (pp, "|");
    }
    pp_setb (pp);
    pp_printf (pp, " ");
    {
      act_chp_gc_t *gc = c->u.gc;

      if (c->type == ACT_CHP_DOLOOP) {
	pp_printf (pp, " ");
	_chp_print (pp, gc->s, 0);
	pp_printf (pp, " <- ");
	if (gc->g) {
	  char buf[10240];
	  sprint_uexpr (buf, 10240, gc->g);
	  pp_printf (pp, "%s", buf);
	}
	else {
	  pp_printf (pp, "true");
	}
	pp_printf (pp, " { %d }", stat_count++);
      }
      else {
	while (gc) {
	  pp_setb (pp);
	  if (!gc->g) {
	    if (c->type == ACT_CHP_LOOP) {
	      pp_printf (pp, "true");
	    }
	    else {
	      pp_printf (pp, "else");
	    }
	  }
	  else {
	    char buf[10240];
	    sprint_uexpr (buf, 10240, gc->g);
	    pp_printf (pp, "%s", buf);
	  }
	  pp_printf (pp, " { %d }", stat_count++);
	  if (gc->s) {
	    pp_printf (pp, " -> ");
	    pp_lazy (pp, 0);
	    _chp_print (pp, gc->s, 0);
	  }
	  pp_endb (pp);
	  if (gc->next) {
	    pp_united (pp, -1);
	    pp_printf (pp, " [] ");
	  }
	  gc = gc->next;
	}
      }
    }
    pp_endb (pp);
    pp_printf (pp, " ");
    pp_lazy (pp, 0);
    if (c->type == ACT_CHP_SELECT_NONDET) {
      pp_printf (pp, "|");
    }
    pp_printf (pp, "]");
    pp_lazy (pp, 0);
    break;
    
  case ACT_CHP_SKIP:
  case ACT_CHP_ASSIGN:
  case ACT_CHP_ASSIGNSELF:
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
  case ACT_CHP_FUNC:
  case ACT_CHP_MACRO:
    pp_printf (pp, ".");
    pp_lazy (pp, 0);
    break;
    
  case ACT_HSE_FRAGMENTS:
    do {
      _chp_print (pp, c->u.frag.body);
      pp_printf (pp, " : %s", c->u.frag.nextlabel);
      c = c->u.frag.next;
      if (c) {
	pp_printf (pp, ";\n");
	pp_forced (pp, 0);
	pp_printf (pp, "%s : ", c->label);
      }
    } while (c);
    break; 
    
  default:
    fatal_error ("Unknown type");
    break;
  }

  if (c->label) {
    pp_endb (pp);
  }
}

static void chp_print_stats (pp_t *pp, act_chp_lang_t *c)
{
  stat_count = 0;
  _chp_print (pp, c);
}



static pHashtable *_ZEROHASH = NULL;

static int _addhash (ChpSimGraph *g)
{
  phash_bucket_t *b;
  if (!_ZEROHASH) {
    _ZEROHASH = phash_new (16);
  }
  b = phash_lookup (_ZEROHASH, g);
  if (b) {
    return 1;
  }
  b = phash_add (_ZEROHASH, g);
  return 0;
}

static void _clrhash ()
{
  phash_free (_ZEROHASH);
  _ZEROHASH = NULL;
}

ChpSim::ChpSim (chpsimgraph_info *cgi,
		act_chp_lang_t *c, ActSimCore *sim, Process *p)
: ActSimObj (sim, p)
{
  ChpSimGraph *g;
  int max_cnt, max_stats;
  char buf[1024];

  if (cgi) {
    g = cgi->g;
    max_cnt = cgi->max_count;
    max_stats = cgi->max_stats;
  }
  else {
    g = NULL;
    max_cnt = 0;
    max_stats = 0;
  }

  _deadlock_pc = NULL;
  _stalled_pc = list_new ();
  _probe = NULL;
  _savedc = c;
  _energy_cost = 0;
  _leakage_cost = 0.0;
  _area_cost = 0;
  _statestk = NULL;
  _cureval = NULL;
  _frag_ch = NULL;
  _hse_mode = 0;		/* default is CHP */
  
  _maxstats = max_stats;
  if (_maxstats > 0) {
    MALLOC (_stats, unsigned long, _maxstats);
    for (int i=0; i < _maxstats; i++) {
      _stats[i] = 0;
    }
  }

  if (p) {
    char *nsname;
 
    if (p->getns() != ActNamespace::Global()) {
      nsname = p->getns()->Name();
    }
    else {
      nsname = NULL;
    }
    char tmpbuf[900];

    if (nsname) {
      snprintf (tmpbuf, 900, "%s::%s", nsname+2, p->getName());
      FREE (nsname);
    }
    else {
      snprintf (tmpbuf, 900, "%s", p->getName());
    }

    snprintf (buf, 1024, "sim.chp.%s.leakage", tmpbuf);
    if (config_exists (buf)) {
      _leakage_cost = config_get_real (buf);
    }
    else {
      _leakage_cost = config_get_real ("sim.chp.default_leakage");
    }
    snprintf (buf, 1024, "sim.chp.%s.area", tmpbuf);
    if (config_exists (buf)) {
      _area_cost = config_get_int (buf);
    }
    else {
      _area_cost = config_get_int ("sim.chp.default_area");
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

    if (_npc > SIM_EV_MAX-1) {
      fatal_error ("Currently there is a hard limit of %d concurrent modules within a single CHP block. Your program requires %d.", SIM_EV_MAX, _npc);
    }

    _pc = (ChpSimGraph **)
      sim->getState()->allocState (sizeof (ChpSimGraph *)*_npc);
    _holes = (int *) sim->getState()->allocState (sizeof (int)*_npc);
    for (int i=0; i < _npc; i++) {
      _pc[i] = NULL;
      _holes[i] = i;
    }
    _holes[0] = -1;

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
    _initEvent ();
    if (cgi) {
      _labels = cgi->labels;
    }
  }
  else {
    _pc = NULL;
    _npc = 0;
  }

}

void ChpSim::zeroInit ()
{
  if (_pc && _pc[0]) {
    _zeroAllIntsChans (_pc[0]);
    _clrhash ();
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
  _nextEvent (0, 0);
}
		     

ChpSim::~ChpSim()
{
  if (_statestk) {
    list_free (_statestk);
  }
  list_free (_stalled_pc);

  if (_deadlock_pc) {
    list_free (_deadlock_pc);
  }

  if (_maxstats > 0) {
    FREE (_stats);
  }
}

int ChpSim::_nextEvent (int pc, int bw_cost)
{
  while (_pc[pc] && !_pc[pc]->stmt) {
    pc = _updatepc (pc);
  }
  if (_pc[pc]) {
    new Event (this, SIM_EV_MKTYPE (pc,0) /* pc */,
	       _sc->getDelay (_pc[pc]->stmt->delay_cost + bw_cost));
    return 1;
  }
  return 0;
}

void ChpSim::_initEvent ()
{
  _nextEvent (0, 0);
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

  if (!_pc[pc]) {
#ifdef DUMP_ALL    
    printf (" [released slot %d (pcused now: %d)]", pc, _pcused-1);
#endif    
    Assert (_pcused > 0, "What?");
    _holes[_pcused-1] = pc;
    _pcused--;
  }
  
  if (joined) {
#ifdef DUMP_ALL
    printf (" [joined #%d / %d %d]", pc, _pcused, _pc[pc]->wait);
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
  case E_BUILTIN_BOOL:
  case E_BUILTIN_INT:
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
    // don't need to deal with shared variables here
    // since this is pre-computed
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
      if ((c->fragmented & 0x1) && e->type == E_PROBEOUT) {
	return 1;
      }
      else if ((c->fragmented & 0x2) && e->type == E_PROBEIN) {
	return 1;
      }
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
	    int dy;
	    fprintf (stderr, "Process %s: channel `",
		     _proc ? _proc->getName() : "-global-");
	    act_connection *x = _sc->getConnFromOffset (_proc, e->u.x.val, 2, &dy);
	    x->Print (stderr);
	    fprintf (stderr, "'; ");
	    fprintf (stderr, "Instance: ");
	    if (getName()) {
	      getName()->Print (stderr);
	    }
	    else {
	      fprintf (stderr, "<>");
	    }
	    fprintf (stderr, "\n");
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
	    int dy;
	    fprintf (stderr, "Process %s: channel `",
		     _proc ? _proc->getName() : "-global-");
	    act_connection *x = _sc->getConnFromOffset (_proc, e->u.x.val, 2, &dy);
	    x->Print (stderr);
	    fprintf (stderr, "'; ");
	    fprintf (stderr, "Instance: ");
	    if (getName()) {
	      getName()->Print (stderr);
	    }
	    else {
	      fprintf (stderr, "<>");
	    }
	    fprintf (stderr, "\n");
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
    
  case E_FUNCTION:
    {
      Expr *tmp;

      tmp = e->u.fn.r;
      while (tmp) {
	ret = ret | _collect_sharedvars (tmp->u.e.l, pc, undo);
	tmp = tmp->u.e.r;
      }
    }
    break;

  case E_SELF:
  case E_SELF_ACK:
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

int ChpSim::computeOffset (const struct chpsimderef *d)
{
  if (!d->range) {
    return d->offset;
  }
  for (int i=0; i < d->range->nDims(); i++) {
    BigInt res = exprEval (d->chp_idx[i]);
    d->idx[i] = res.getVal(0);
  }
  int x = d->range->Offset (d->idx);
  if (x == -1) {
    fprintf (stderr, "In: ");
    if (getName()) {
      getName()->Print (stderr);
    }
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
  return d->offset + x*d->stride;
}

static const char *_process_string (const char *s,
				    int *is_zero,
				    int *type,
				    int *width)
{
  const char *stmp = s;
  *is_zero = 0;
  *type = 0;
  *width = -1;

  if (*stmp == '%') {
    int idx = 1;
    while (stmp[idx] >= '0' && stmp[idx] <= '9' && stmp[idx]) {
      if (*width == -1) {
	*width = stmp[idx] - '0';
      }
      else {
	*width *= 10;
	*width += stmp[idx] - '0';
      }
      idx++;
    }
    if (idx != 1) {
      if (stmp[1] == '0') {
	*is_zero = 1;
      }
      else {
	*is_zero = 0;
      }
    }
    if (stmp[idx]) {
      if (stmp[idx] == 'x') {
	*type = 2; /* hex */
	stmp = stmp + idx + 1;
      }
      else if (stmp[idx] == 'd') {
	*type = 1; /* signed */
	stmp = stmp + idx + 1;
      }
      else if (stmp[idx] == 'u') {
	*type = 0; /* default */
	stmp = stmp + idx + 1;
      }
      else if (stmp[idx] == 'b') {
	*type = 3; /* bits */
	stmp = stmp + idx + 1;
      }
      else {
	*is_zero = 0;
	*type = 0;
	*width = -1;
      }
    }
    else {
      *is_zero = 0;
      *type = 0;
      *width = -1;
    }
  }
  else {
    *is_zero = 0;
    *type = 0;
    *width = -1;
  }
  return stmp;
}

static void _process_print_int (BigInt &v,
				int int_is_zero,
				int int_type,
				int int_width)
{
  if (int_width == -1) {
    if (int_type == 0) {
      v.decPrint (actsim_log_fp());
      //actsim_log ("%" ACT_EXPR_RES_PRINTF "u", v.getVal (0));
    }
    else if (int_type == 1) {
      actsim_log ("%" ACT_EXPR_RES_PRINTF "d", v.getVal (0));
    }
    else if (int_type == 2) {
      v.hexPrint (actsim_log_fp ());
    }
    else if (int_type == 3) {
      v.bitPrint (actsim_log_fp ());
    }
  }
  else {
    if (int_type == 0) {
      v.decPrint (actsim_log_fp());
#if 0
      if (int_is_zero) {
	actsim_log ("%0" ACT_EXPR_RES_PRINTF "*u", int_width, v.getVal (0));
      }
      else {
	actsim_log ("%" ACT_EXPR_RES_PRINTF "*u", int_width, v.getVal (0));
      }
#endif
    }
    else if (int_type == 1) {
      if (int_is_zero) {
	actsim_log ("%0" ACT_EXPR_RES_PRINTF "*d", int_width, v.getVal (0));
      }
      else {
	actsim_log ("%" ACT_EXPR_RES_PRINTF "*d", int_width, v.getVal (0));
      }
    }
    else if (int_type == 2) {
      v.hexPrint (actsim_log_fp ());
    }
    else if (int_type == 3) {
      v.bitPrint (actsim_log_fp ());
    }
    else {
      fatal_error ("What?!");
    }
  }
}

void ChpSim::_remove_me (int pc)
{
  listitem_t *li;
  listitem_t *prev = NULL;

  for (li = list_first (_stalled_pc); li; li = list_next (li)) {
    if (list_ivalue (li) == pc) {
      list_delete_next (_stalled_pc, prev);
      sRemove ();
      return;
    }
    prev = li;
  }
}

static void _enum_error (Process *p, BigInt &v, int enum_sz)
{
  fprintf (stderr, "** FATAL ** Process %s: enumeration assigned illegal value\n", p ? p->getName() : "-top-");
  fprintf (stderr, "**   Max=%d; ", enum_sz);
  fprintf (stderr, " Value: ");
  v.decPrint (stderr);
  fprintf (stderr, " (0x");
  v.hexPrint (stderr);
  fprintf (stderr, ")\n");
}


static SimDES *_matchme_obj;
static int _matchme_pc;

static bool _matchme (Event *ev)
{
  if (ev->getObj() == _matchme_obj &&
      ev->getType() == SIM_EV_MKTYPE (_matchme_pc, 1)) {
    return true;
  }
  return false;
}

int ChpSim::Step (Event *ev)
{
  int ev_type = ev->getType ();
  int pc = SIM_EV_TYPE (ev_type);
  int flag = SIM_EV_FLAGS (ev_type);
  int forceret = 0;
  int frag;
  BigInt v;
  expr_multires vs, xchg;
  int off, goff;
  int _breakpt = 0;
  int sh_wakeup = 0;

  if (pc == MAX_LOCAL_PCS) {
    // wake-up from a shared variable block.
    
    // Note that the shared variable block may have been pre-empted
    // because you woke up on a channel action instead, followed by a
    // remove_me operation. In this case the stalled pc list will be
    // empty because it will have been cleared by a _remove_me()
    // operation.
    if (list_isempty (_stalled_pc)) {
      return 1;
    }
    Assert (!list_isempty (_stalled_pc), "What?");
    pc = list_delete_ihead (_stalled_pc);

#ifdef DUMP_ALL
    printf ("<< conv: %d >>\n", pc);
#endif
    sh_wakeup = 1;
  }

  //Assert (0 <= pc && pc < _pcused, "What?");
  Assert (0 <= pc && pc < _npc, "What?");

  if (!_pc[pc]) {
    return 1;
  }

  if (!_hse_mode && _sc->isResetMode() && _proc != NULL) {
    /*-- this is a real process: wait for run mode --*/
    new Event (this, SIM_EV_MKTYPE (pc, 0), 10);
    return 1;
  }

  /*-- go forward through sim graph until there's some work --*/
  while (_pc[pc] && !_pc[pc]->stmt) {
    pc = _updatepc (pc);
    /* if you go forward, then you're not waking up any more */
    flag = 0;
  }
  if (!_pc[pc]) return 1;

  chpsimstmt *stmt = _pc[pc]->stmt;

  int bw_cost = stmt->bw_cost;

#ifdef DUMP_ALL
#if 0
  SimDES::showAll (stdout);
#endif
  printf ("[%8lu %d; pc:%d(%d)] <", CurTimeLo(), flag, pc, _pcused);
  if (name) {
    name->Print (stdout);
  }
  printf ("> ");
#endif

  int my_loff;
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
	if (g->all[i]) {
	  if (first) {
	    idx = pc;
	    first = 0;
	  }
	  else {
	    Assert (_pcused < _npc, "What?");
	    idx = _holes[_pcused++];
	    _holes[_pcused-1] = -1;
	    count++;
	  }
	  _pc[idx] = g->all[i];
#if 0
	  printf (" idx:%d", idx);
#endif
	  _nextEvent (idx, bw_cost);
	}
      }
      //_pcused += stmt->u.fork - 1;
#ifdef DUMP_ALL
      printf (" _used:%d", _pcused);
#endif      
    }
    break;

  case CHPSIM_NOP:
    pc = _updatepc (pc);
    if (_sc->infLoopOpt()) {
      msgPrefix();
      printf ("** infinite loop **\n");
      _pc[pc] = NULL;
    }
    break;

  case CHPSIM_ASSIGN:
    _energy_cost += stmt->energy_cost;
#ifdef DUMP_ALL
    printf ("assign v[%d] := ", stmt->u.assign.d.offset);
#endif
    pc = _updatepc (pc);
    if (stmt->u.assign.is_struct) {
      vs = exprStruct (stmt->u.assign.e);
      if (!_structure_assign (&stmt->u.assign.d, &vs)) {
	_breakpt = 1;
      }
    }
    else {
      v = exprEval (stmt->u.assign.e);
#ifdef DUMP_ALL
      printf ("%lu (w=%d)", v.getVal (0), v.getWidth());
#endif
      off = computeOffset (&stmt->u.assign.d);
      my_loff = off;

      if (stmt->u.assign.isint == 0) {
	off = getGlobalOffset (off, 0);
#if 0
	printf (" [bglob=%d]", off);
#endif
	if (chkWatchBreakPt (0, my_loff, off, v, ev->getCause())) {
	  _breakpt = 1;
	}
	_sc->setBool (off, v.getVal (0));
	boolProp (off);
      }
      else {
	off = getGlobalOffset (off, 1);
#if 0
	printf (" [iglob=%d]", off);
#endif
	v.setWidth (stmt->u.assign.isint);
	v.toStatic ();

	if (chkWatchBreakPt (1, my_loff, off, v, ev->getCause())) {
	  _breakpt = 1;
	}
	v.setWidth (stmt->u.assign.isint);
	if (stmt->u.assign.d.isenum) {
	  BigInt tmpv (64, 0, 0);
	  tmpv.setVal (0, stmt->u.assign.d.enum_sz);
	  if (v >= tmpv) {
	    _breakpt = 1;
	    _enum_error (_proc, v, stmt->u.assign.d.enum_sz);
	  }
	}
	_sc->setInt (off, v);
	intProp (off);
      }
    }
    break;

  case CHPSIM_SEND:
    {
      struct chpsimderef *d;
      int id;
      int type;
      int rv;
      int skipwrite = 0;

      if (stmt->u.sendrecv.is_structx) {
	/* bidirectional */
	if (stmt->u.sendrecv.d) {
	  type = stmt->u.sendrecv.d_type;
	  d = stmt->u.sendrecv.d;
	  id = computeOffset (d);
	}
	else {
	  type = -1;
	}
      }
      
      if (!flag) {
	/*-- this is the first time we are at the send, so evaluate the
	  expression being sent over the channel --*/
	if (stmt->u.sendrecv.e) {
	  if (stmt->u.sendrecv.is_struct) {
	    vs = exprStruct (stmt->u.sendrecv.e);
	  }
	  else {
	    v = exprEval (stmt->u.sendrecv.e);
	    vs.setSingle (v);
	  }
	}
	else {
	  /* data-less */
	  v.setVal (0,0);
	  v.setWidth (1);
	  vs.setSingle (v);
	}
#ifdef DUMP_ALL      
	printf ("send val=%lu ", v.getVal (0));
#endif
      }
      /*-- attempt to send; suceeds if there is a receiver waiting,
	otherwise we have to wait for the receiver --*/
      goff = getGlobalOffset (stmt->u.sendrecv.chvar, 2);
      rv = varSend (pc, flag, stmt->u.sendrecv.chvar, goff,
		    stmt->u.sendrecv.flavor, vs, stmt->u.sendrecv.is_structx,
		    &xchg, &frag, &skipwrite);

      if (stmt->u.sendrecv.is_structx) {
	if (!rv && xchg.nvals > 0) {
	  v = xchg.v[0];
	}
      }
      
      if (rv) {
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
	if (stmt->u.sendrecv.is_structx) {
	  if (type != -1 && skipwrite == 0) {
	    if (stmt->u.sendrecv.is_structx == 1) {
	      if (type == 0) {
		off = getGlobalOffset (id, 0);
#if 0
		printf (" [glob=%d]", off);
#endif
		if (chkWatchBreakPt (0, id, off, v, ev->getCause())) {
		  _breakpt = 1;
		}
		_sc->setBool (off, v.getVal (0));
		boolProp (off);
	      }
	      else {
		off = getGlobalOffset (id, 1);
#if 0	    
		printf (" [glob=%d]", off);
#endif
		if (chkWatchBreakPt (1, id, off, v, ev->getCause())) {
		  _breakpt = 1;
		}
		v.setWidth (stmt->u.sendrecv.width);
		if (stmt->u.sendrecv.d->isenum) {
		  BigInt tmpv (64, 0, 0);
		  tmpv.setVal (0, stmt->u.sendrecv.d->enum_sz);
		  if (v >= tmpv) {
		    _breakpt = 1;
		    _enum_error (_proc, v, stmt->u.sendrecv.d->enum_sz);
		  }
		}
		_sc->setInt (off, v);
		intProp (off);
	      }
	    }
	    else {
	      if (!_structure_assign (stmt->u.sendrecv.d, &xchg)) {
		_breakpt = 1;
	      }
	    }
	  }
	}
      }
      if (chkWatchBreakPt (3, stmt->u.sendrecv.chvar, goff, v,
			   ev->getCause(),
			   (frag ? 1 : 0) | ((rv ? 1 : (flag ? 2 : 0)) << 1))) {
	_breakpt = 1;
      }
    }
    break;

  case CHPSIM_RECV:
    {
      struct chpsimderef *d;
      int id;
      int type;
      int rv;
      int skipwrite = 0;
      
      if (stmt->u.sendrecv.d) {
	type = stmt->u.sendrecv.d_type;
	d = stmt->u.sendrecv.d;
	id = computeOffset (d);
      }
      else {
	type = -1;
      }

      if (stmt->u.sendrecv.is_structx) {
	/* bidirectional */
	if (!flag) {
	  if (stmt->u.sendrecv.e) {
	    if (stmt->u.sendrecv.is_structx == 2) {
	      xchg = exprStruct (stmt->u.sendrecv.e);
	    }
	    else {
	      v = exprEval (stmt->u.sendrecv.e);
	      xchg.setSingle (v); 
	    }
	  }
	  else {
	    v.setVal (0,0);
	    v.setWidth (1);
	    xchg.setSingle (v);
	  }
	}
      }

      goff = getGlobalOffset (stmt->u.sendrecv.chvar, 2);
      rv = varRecv (pc, flag, stmt->u.sendrecv.chvar, goff,
		    stmt->u.sendrecv.flavor, &vs,
		    stmt->u.sendrecv.is_structx, xchg, &frag, &skipwrite);
      
      if (!rv && vs.nvals > 0) {
	v = vs.v[0];
      }
      /*-- attempt to receive value --*/
      if (chkWatchBreakPt (2, stmt->u.sendrecv.chvar, goff, v,
			   ev->getCause(),
			   ((rv ? 1 : (flag ? 2 : 0)) << 1))) {
	_breakpt = 1;
      }
      if (rv) {
	/*-- blocked, we have to wait for the sender --*/
#ifdef DUMP_ALL	
	printf ("recv blocked");
#endif	
	forceret = 1;
      }
      else {
	/*-- success! --*/
#ifdef DUMP_ALL	
	printf ("recv got %lu!", v.getVal (0));
#endif	
	if (type != -1 && skipwrite == 0) {
	  if (stmt->u.sendrecv.is_struct == 0) {
	    if (type == 0) {
	      off = getGlobalOffset (id, 0);
#if 0	    
	      printf (" [glob=%d]", off);
#endif
	      if (chkWatchBreakPt (0, id, off, v, ev->getCause())) {
		_breakpt = 1;
	      }
	      _sc->setBool (off, v.getVal (0));
	      boolProp (off);
	    }
	    else {
	      off = getGlobalOffset (id, 1);
#if 0	    
	      printf (" [glob=%d]", off);
#endif
	      if (chkWatchBreakPt (1, id, off, v, ev->getCause())) {
		_breakpt = 1;
	      }

	      if (stmt->u.sendrecv.d->isenum) {
		BigInt tmpv (64, 0, 0);
		tmpv = stmt->u.sendrecv.d->enum_sz;
		if (v >= tmpv) {
		  _breakpt = 1;
		  _enum_error (_proc, v, stmt->u.sendrecv.d->enum_sz);
		}
	      }
	      v.setWidth (stmt->u.sendrecv.width);
	      _sc->setInt (off, v);
	      intProp (off);
	    }
	  }
	  else {
	    if (!_structure_assign (stmt->u.sendrecv.d, &vs)) {
	      _breakpt = 1;
	    }
	  }
	}
	pc = _updatepc (pc);
	_energy_cost += stmt->energy_cost;
      }
    }
    break;

  case CHPSIM_FUNC:
    _energy_cost += stmt->energy_cost;
    {
      char buf[10240];
      buf[0] = '\0';
      if (name) {
	      name->sPrint (buf, 10240);
      }
      if (strcmp (stmt->u.fn.name, "log") == 0 || strcmp (stmt->u.fn.name, "log_p") == 0) {
        bool is_full = strcmp (stmt->u.fn.name, "log") == 0;
        if (_sc->isFiltered (buf)) {
          int int_is_zero = 0;
          int int_type = 0;
          int int_width = -1;
          if (is_full) msgPrefix (actsim_log_fp());
          for (listitem_t *li = list_first (stmt->u.fn.l); li; li = list_next (li)) {
            act_func_arguments_t *arg = (act_func_arguments_t *) list_value (li);
            if (arg->isstring) {
              const char *stmp = _process_string (string_char (arg->u.s),
                    &int_is_zero,
                    &int_type,
                    &int_width);
              actsim_log ("%s", stmp);
            }
            else {
              v = exprEval (arg->u.e);
              _process_print_int (v, int_is_zero, int_type, int_width);
            }
          }
          if (is_full) actsim_log ("\n");
          actsim_log_flush ();
        }
      }
      else if (strcmp (stmt->u.fn.name, "warn") == 0) {
        if (_sc->isFiltered (buf)) {
          int int_is_zero = 0;
          int int_type = 0;
          int int_width = -1;
          std::stringstream stream;
          
          for (listitem_t *li = list_first (stmt->u.fn.l); li; li = list_next (li)) {
            act_func_arguments_t *arg = (act_func_arguments_t *) list_value (li);
            if (arg->isstring) {
              const char *stmp = _process_string (string_char (arg->u.s),
                    &int_is_zero,
                    &int_type,
                    &int_width);
              stream << stmp;
            }
            else {
              v = exprEval (arg->u.e);
              stream << " [cannot print expressions] ";
            }
          }

          warning ("%s", stream.str().c_str());
        }
      }
      else if (strcmp (stmt->u.fn.name, "assert") == 0) {
        int int_is_zero = 0;
        int int_type = 0;
        int int_width = -1;
        bool condition = true;
        for (listitem_t *li = list_first (stmt->u.fn.l); li; li = list_next (li)) {
          act_func_arguments_t *arg = (act_func_arguments_t *) list_value (li);
          
          // evaluate the condition in the first argument
          if (condition) {
            Assert(!arg->isstring, "First assert argument is not an expression despite type check?");
            if (exprEval (arg->u.e).isZero()) {
              condition = false;
              msgPrefix (actsim_log_fp());
              actsim_log ("ASSERTION failed: ");
            
            // everything is fine, nothing more to do
            } else {
              break;
            }
          }
          else {
            if (arg->isstring) {
              const char *stmp = _process_string (string_char (arg->u.s),
                    &int_is_zero,
                    &int_type,
                    &int_width);
              actsim_log ("%s", stmp);
            }
            else {
              v = exprEval (arg->u.e);
              _process_print_int (v, int_is_zero, int_type, int_width);
            }
          }
        }
        if (!condition) {
          actsim_log ("\n");
          actsim_log_flush ();
          _breakpt = 1;
        }
      }
      else if (strcmp (stmt->u.fn.name, "log_nl") == 0) {
        if (_sc->isFiltered (buf)) {
          actsim_log ("\n");
          actsim_log_flush ();
        }
      }
      else if (strcmp (stmt->u.fn.name, "log_st") == 0) {
        if (_sc->isFiltered (buf)) {
          msgPrefix (actsim_log_fp());
          actsim_log_flush ();
        }
      }
      pc = _updatepc (pc);
    }
    break;

  case CHPSIM_COND:
  case CHPSIM_CONDARB:
  case CHPSIM_LOOP:
    {
      chpsimcond *gc;
      int cnt = 0;
      list_t *ch_list = NULL;
      int choice = -1;
      BigInt res;

#ifdef DUMP_ALL
      if (stmt->type == CHPSIM_COND || stmt->type == CHPSIM_CONDARB) {
	printf ("cond");
      }
      else {
	printf ("loop");
      }
#endif
      _energy_cost += stmt->energy_cost;
      
      if (flag) {
	int tmpret;
	/*-- release wait conditions in case there are multiple --*/
	tmpret = _add_waitcond (&stmt->u.cond.c, pc, 1);
	if (stmt->u.cond.is_shared || tmpret) {
	  _remove_me (pc);
	}
	if (_probe && sh_wakeup) {
	  /* search and delete probe event, if any */
	  _matchme_obj = this;
	  _matchme_pc = pc;
	  Event *ev = SimDES::matchPendingEvent (_matchme);
	  if (ev) {
	    ev->Remove();
#ifdef DUMP_ALL
	    printf (" [pruned ev]");
#endif
	  }
	}
      }

      gc = &stmt->u.cond.c;
      cnt = 0;
      ch_list = list_new ();
      while (gc) {
	if (gc->g) {
	  res = exprEval (gc->g);
	  if (res.getVal (0) != 0) {
	    list_iappend (ch_list, cnt);
	  }
	  cnt++;
	}
	gc = gc->next;
      }

      if (list_length (ch_list) > 1) {
	if (stmt->type != CHPSIM_CONDARB) {
	  msgPrefix (actsim_log_fp());
	  actsim_log ("** ERROR ** multiple (%d) guards true, mutex violation.\n", list_length (ch_list));
	  actsim_log_flush ();
	}
	else {
	  if (_sc->isRandomChoice ()) {
	    choice = _sc->getRandom (list_length (ch_list));
	  }
	}
      }

      cnt = 0;
      if (!list_isempty (ch_list)) {
	/* at least one true guard */
	if (choice == -1) {
	  choice = list_ivalue (list_first (ch_list));
	}
	else {
	  while (choice > 0) {
	    list_delete_ihead (ch_list);
	    choice--;
	  }
	  choice = list_delete_ihead (ch_list);
	}
      }
      gc = &stmt->u.cond.c;
      while (gc) {
	if ((cnt == choice) || !gc->g) {
#ifdef DUMP_ALL	  
	  printf (" gd#%d true", cnt);
#endif
	  _pc[pc] = _pc[pc]->all[cnt];
	  if (!_pc[pc]) {
	    Assert (_pcused > 0, "What?");
	    _holes[_pcused-1] = pc;
	    _pcused--;
	  }
	  if (_maxstats > 0 && stmt->u.cond.stats >= 0) {
	    _stats[stmt->u.cond.stats + cnt]++;
	  }
	  break;
	}
	cnt++;
	gc = gc->next;
      }
      list_free (ch_list);
      /* all guards false */
      if (!gc) {
	if (stmt->type == CHPSIM_COND || stmt->type == CHPSIM_CONDARB) {
	  /* selection: we just try again later: add yourself to
	     probed signals */
	  int tmpret;
	  forceret = 1;
	  tmpret = _add_waitcond (&stmt->u.cond.c, pc);
	  if (stmt->u.cond.is_shared || tmpret) {
#ifdef DUMP_ALL	      
	      printf (" [stall-sh]");
#endif
	      sStall ();
	      list_iappend (_stalled_pc, pc);
	  }
	  else {
	    if (!_probe) {
	      msgPrefix (actsim_log_fp());
	      actsim_log ("Warning: all guards false; no probes/shared vars\n");
	      actsim_log_flush ();
	      if (!_deadlock_pc)  {
		_deadlock_pc = list_new ();
	      }
	      list_iappend (_deadlock_pc, pc);
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
    return 1 - _breakpt;
  }
  else {
#ifdef DUMP_ALL  
    printf (" [NEXT!]\n");
#endif
    _nextEvent (pc, bw_cost);
  }
  return 1 - _breakpt;
}


/* returns 1 if blocked */
int ChpSim::varSend (int pc, int wakeup, int id, int off, int flavor,
		     expr_multires &v, int bidir,
		     expr_multires *xchg, int *frag,
		     int *skipwrite)
{
  act_channel_state *c;
  c = _sc->getChan (off);

  if (!c->use_flavors && flavor != 0) {
    c->use_flavors = 1;
    c->send_flavor = 0;
    c->recv_flavor = 0;
  }

  if (c->use_flavors && flavor == 0) {
    int dy;
    fprintf (stderr, "Process %s: all channel actions must use +/- flavors (`",
	     _proc ? _proc->getName() : "-global-");
    act_connection *x = _sc->getConnFromOffset (_proc, id, 2, &dy);
    x->toid()->Print (stderr);
    fprintf (stderr, "')\n");
    exit (1);
  }

  if (!wakeup && c->use_flavors) {
    if ((c->send_flavor+1) != flavor) {
      int dy;
      fprintf (stderr, "Process %s: channel send flavors must alternate between + and - (`",  _proc ? _proc->getName() : "-global-");
      act_connection *x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "')\n");
      exit (1);
    }
    c->send_flavor = 1 - c->send_flavor;
  }

  if (c->fragmented) {
    if (c->rfrag_st != 0 && !c->frag_warn) {
      int dy;
      ActId *pr;
      act_connection *x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      msgPrefix(actsim_log_fp());
      fprintf (actsim_log_fp(), "Channel has fragmented send and recv? (`");
      pr = x->toid();
      pr->Print (actsim_log_fp());
      fprintf (actsim_log_fp(), "')\n");
      msgPrefix (actsim_log_fp());
      fprintf (actsim_log_fp(), "CHP+hse/circuits are driving the same end of the channel?\n");
      c->frag_warn = 1;
      delete pr;
    }
#if 0
    printf ("[send %p] fragmented; in-st: %d / %d; wake-up: %d\n", c,
	    c->sfrag_st, c->sufrag_st, wakeup);
#endif
    *frag = 1;
    if (c->sfrag_st == 0) {
      c->data = v;
      c->sfrag_st = 1;
      c->sufrag_st = 0;
    }

    if (_sc->isResetMode()) {
      list_iappend (_stalled_pc, pc);
      sStall ();
#if 0
	printf ("[send %p] stall\n", c);
#endif
      return 1;
    }

    while (c->sufrag_st >= 0) {
      int idx;
      if (c->sfrag_st == 1) {
	idx = ACT_METHOD_SET;
      }
      else if (c->sfrag_st == 2) {
	idx = ACT_METHOD_SEND_UP;
      }
      else if (c->sfrag_st == 3) {
	idx = ACT_METHOD_SEND_REST;
      }
      else {
	/* finished protocol */
	c->sfrag_st = 0;
	if (bidir) {
	  *xchg = c->data2;
	  *skipwrite = c->skip_action;
	  c->skip_action = 0;
	}
        c->count++;
#if 0
	printf ("[send %p] done\n", c);
#endif
	return 0;
      }
      c->sufrag_st = c->cm->runMethod (_sc, c, idx, c->sufrag_st);
      if (c->sufrag_st == 0xff) { /* -1 */
	c->sfrag_st++;
	c->sufrag_st = 0;

	/* if flavors, then we are done half way as well */
	if (c->use_flavors && c->send_flavor == 1) {
	  if (c->sfrag_st == 3) {
	    if (bidir) {
	      *xchg = c->data2;
	      *skipwrite = c->skip_action;
	      c->skip_action = 0;
	    }
            c->count++;
#if 0
	printf ("[send %p] done\n", c);
#endif
	    return 0;
	  }
	}
      }
      else {
	list_iappend (_stalled_pc, pc);
	sStall ();
#if 0
	printf ("[send %p] stall\n", c);
#endif
	return 1;
      }
    }
  }
  Assert (!c->fragmented, "Hmm");
  *frag = 0;

#ifdef DUMP_ALL  
  printf (" [s=%d]", off);
#endif  

  if (wakeup) {
#ifdef DUMP_ALL
    printf (" [send-wake %d]", pc);
#endif
    c->send_here = 0;
    Assert (c->sender_probe == 0, "What?");

    if (bidir) {
      *xchg = c->data;
      *skipwrite = c->skip_action;
      c->skip_action = 0;
    }
    c->count++;
    return 0;
  }

  if (WAITING_RECEIVER (c)) {
#ifdef DUMP_ALL
    printf (" [waiting-recv %d]", c->recv_here-1);
#endif
    // blocked receive, because there was no data
    c->data = v;
    if (bidir) {
      *xchg = c->data2;
      *skipwrite = c->skip_action;
      c->skip_action = 0;
    }
    c->w->Notify (c->recv_here-1, this);
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
      if (getName()) {
	getName()->Print (stderr);
      }
      else {
	fprintf (stderr, "<>");
      }
      fprintf (stderr, "\n");
    }
    Assert (c->send_here == 0 && c->sender_probe == 0 &&
	    c->receiver_probe == 0,"What?");
    c->count++;
    return 0;
  }
  else {
#ifdef DUMP_ALL
    printf (" [send-blk %d]", pc);
#endif    
    if (WAITING_RECV_PROBE (c)) {
#ifdef DUMP_ALL      
      printf (" [waiting-recvprobe %d]", c->recv_here-1);
#endif
      c->probe->Notify (c->recv_here-1, this);
      c->recv_here = 0;
      c->receiver_probe = 0;
    }
    // we need to wait for the receive to show up
    c->data2 = v;
    if (c->send_here != 0) {
      act_connection *x;
      int dy;
      fprintf (stderr, "Process %s: concurrent access to channel `",
	       _proc ? _proc->getName() : "-global-");
      x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "'\n");
      fprintf (stderr, "Instance: ");
      if (getName()) {
	getName()->Print (stderr);
      }
      else {
	fprintf (stderr, "<>");
      }
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
int ChpSim::varRecv (int pc, int wakeup, int id, int off, int flavor,
		     expr_multires *v, int bidir,
		     expr_multires &xchg, int *frag, int *skipwrite)
{
  act_channel_state *c;
  c = _sc->getChan (off);

  if (!c->use_flavors && flavor != 0) {
    c->use_flavors = 1;
    c->send_flavor = 0;
    c->recv_flavor = 0;
  }

  if (c->use_flavors && flavor == 0) {
    int dy;
    fprintf (stderr, "Process %s: all channel actions must use +/- flavors (`",
	     _proc ? _proc->getName() : "-global-");
    act_connection *x = _sc->getConnFromOffset (_proc, id, 2, &dy);
    x->toid()->Print (stderr);
    fprintf (stderr, "')\n");
    exit (1);
  }

  if (!wakeup && c->use_flavors) {
    if ((c->recv_flavor+1) != flavor) {
      int dy;
      fprintf (stderr, "Process %s: channel receive flavors must alternate between + and - (`",  _proc ? _proc->getName() : "-global-");
      act_connection *x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "')\n");
      exit (1);
    }
    c->recv_flavor = 1 - c->recv_flavor;
  }
  

  if (c->fragmented) {
    if (c->sfrag_st != 0 && !c->frag_warn) {
      int dy;
      ActId *pr;
      act_connection *x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      msgPrefix(actsim_log_fp());
      fprintf (actsim_log_fp(), "Channel has fragmented send and recv? (`");
      pr = x->toid();
      pr->Print (actsim_log_fp());
      fprintf (actsim_log_fp(), "')\n");
      msgPrefix (actsim_log_fp());
      fprintf (actsim_log_fp(), "CHP+hse/circuits are driving the same end of the channel?\n");
      c->frag_warn = 1;
      delete pr;
    }
#if 0
    printf ("[recv %p] fragmented; in-st: %d / %d\n", c,
	    c->rfrag_st, c->rufrag_st);
#endif
    *frag = 1;
    if (c->rfrag_st == 0) {
      if (bidir) {
	c->data2 = xchg;
      }
      c->rfrag_st = 1;
      c->rufrag_st = 0;
    }

    if (_sc->isResetMode()) {
      list_iappend (_stalled_pc, pc);
      sStall ();
#if 0
	printf ("[recv %p] stall\n", c);
#endif
      return 1;
    }
    
    while (c->rufrag_st >= 0) {
      int idx;
      if (c->rfrag_st == 1) {
	idx = ACT_METHOD_GET;
      }
      else if (c->rfrag_st == 2) {
	idx = ACT_METHOD_RECV_UP;
      }
      else if (c->rfrag_st == 3) {
	idx = ACT_METHOD_RECV_REST;
      }
      else {
	/* finished protocol */
#if 0
	printf ("[recv %p] done\n", c);
#endif
	c->rfrag_st = 0;
	(*v) = c->data;
	*skipwrite = c->skip_action;
	c->skip_action = 0;
	return 0;
      }
#if 0
      printf ("[recv %p] run method %d\n", c, idx);
#endif
      c->rufrag_st = c->cm->runMethod (_sc, c, idx, c->rufrag_st);
      if (c->rufrag_st == 0xff) { /* -1 */
	c->rfrag_st++;
	c->rufrag_st = 0;
	if (c->use_flavors && c->recv_flavor == 1) {
	  if (c->rfrag_st == 3) {
#if 0
	    printf ("[recv %p] done\n", c);
#endif
	    (*v) = c->data;
	    *skipwrite = c->skip_action;
	    c->skip_action = 0;
	    return 0;
	  }
	}
      }
      else {
	list_iappend (_stalled_pc, pc);
	sStall ();
#if 0
	printf ("[recv %p] stall\n", c);
#endif
	return 1;
      }
    }
  }
  Assert (!c->fragmented, "What?");
  
  *frag = 0;
  
#ifdef DUMP_ALL  
  printf (" [r=%d]", off);
#endif
  
  if (wakeup) {
#ifdef DUMP_ALL    
    printf (" [recv-wakeup %d]", pc);
#endif
    //v->v[0].v = c->data;
    *v = c->data;
    *skipwrite = c->skip_action;
    c->skip_action = 0;
    if (c->recv_here != 0) {
      act_connection *x;
      int dy;
      fprintf (stderr, "Process %s: concurrent access to channel `",
	       _proc ? _proc->getName() : "-global-");
      x = _sc->getConnFromOffset (_proc, id, 2, &dy);
      x->toid()->Print (stderr);
      fprintf (stderr, "'\n");
      fprintf (stderr, "Instance: ");
      if (getName()) {
	getName()->Print (stderr);
      }
      else {
	fprintf (stderr, "<>");
      }
      fprintf (stderr, "\n");
    }
    Assert (c->recv_here == 0, "What?");
    return 0;
  }
  
  if (WAITING_SENDER (c)) {
#ifdef DUMP_ALL    
    printf (" [waiting-send %d]", c->send_here-1);
#endif    
    *v = c->data2;
    *skipwrite = c->skip_action;
    c->skip_action = 0;
    if (bidir) {
      c->data = xchg;
    }
    c->w->Notify (c->send_here-1, this);
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
      printf (" [waiting-sendprobe %d]", c->send_here-1);
#endif      
      c->probe->Notify (c->send_here-1, this);
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
      if (getName()) {
	getName()->Print (stderr);
      }
      else {
	fprintf (stderr, "<>");
      }
      fprintf (stderr, "\n");
      fatal_error ("Aborting execution.");
    }
    Assert (c->recv_here == 0, "What?");
    c->recv_here = (pc+1);
    if (!c->w->isWaiting (this)) {
      c->w->AddObject (this);
    }
    if (bidir) {
      c->data2 = xchg;
    }
    return 1;
  }
  return 0;
}


BigInt ChpSim::varEval (int id, int type)
{
  BigInt r;
  int off;

  off = getGlobalOffset (id, type == 3 ? 2 : type);
  if (type == 0) {
    int val;
    r.setWidth (1);
    val = _sc->getBool (off);
    r.setVal (0, val);
    if (val == 2) {
      act_connection *c;
      ActId *tmp;
      int dy;
      c = _sc->getConnFromOffset (_proc, id, type, &dy);
      msgPrefix (actsim_log_fp());
      fprintf (actsim_log_fp(), "WARNING: Boolean variable `");
      if (c) {
	tmp = c->toid();
	tmp->Print (actsim_log_fp());
	delete tmp;
      }
      fprintf (actsim_log_fp(), "' is X\n");
      r.setWidth (2);
      r.setVal (0, val);
    }
  }
  else if (type == 1) {
    r = *_sc->getInt (off);
  }
  else if (type == 2) {
    act_channel_state *c = _sc->getChan (off);
    if (WAITING_SENDER (c)) {
      Assert (c->data2.nvals == 1, "structure probes not supported!");
      r = c->data2.v[0];
    }
    else {
      /* value probe */
      r.setWidth (1);
      r.setVal (0, 0);
    }
  }
  else {
    /* probe */
    act_channel_state *c = _sc->getChan (off);
    if (WAITING_SENDER (c) || WAITING_RECEIVER (c)) {
      r.setWidth (1);
      r.setVal (0, 1);
    }
    else {
      r.setWidth (1);
      r.setVal (0, 0);
    }
  }
  r.toDynamic ();
  return r;
}

expr_multires ChpSim::varChanEvalStruct (int id, int type)
{
  int off;

  Assert (type != 0 && type != 1, "What?!");

  off = getGlobalOffset (id, 2);
  act_channel_state *c = _sc->getChan (off);
  if (!WAITING_SENDER (c)) {
    actsim_log ("ERROR: reading channel state without waiting sender!");
    actsim_log_flush ();
  }
  return c->data2;
}


void ChpSim::_run_chp (Function *f, act_chp_lang_t *c)
{
  listitem_t *li;
  hash_bucket_t *b;
  BigInt *x, res;
  expr_multires *xm, resm;
  act_chp_gc_t *gc;
  struct Hashtable *state = ((struct Hashtable *)stack_peek (_statestk));
  InstType *xit;
  
  if (!c) return;
  switch (c->type) {
  case ACT_CHP_SEMI:
  case ACT_CHP_COMMA:
    for (li = list_first (c->u.semi_comma.cmd); li; li = list_next (li)) {
      _run_chp (f, (act_chp_lang_t *) list_value (li));
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
	_run_chp (f, gc->s);
	return;
      }
      res = exprEval (gc->g);
      if (res.getVal (0)) {
	_run_chp (f, gc->s);
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
	  _run_chp (f, gc->s);
	  break;
	}
	res = exprEval (gc->g);
	if (res.getVal (0)) {
	  _run_chp (f, gc->s);
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
      _run_chp (f, gc->s);
      res = exprEval (gc->g);
    } while (res.getVal (0));
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
  case ACT_CHP_RECV:
    fatal_error ("Functions cannot use send/receive");
    break;
    
  case ACT_CHP_FUNC:
    if (strcmp (string_char (c->u.func.name), "log") == 0 || strcmp (string_char (c->u.func.name), "log_p") == 0) {
      listitem_t *li;
      int int_is_zero = 0;
      int int_type = 0;
      int int_width = -1;
      if (strcmp (string_char (c->u.func.name), "log") == 0) msgPrefix (actsim_log_fp());
      for (li = list_first (c->u.func.rhs); li; li = list_next (li)) {
	      act_func_arguments_t *tmp = (act_func_arguments_t *)list_value (li);
	      if (tmp->isstring) {
	        const char *stmp = _process_string (string_char (tmp->u.s),
	      				      &int_is_zero,
	      				      &int_type,
	      				      &int_width);
	        actsim_log ("%s", stmp);
	      }
	      else {
	        BigInt v = exprEval (tmp->u.e);
	        _process_print_int (v, int_is_zero, int_type, int_width);
	      }
      }
      if (strcmp (string_char (c->u.func.name), "log") == 0) actsim_log ("\n");
      actsim_log_flush ();
    }
    else if (strcmp (string_char (c->u.func.name), "warn") == 0) {
      int int_is_zero = 0;
      int int_type = 0;
      int int_width = -1;
      std::stringstream stream;
      
      for (li = list_first (c->u.func.rhs); li; li = list_next (li)) {
        act_func_arguments_t *arg = (act_func_arguments_t *) list_value (li);
        if (arg->isstring) {
          const char *stmp = _process_string (string_char (arg->u.s),
                &int_is_zero,
                &int_type,
                &int_width);
          stream << stmp;
        }
        else {
          BigInt v = exprEval (arg->u.e);
          stream << " [cannot print expressions] ";
        }
      }

      warning ("%s", stream.str().c_str());
    }
    else if (strcmp (string_char (c->u.func.name), "assert") == 0) {
      bool condition = true;
      int int_is_zero = 0;
      int int_type = 0;
      int int_width = -1;

      for (li = list_first (c->u.func.rhs); li; li = list_next (li)) {
	      act_func_arguments_t *tmp = (act_func_arguments_t *)list_value (li);
        if (condition) {
          Assert (!tmp->isstring, "First assert argument is not an expression despite type check?");
          // check if the expression holds
          if (exprEval (tmp->u.e).isZero()) {
            // the assertion failed!
            msgPrefix (actsim_log_fp());
            actsim_log ("ASSERTION failed: ");
            condition = false;
          }
          // the condition is true; nothing further to do
          else {
            break;
          }
        }
        // print the rest of the arguments
        else {
          if (tmp->isstring) {
	          const char *stmp = _process_string (string_char (tmp->u.s),
	        				      &int_is_zero,
	        				      &int_type,
	        				      &int_width);
	          actsim_log ("%s", stmp);
	        }
	        else {
	          BigInt v = exprEval (tmp->u.e);
	          _process_print_int (v, int_is_zero, int_type, int_width);
	        }
        }
      }
    }
    else if (strcmp (string_char (c->u.func.name), "log_nl") == 0) {
      actsim_log ("\n");
      actsim_log_flush ();
    }
    else if (strcmp (string_char (c->u.func.name), "log_st") == 0) {
      msgPrefix (actsim_log_fp());
      actsim_log_flush ();
    }
    else {
      warning ("Built-in function `%s' is not known; valid values: log, log_st, log_p, log_nl, assert, warn",
	       string_char (c->u.func.name));
    }
    break;
    
  case ACT_CHP_ASSIGN:
#if 0
    if (c->u.assign.id->Rest()) {
      fatal_error ("Dots not permitted in functions!");
    }
#endif    
    b = hash_lookup (state, c->u.assign.id->getName());
    if (!b) {
      fatal_error ("Variable `%s' not found?!", c->u.assign.id->getName());
    }
    xit = f->CurScope()->Lookup (c->u.assign.id->getName());
    if (TypeFactory::isStructure (xit)) {
      /* this is either a structure or a part of structure assignment */
      xm = (expr_multires *)b->v;

      /* check if this is a structure! */
      xit = f->CurScope()->FullLookup (c->u.assign.id, NULL);
      if (TypeFactory::isStructure (xit)) {
	resm = exprStruct (c->u.assign.e);
	xm->setField (c->u.assign.id->Rest(), &resm);
      }
      else {
	res = exprEval (c->u.assign.e);
	xm->setField (c->u.assign.id->Rest(), &res);
      }
      delete xit;
    }
    else {
      x = (BigInt *) b->v;
      res = exprEval (c->u.assign.e);
      res.setWidth (x->getWidth());
      *x = res;
      x->toStatic ();
    }
    break;

  default:
    fatal_error ("Unknown chp type %d\n", c->type);
    break;
  }
}

typedef expr_res (*EXTFUNC) (int nargs, expr_res *args);
struct ExtLibs *_chp_ext = NULL;

BigInt ChpSim::funcEval (Function *f, int nargs, void **vargs)
{
  struct Hashtable *lstate;
  hash_bucket_t *b;
  hash_iter_t iter;
  BigInt *x;
  expr_multires *xm;
  BigInt ret;
  ActInstiter it(f->CurScope());

  /*-- allocate state and bindings --*/
  lstate = hash_new (4);

  for (it = it.begin(); it != it.end(); it++) {
    ValueIdx *vx = (*it);
    if (TypeFactory::isParamType (vx->t)) continue;

    if (vx->t->arrayInfo()) {
      warning ("Ignoring arrays for now...");
    }

    b = hash_add (lstate, vx->getName());

    if (TypeFactory::isStructure (vx->t)) {
      expr_multires *x2;
      Data *xd = dynamic_cast<Data *> (vx->t->BaseType());
      NEW (x2, expr_multires);
      new (x2) expr_multires (xd);
      b->v = x2;
    }
    else {
      NEW (x, BigInt);
      new (x) BigInt;
      x->setVal (0, 0);
      x->setWidth (TypeFactory::bitWidth (vx->t));
      b->v = x;
    }
  }

  if (nargs != f->getNumPorts()) {
    fatal_error ("Function `%s': invalid number of arguments", f->getName());
  }

  for (int i=0; i < f->getNumPorts(); i++) {
    int w;
    b = hash_lookup (lstate, f->getPortName (i));
    Assert (b, "What?");

    if (TypeFactory::isStructure (f->getPortType (i))) {
      xm = (expr_multires *)b->v;
      *xm = *((expr_multires *)vargs[i]);
    }
    else {
      x = (BigInt *) b->v;
      w = x->getWidth ();
      *x = *((BigInt *)vargs[i]);
      x->setWidth (w);
      x->toStatic ();
    }
  }

  /* --- run body -- */
  if (f->isExternal()) {
    EXTFUNC extcall = NULL;

    if (!_chp_ext) {
      _chp_ext = act_read_extern_table ("sim.extern");
    }
    extcall = (EXTFUNC) act_find_dl_func (_chp_ext, f->getns(), f->getName());

    if (!extcall) {
      fatal_error ("Function `%s%s' missing chp body as well as external definition.",
		   f->getns() == ActNamespace::Global() ? "" :
		   f->getns()->Name(true) + 2,
		   f->getName());
    }

    expr_res *extargs = NULL;
    expr_res extret;
    if (nargs > 0) {
      MALLOC (extargs, expr_res, nargs);
      for (int i=0; i < nargs; i++) {
	if (TypeFactory::isStructure (f->getPortType (i))) {
	  fatal_error ("External function calls cannot have structure arguments");
	}
	extargs[i].width = ((BigInt *)vargs[i])->getWidth ();
	extargs[i].v = ((BigInt *)vargs[i])->getVal (0);
      }
    }
    extret = (*extcall) (nargs, extargs);
    if (nargs > 0) {
      FREE (extargs);
    }
    BigInt tmp;
    tmp.setWidth (extret.width);
    tmp.setVal (0, extret.v);

    hash_iter_init (lstate, &iter);
    while ((b = hash_iter_next (lstate, &iter))) {
      ValueIdx *vx = f->CurScope()->LookupVal (b->key);
      Assert (vx, "How is this possible?");
      if (TypeFactory::isStructure (vx->t)) {
	((expr_multires *)b->v)->~expr_multires();
      }
      else {
	((BigInt *)b->v)->~BigInt();
      }
      FREE (b->v);
    }
    hash_free (lstate);
    
    return tmp;
  }
  
  act_chp *c = f->getlang()->getchp();
  Scope *_tmp = _cureval;
  stack_push (_statestk, lstate);
  _cureval = f->CurScope();
  _run_chp (f, c->c);
  _cureval = _tmp;
  stack_pop (_statestk);

  /* -- return result -- */
  b = hash_lookup (lstate, "self");
  x = (BigInt *)b->v;
  ret = *x;

  hash_iter_init (lstate, &iter);
  while ((b = hash_iter_next (lstate, &iter))) {
    ValueIdx *vx = f->CurScope()->LookupVal (b->key);
    Assert (vx, "How is this possible?");
    if (TypeFactory::isStructure (vx->t)) {
      ((expr_multires *)b->v)->~expr_multires();
    }
    else {
      ((BigInt *)b->v)->~BigInt();
    }
    FREE (b->v);
  }
  hash_free (lstate);

  return ret;
}


// but at least 1
static int _ceil_log2 (int w)
{
  int i;

  w--;
  i = 0;
  while (w > 1) {
    i++;
    w = w >> 1;
  }
  if (i == 0) {
    return 1;
  }
  return i;
}
  
BigInt ChpSim::exprEval (Expr *e)
{
  BigInt l, r;
  Assert (e, "What?!");

  switch (e->type) {

  case E_TRUE:
    l.setWidth (1);
    l.setVal (0, 1);
    break;
    
  case E_FALSE:
    l.setWidth (1);
    l.setVal (0, 0);
    break;
    
  case E_INT:
    if (e->u.ival.v_extra) {
      l = *((BigInt *)e->u.ival.v_extra);
    }
    else {
      unsigned long x = e->u.ival.v;
      int width = 0;
      while (x) {
	x = x >> 1;
	width++;
      }
      if (width == 0) {
	width = 1;
      }
      l.setWidth (width);
      l.setVal (0, e->u.ival.v);
    }
    break;

  case E_REAL:
    fatal_error ("No real expressions permitted in CHP!");
    return l;
    break;

    /* binary */
  case E_AND:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l &= r;
    break;

  case E_OR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l |= r;
    break;

  case E_PLUS:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l += r;
    break;

  case E_MINUS:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    {
      phash_bucket_t *b;
      b = _sc->exprWidth (e);
      if (b) {
	l.setWidth (b->i);
      }
    }
    l -= r;
    break;

  case E_MULT:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l = l * r;
    break;
    
  case E_DIV:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l = l / r;
    break;
      
  case E_MOD:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l = l % r;
    break;
    
  case E_LSL:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l <<= r;
    break;
    
  case E_LSR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.toStatic();
    l >>= r;
    break;
    
  case E_ASR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l.toSigned ();
    l.toStatic();
    l >>= r;
    l.toUnsigned();
    break;
    
  case E_XOR:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    l ^= r;
    break;
    
  case E_LT:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if (l < r) {
      l.setWidth (1);
      l.setVal (0, 1);
    }
    else {
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;
    
  case E_GT:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if (l > r) {
      l.setWidth (1);
      l.setVal (0, 1);
    }
    else {
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;

  case E_LE:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if (l <= r) {
      l.setWidth (1);
      l.setVal (0, 1);
    }
    else {
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;
    
  case E_GE:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if (l >= r) {
      l.setWidth (1);
      l.setVal (0, 1);
    }
    else {
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;
    
  case E_EQ:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if (l == r) {
      l.setWidth (1);
      l.setVal (0, 1);
    }
    else {
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;
    
  case E_NE:
    l = exprEval (e->u.e.l);
    r = exprEval (e->u.e.r);
    if (l != r) {
      l.setWidth (1);
      l.setVal (0, 1);
    }
    else {
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;
    
  case E_NOT:
  case E_COMPLEMENT:
    l = exprEval (e->u.e.l);
    {
      phash_bucket_t *b;
      b = _sc->exprWidth (e->u.e.l);
      if (b) {
	l.setWidth (b->i);
      }
    }
    l = ~l;
    break;
    
  case E_UMINUS:
    l = exprEval (e->u.e.l);
    {
      phash_bucket_t *b;
      b = _sc->exprWidth (e->u.e.l);
      if (b) {
	l.setWidth (b->i);
      }
    }
    l = -l;
    l.toUnsigned ();
    break;

  case E_QUERY:
    l = exprEval (e->u.e.l);
    {
      BigInt zero;
      zero.setWidth (1);
      zero.setVal (0, 0);
      if (l != zero) {
	l = exprEval (e->u.e.r->u.e.l);
      }
      else {
	l = exprEval (e->u.e.r->u.e.r);
      }
    }
    break;

  case E_COLON:
  case E_COMMA:
    fatal_error ("Should have been handled elsewhere");
    break;

  case E_CONCAT:
    {
      int first = 1;
      do {
	int width;
	phash_bucket_t *b;
	r = exprEval (e->u.e.l);
	b = _sc->exprWidth (e->u.e.l);
	if (b) {
	  r.setWidth (b->i);
	}
	if (first) {
	  l = r;
	  first = 0;
	}
	else {
	  BigInt tmp;
	  tmp.setWidth (32);
	  tmp.setVal (0, r.getWidth());
	  l <<= tmp;
	}
	r.setWidth(l.getWidth());
	l |= r;
	e = e->u.e.r;
      } while (e);
    }
    break;

  case E_CHP_BITFIELD:
    {
      int lo, hi;
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);

      hi = (long)e->u.e.r->u.e.r->u.ival.v;
      if (e->u.e.r->u.e.l) {
	lo = (long)e->u.e.r->u.e.l->u.ival.v;
      }
      else {
	lo = hi;
      }
      /* is an int */
      l = varEval (off, 1);
      l.setWidth (((struct chpsimderef *)e->u.e.l)->width);
      l >>= lo;

      if (hi < lo) {
	msgPrefix();
	printf ("bit-field {%d..%d} is invalid; using one-bit result",
		hi, lo);
	printf ("\n");
	l.setWidth (1);
      }
      else {
	l.setWidth (hi - lo + 1);
      }
    }
    break;
    
  case E_CHP_VARBOOL:
    l = varEval (e->u.x.val, 0);
    l.setWidth (1);
    break;

  case E_CHP_VARINT:
    l = varEval (e->u.x.val, 1);
    l.setWidth (e->u.x.extra);
    break;
    
  case E_CHP_VARCHAN:
    l = varEval (e->u.x.val, 2);
    l.setWidth (e->u.x.extra);
    break;
    
  case E_CHP_VARBOOL_DEREF:
    {
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);
      l = varEval (off, 0);
      l.setWidth (((struct chpsimderef *)e->u.e.l)->width);
    }
    break;

  case E_CHP_VARINT_DEREF:
    {
      int off = computeOffset ((struct chpsimderef *)e->u.e.l);
      l = varEval (off, 1);
      l.setWidth (((struct chpsimderef *)e->u.e.l)->width);
    }
    break;

  case E_VAR:
    {
      ActId *xid = (ActId *) e->u.e.l;

      if (_statestk) {
	struct Hashtable *state = ((struct Hashtable *)stack_peek (_statestk));
	Assert (state,"what?");
	hash_bucket_t *b = hash_lookup (state, xid->getName());
	Assert (b, "what?");
	Assert (_cureval, "What?");
	InstType *xit = _cureval->Lookup (xid->getName());
	if (TypeFactory::isStructure (xit)) {
	  expr_multires *x2 = (expr_multires *)b->v;
	  l = *(x2->getField (xid->Rest()));
	}
	else {
	  l = *((BigInt *)b->v);
	}
      }
      else if (_frag_ch) {
	act_connection *c;
	ihash_bucket_t *b;
	if (strcmp (xid->getName(), "self") == 0) {
	  l = _frag_ch->data.v[0];
	}
	else {
	  c = xid->Canonical (_frag_ch->ct->CurScope());
	  b = ihash_lookup (_frag_ch->fH, (long)c);
	  Assert (b, "Error during channel registration");
	  l.setWidth (1);
	  l.setVal (0, _sc->getBool (b->i));
	  if (_sc->getBool (b->i) == 2) {
	    ActId *tid;
	    msgPrefix ();
	    printf ("CHP model: Boolean variable `");
	    tid = c->toid();
	    tid->Print (stdout);
	    delete tid;
	    printf ("' is X\n");
	  }
	}
      }
      else {
	Assert (0, "E_VAR found without state stack or frag chan hash");
      }
    }
    break;

  case E_CHP_VARSTRUCT:
    fatal_error ("fixme");
    break;

  case E_BITFIELD:
    {
      ActId *xid = (ActId *) e->u.e.l;
      int lo, hi;

      Assert (!list_isempty (_statestk), "What?");

      struct Hashtable *state = ((struct Hashtable *)stack_peek (_statestk));
      Assert (state,"what?");
      hash_bucket_t *b = hash_lookup (state, xid->getName());
      Assert (b, "what?");
      Assert (_cureval, "What?");
      InstType *xit = _cureval->Lookup (xid->getName());
      if (TypeFactory::isStructure (xit)) {
	expr_multires *x2 = (expr_multires *)b->v;
	l = *(x2->getField (xid->Rest()));
      }
      else {
	l = *((BigInt *)b->v);
      }

      hi = (long)e->u.e.r->u.e.r->u.ival.v;
      if (e->u.e.r->u.e.l) {
	lo = (long)e->u.e.r->u.e.l->u.ival.v;
      }
      else {
	lo = hi;
      }
      l >>= lo;
      if (hi < lo) {
	msgPrefix();
	printf ("bit-field {%d..%d} is invalid; using one-bit result",
		hi, lo);
	printf ("\n");
	l.setWidth (1);
      }
      else {
	l.setWidth (hi - lo + 1);
      }
    }
    break;

  case E_PROBE:
    fatal_error ("E_PROBE-2");
    
  case E_PROBEIN:
  case E_PROBEOUT:
    {
      int off = getGlobalOffset (e->u.x.val, 2);
      act_channel_state *c = _sc->getChan (off);
      if ((c->fragmented & 0x1) && e->type == E_PROBEOUT) {
	l.setWidth (1);
	l.setVal (0, c->cm->runProbe (_sc, c, ACT_METHOD_SEND_PROBE));
	return l;
      }
      else if ((c->fragmented & 0x2) && e->type == E_PROBEIN) {
	l.setWidth (1);
	l.setVal (0, c->cm->runProbe (_sc, c, ACT_METHOD_RECV_PROBE));
	return l;
      }
    }
    l = varEval (e->u.x.val, 3);
    break;

  case E_BUILTIN_BOOL:
    l = exprEval (e->u.e.l);
    if (l.getVal (0)) {
      l.setVal (0, 1);
      l.setWidth (1);
    }
    else {
      l.setVal (0, 0);
      l.setWidth (1);
    }
    break;
    
  case E_BUILTIN_INT:
    l = exprEval (e->u.e.l);
    if (e->u.e.r) {
      r = exprEval (e->u.e.r);
    }
    else {
      r.setWidth (1);
      r.setVal (0, 1);
    }
    l.setWidth (r.getVal (0));
    break;

  case E_FUNCTION:
    /* function is e->u.fn.s */
    {
      Expr *tmp = NULL;
      int nargs = 0;
      int i;
      void **args;
      Function *fx = (Function *)e->u.fn.s;

      /* first: evaluate arguments */
      tmp = e->u.fn.r;
      while (tmp) {
	nargs++;
	tmp = tmp->u.e.r;
      }
      if (nargs > 0) {
	MALLOC (args, void *, nargs);
      }
      for (i=0; i < nargs; i++) {
	if (TypeFactory::isStructure (fx->getPortType (i))) {
	  args[i] = new expr_multires;
	}
	else {
	  args[i] = new BigInt;
	}
      }
      tmp = e->u.fn.r;
      i = 0;
      while (tmp) {
	if (TypeFactory::isStructure (fx->getPortType (i))) {
	  *((expr_multires *)args[i]) = exprStruct (tmp->u.e.l);
	}
	else {
	  *((BigInt *)args[i]) = exprEval (tmp->u.e.l);
	}
	i++;
	tmp = tmp->u.e.r;
      }
      l = funcEval ((Function *)e->u.fn.s, nargs, args);
      for (i=0; i < nargs; i++) {
	if (TypeFactory::isStructure (fx->getPortType (i))) {
	  delete ((expr_multires *)args[i]);
	}
	else {
	  delete ((BigInt *)args[i]);
	}
      }
      if (nargs > 0) {
	FREE (args);
      }
    }
    break;

  case E_SELF:
    if (_frag_ch) {
      l = _frag_ch->data.v[0];
    }
    else {
      Assert (0, "E_SELF used?!");
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;

  case E_SELF_ACK:
    if (_frag_ch) {
      l = _frag_ch->data2.v[0];
    }
    else {
      Assert (0, "E_SELF_ACK used?!");
      l.setWidth (1);
      l.setVal (0, 0);
    }
    break;
    
  default:
    l.setVal (0, 0);
    l.setWidth (1);
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  l.toDynamic ();
  return l;
}

expr_multires ChpSim::varStruct (struct chpsimderef *d)
{
  expr_multires res (d->d);
  if (!d->range) {
    for (int i=0; i < res.nvals; i++) {
      res.v[i] = varEval (d->idx[3*i], d->idx[3*i+1] == 2 ? 1 : d->idx[3*i+1]);
    }
  }
  else {
    /*-- structure deref --*/
    state_counts sc;
    ActStatePass::getStructCount (d->d, &sc);
    int off = computeOffset (d) - d->offset;
    int off_i = d->offset + off*sc.numInts();
    int off_b = d->width + off*sc.numBools();
    res.fillValue (d->d, _sc, off_i, off_b);
  }
  return res;
}

expr_multires ChpSim::funcStruct (Function *f, int nargs, void **vargs)
{
  struct Hashtable *lstate;
  hash_bucket_t *b;
  BigInt *x;
  expr_multires *xm;
  expr_multires ret;
  ActInstiter it(f->CurScope());
  InstType *ret_type = f->getRetType ();
  Data *d;

  Assert (TypeFactory::isStructure (ret_type), "What?");
  d = dynamic_cast<Data *> (ret_type->BaseType());
  Assert (d, "What?");
  /*-- allocate state and bindings --*/
  lstate = hash_new (4);

  for (it = it.begin(); it != it.end(); it++) {
    ValueIdx *vx = (*it);
    if (TypeFactory::isParamType (vx->t)) continue;

    if (vx->t->arrayInfo()) {
      warning ("Ignoring arrays for now...");
    }
    
    b = hash_add (lstate, vx->getName());
    if (TypeFactory::isStructure (vx->t)) {
      expr_multires *x2;
      Data *xd = dynamic_cast<Data *> (vx->t->BaseType());
      x2 = new expr_multires (xd);
      b->v = x2;
      //printf ("allocated struct (%s), nvals = %d, ptr = %p\n",
      //vx->getName(), x2->nvals, x2->v);
    }
    else {
      NEW (x, BigInt);
      new (x) BigInt;
      x->setVal (0,0);
      x->setWidth (TypeFactory::bitWidth (vx->t));
      x->toStatic ();
      b->v = x;
    }
  }

  if (nargs != f->getNumPorts()) {
    fatal_error ("Function `%s': invalid number of arguments", f->getName());
  }

  for (int i=0; i < f->getNumPorts(); i++) {
    int w;
    b = hash_lookup (lstate, f->getPortName (i));
    Assert (b, "What?");

    if (TypeFactory::isStructure (f->getPortType (i))) {
      xm = (expr_multires *)b->v;
      *xm = *((expr_multires *)vargs[i]);
    }
    else {
      x = (BigInt *) b->v;
      w = x->getWidth ();
      *x = *((BigInt *)vargs[i]);
      x->setWidth (w);
      x->toStatic ();
    }
  }

  /* --- run body -- */
  if (f->isExternal()) {
    fatal_error ("External function cannot return a structure!");
  }
  
  act_chp *c = f->getlang()->getchp();
  Scope *_tmp = _cureval;
  stack_push (_statestk, lstate);
  _cureval = f->CurScope();
  _run_chp (f, c->c);
  _cureval = _tmp;
  stack_pop (_statestk);

  /* -- return result -- */
  b = hash_lookup (lstate, "self");
  ret = *((expr_multires *)b->v);

  //printf ("ret: %d, %p\n", ret.nvals, ret.v);

  for (it = it.begin(); it != it.end(); it++) {
    ValueIdx *vx = (*it);
    if (TypeFactory::isParamType (vx->t)) continue;

    if (vx->t->arrayInfo()) {
      warning ("Ignoring arrays for now...");
    }
    
    b = hash_lookup (lstate, vx->getName());
    if (TypeFactory::isStructure (vx->t)) {
      expr_multires *x2 = (expr_multires *)b->v;
      delete x2;
    }
    else {
      FREE (b->v);
    }
  }
  hash_free (lstate);

  return ret;
}



expr_multires ChpSim::exprStruct (Expr *e)
{
  expr_multires res;
  Assert (e, "What?!");

  switch (e->type) {
  case E_CHP_VARSTRUCT_DEREF:
  case E_CHP_VARSTRUCT:
    res = varStruct ((struct chpsimderef *)e->u.e.l);
    break;

  case E_VAR:
    {
      Assert (!list_isempty (_statestk), "What?");
      struct Hashtable *state = ((struct Hashtable *)stack_peek (_statestk));
      Assert (state, "what?");
      hash_bucket_t *b = hash_lookup (state, ((ActId*)e->u.e.l)->getName());
      Assert (b, "what?");
      //printf ("looked up state: %s\n", ((ActId *)e->u.e.l)->getName());
      res = *((expr_multires *)b->v);
      if (((ActId *)e->u.e.l)->Rest()) {
	/*
	  ok, now re-construct a new expr_multires as a set of
	  fields from the original expr, with the appropriate type! 
	*/
	res = res.getStruct (((ActId *)e->u.e.l)->Rest());
      }
      //printf ("res = %d (%p)\n", res.nvals, res.v);
    }
    break;

  case E_FUNCTION:
    /* function is e->u.fn.s */
    {
      Expr *tmp = NULL;
      int nargs = 0;
      int i;
      void **args;
      Function *fx = (Function *)e->u.fn.s;

      /* first: evaluate arguments */
      tmp = e->u.fn.r;
      while (tmp) {
	nargs++;
	tmp = tmp->u.e.r;
      }
      if (nargs > 0) {
	MALLOC (args, void *, nargs);
      }
      for (i=0; i < nargs; i++) {
	if (TypeFactory::isStructure (fx->getPortType (i))) {
	  args[i] = new expr_multires;
	}
	else {
	  args[i] = new BigInt;
	}
      }
      tmp = e->u.fn.r;
      i = 0;
      while (tmp) {
	if (TypeFactory::isStructure (fx->getPortType (i))) {
	  *((expr_multires *)args[i]) = exprStruct (tmp->u.e.l);
	}
	else {
	  *((BigInt *)args[i]) = exprEval (tmp->u.e.l);
	}
	i++;
	tmp = tmp->u.e.r;
      }
      res = funcStruct (fx, nargs, args);
      for (i=0; i < nargs; i++) {
	if (TypeFactory::isStructure (fx->getPortType (i))) {
	  delete ((expr_multires *)args[i]);
	}
	else {
	  delete ((BigInt *)args[i]);
	}
      }
      if (nargs > 0) {
	FREE (args);
      }
    }
    break;

  case E_CHP_VARCHAN:
    res = varChanEvalStruct (e->u.x.val, 2);
    break;

  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  return res;
}


int ChpSim::_max_program_counters (act_chp_lang_t *c)
{
  int ret, val;

  if (!c) return 1;
  
  switch (c->type) {
  case ACT_HSE_FRAGMENTS:
    ret = 1;
    while (c) {
      val = _max_program_counters (c->u.frag.body);
      if (val > ret) {
	ret = val;
      }
      c = c->u.frag.next;
    }
    break;
    
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
    ret = 1;
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

static void _mark_vars_used (ActSimCore *_sc, ActId *id, struct iHashtable *H);

static void _rec_mark_userdef_used (ActSimCore *_sc, ActId *id,
				    UserDef *u, struct iHashtable *H)
{
  ActId *tl;

  if (!u) return;

  tl = id;
  while (tl->Rest()) {
    tl = tl->Rest();
  }
  
  for (int i=0; i < u->getNumPorts(); i++) {
    const char *nm = u->getPortName (i);
    InstType *it = u->getPortType (i);
    ActId *extra = new ActId (nm);
    ActId *prev;

    tl->Append (extra);
    prev = tl;
    
    tl = tl->Rest();

    if (TypeFactory::isBoolType (it)) {
      /* mark used */
      if (it->arrayInfo()) {
	Arraystep *s = it->arrayInfo()->stepper();
	while (!s->isend()) {
	  Array *a = s->toArray ();

	  tl->setArray (a);
	  _mark_vars_used (_sc, id, H);
	  tl->setArray (NULL);

	  delete a;
	  
	  s->step();
	}
	delete s;
      }
      else {
	_mark_vars_used (_sc, id, H);
      }
    }
    else if (TypeFactory::isUserType (it)) {
      UserDef *nu = dynamic_cast<UserDef *>(it->BaseType());
      Assert (nu, "What?");
      /* mark recursively */
      if (it->arrayInfo()) {
	Arraystep *s = it->arrayInfo()->stepper();
	while (!s->isend()) {
	  Array *a = s->toArray();
	  tl->setArray (a);
	  _rec_mark_userdef_used (_sc, id, nu, H);
	  tl->setArray (NULL);
	  delete a;
	  s->step();
	}
	delete s;
      }
      else {
	_rec_mark_userdef_used (_sc, id, nu, H);
      }
    }
    prev->prune();
    delete extra;
    tl = prev;
  }
}

static void _mark_vars_used (ActSimCore *_sc, ActId *id, struct iHashtable *H)
{
  int loff;
  int type;
  ihash_bucket_t *b;
  InstType *it;

  it = _sc->cursi()->bnl->cur->FullLookup (id, NULL);

  if (ActBooleanizePass::isDynamicRef (_sc->cursi()->bnl, id)) {
    
    /* -- we can ignore this for fanout tables, since dynamic arrays
       are entirely contained within a single process! -- */

    return;
    
    /* mark the entire array as used */
    ValueIdx *vx = id->rootVx (_sc->cursi()->bnl->cur);
    int sz;
    Assert (vx->connection(), "Hmm");

    /* check structure case */
    if (TypeFactory::isStructure (it)) {
      int loff_i, loff_b;
      if (!_sc->getLocalDynamicStructOffset (vx->connection()->primary(),
					    _sc->cursi(), &loff_i, &loff_b)) {
	fatal_error ("Structure int/bool error!");
      }
      sz = vx->t->arrayInfo()->size();
      state_counts sc;
      Data *d = dynamic_cast<Data *>(it->BaseType());
      Assert (d, "Hmm");
      ActStatePass::getStructCount (d, &sc);
      while (sz > 0) {
	sz--;
	if (loff_i >= 0) {
	  for (int i=0; i < sc.numInts(); i++) {
	    if (!ihash_lookup (H, loff_i + i)) {
	      b = ihash_add (H, loff_i + i);
	      b->i = 1; /* int */
	    }
	  }
	  loff_i += sc.numInts();
	}
	if (loff_b >= 0) {
	  for (int i=0; i < sc.numBools(); i++) {
	    if (!ihash_lookup (H, loff_b + i)) {
	      b = ihash_add (H, loff_b + i);
	      b->i = 0; /* bool */
	    }
	  }
	  loff_b += sc.numBools();
	}
      }
    }
    else {
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
  }
  else {
    if (TypeFactory::isStructure (it)) {
      /* walk through all pieces of the struct and mark it used */
      Data *d = dynamic_cast<Data *>(it->BaseType());
      ActId *tmp, *tail;
      tail = id;
      while (tail->Rest()) {
	tail = tail->Rest();
      }
      
      for (int i=0; i < d->getNumPorts(); i++) {
	InstType *xit;
	tmp = new ActId (d->getPortName(i));
	tail->Append (tmp);
	xit = d->getPortType (i);

	if (xit->arrayInfo()) {
	  Arraystep *as = xit->arrayInfo()->stepper();
	  while (!as->isend()) {
	    Array *a = as->toArray();
	    tmp->setArray (a);
	    _mark_vars_used (_sc, id, H);
	    delete a;
	    as->step();
	  }
	  tmp->setArray (NULL);
	  delete as;
	}
	else {
	  _mark_vars_used (_sc, id, H);
	}
	tail->prune();
	delete tmp;
      }
    }
    else {
      loff = _sc->getLocalOffset (id, _sc->cursi(), &type);
      if (!ihash_lookup (H, loff)) {
	b = ihash_add (H, loff);
	b->i = type;
      }
      /* channels might be fragmented */
      if (type == 2 || type == 3) {
	/*-- mark all the channel fields as used too --*/
	_rec_mark_userdef_used (_sc, id,
				dynamic_cast<UserDef *>(it->BaseType()), H);
      }
    }
  }
}

void ChpSim::_compute_used_variables_helper (Expr *e)
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
    _compute_used_variables_helper (e->u.e.l);
    _compute_used_variables_helper (e->u.e.r);
    break;
    
  case E_NOT:
  case E_UMINUS:
  case E_COMPLEMENT:
  case E_BUILTIN_BOOL:
  case E_BUILTIN_INT:
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
    _mark_vars_used (_sc, (ActId *)e->u.e.l, _tmpused);
    break;
    
  case E_FUNCTION:
    {
      e = e->u.fn.r;
      while (e) {
	_compute_used_variables_helper (e->u.e.l);
	e = e->u.e.r;
      }
    }
    break;

  case E_SELF:
  case E_SELF_ACK:
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
}

void ChpSim::_compute_used_variables_helper (act_chp_lang_t *c)
{
  if (!c) return;
  
  switch (c->type) {
  case ACT_HSE_FRAGMENTS:
    while (c) {
      _compute_used_variables_helper (c->u.frag.body);
      c = c->u.frag.next;
    }
    break;
    
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


void ChpSim::dumpState (FILE *fp)
{
  int found = 0;
  fprintf (fp, "--- Process: ");
  if (getName()) {
    getName()->Print (fp);
  }
  else {
    fprintf (fp, "-unknown-");
  }
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

void ChpSim::dumpStats (FILE *fp)
{
  if (_maxstats > 0) {
    
    fprintf (fp, "--- Process: ");
    if (getName()) {
      getName()->Print (fp);
    }
    else {
      fprintf (fp, "-unknown-");
    }
    fprintf (fp, " [ %s ] ---\n", _proc ? _proc->getName() : "-global-");

    pp_t *pp = pp_init (fp, 80);
    chp_print_stats (pp, _savedc);
    pp_forced (pp, 0);
    pp_stop (pp);

    for (int i=0; i < _maxstats; i++) {
      fprintf (fp, "%20d : %lu\n", i, _stats[i]);
    }
    fprintf (fp, "---\n");
    fprintf (fp, "\n");
  }
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

void ChpSim::propagate (void *cause)
{
  ActSimObj::propagate (cause);
}

static void _add_deref_struct2 (Data *d,
				int *idx,
				int *off_i,
				int *off_b,
				int *off)
{
  for (int i=0; i < d->getNumPorts(); i++) {
    InstType *it = d->getPortType (i);
    int sz = 1;
    
    if (it->arrayInfo()) {
      sz = it->arrayInfo()->size();
    }
    while (sz > 0) {
      if (TypeFactory::isStructure (it)) {
	Data *x = dynamic_cast<Data *> (it->BaseType());
	Assert (x, "What?");
	_add_deref_struct2 (x, idx, off_i, off_b, off);
      }
      else if (TypeFactory::isIntType (it)) {
	idx[*off] = *off_i;
	idx[*off+1] = 1;
	idx[*off+2] = TypeFactory::bitWidth (it);

	if (TypeFactory::isEnum (it)) {
	  idx[*off+1] = 2;
	  idx[*off+2] = TypeFactory::enumNum (it);
	}
      
	*off_i = *off_i + 1;
	*off += 3;
      }
      else {
	Assert (TypeFactory::isBoolType (it), "Hmm");
	idx[*off] = *off_b;
	idx[*off+1] = 0;
	idx[*off+2] = 1;
	*off_b = *off_b + 1;
	*off += 3;
      }
      sz--;
    }
  }
}


int ChpSim::_structure_assign (struct chpsimderef *d, expr_multires *v)
{
  Assert (d && v, "Hmm");
  int *struct_info;
  int struct_len;
  int ret = 1;

  state_counts ts;
  ActStatePass::getStructCount (d->d, &ts);

  if (d->range) {
    /* array deref */
    for (int i=0; i < d->range->nDims(); i++) {
      BigInt res = exprEval (d->chp_idx[i]);
      d->idx[i] = res.getVal (0);
    }
    int x = d->range->Offset (d->idx);
    if (x == -1) {
      fprintf (stderr, "In: ");
      if (getName()) {
	getName()->Print (stderr);
      }
      else {
	fprintf (stderr, "<>");
      }
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
    int off_i, off_b, off;
    MALLOC (struct_info, int, 3*(ts.numInts() + ts.numBools()));
    off_i = d->offset + x*ts.numInts();
    off_b = d->width + x*ts.numBools();
    off = 0;
    _add_deref_struct2 (d->d, struct_info, &off_i, &off_b, &off);
  }
  else {
    struct_info = d->idx;
  }
  struct_len = 3*(ts.numInts() + ts.numBools());
  for (int i=0; i < struct_len/3; i++) {
#if 0
    printf ("%d (%d:w=%d) := %lu (w=%d)\n",
	    struct_info[3*i], struct_info[3*i+1],
	    struct_info[3*i+2], v->v[i].v, v->v[i].width);
#endif
    int off = getGlobalOffset (struct_info[3*i],
			       struct_info[3*i+1] == 2 ?
			       1 : struct_info[3*i+1]);
    if (struct_info[3*i+1] == 1) {
      /* int */
      Assert (v->v[i].getWidth() == struct_info[3*i+2], "What?");
      _sc->setInt (off, v->v[i]);
      intProp (off);
    }
    else if (struct_info[3*i+1] == 2) {
      /* enum */
      BigInt tmpv (64, 0, 0);
      tmpv.setVal (0, struct_info[3*i+2]);
      if (v->v[i] >= tmpv) {
	_enum_error (_proc, v->v[i], struct_info[3*i+2]);
	ret = 0;
      }
      _sc->setInt (off, v->v[i]);
      intProp (off);
    }
    else {
      Assert (struct_info[3*i+1] == 0, "What?");
      _sc->setBool (off, v->v[i].getVal (0));
      boolProp (off);
    }
  }

  if (struct_info != d->idx) {
    FREE (struct_info);
  }

  return ret;
}

void ChpSim::boolProp (int glob_off)
{
  SimDES **arr;
  arr = _sc->getFO (glob_off, 0);

#ifdef DUMP_ALL
#if 0
  printf ("  >>> propagate %d\n", _sc->numFanout (glob_off, 0));
#endif
#endif
  for (int i=0; i < _sc->numFanout (glob_off, 0); i++) {
    ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
    Assert (p, "What?");
#ifdef DUMP_ALL
#if 0
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
#endif
    p->propagate (this);
  }
}

void ChpSim::intProp (int glob_off)
{
  SimDES **arr;
  arr = _sc->getFO (glob_off, 1);

#ifdef DUMP_ALL
#if 0
  printf ("  >>> propagate %d\n", _sc->numFanout (glob_off, 0));
#endif
#endif
  for (int i=0; i < _sc->numFanout (glob_off, 1); i++) {
    ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
    Assert (p, "What?");
#ifdef DUMP_ALL
#if 0
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
#endif
    p->propagate (this);
  }
}


/*
  flag : used for send/recv
  
  flag & 0x1 : fragmented channel operation
  (flag >> 1) : 0 = normal
              : 1 = blocked
	      : 2 = completed
*/
int ChpSim::chkWatchBreakPt (int type, int loff, int goff, const BigInt& v,
			     void *cause,  int flag)
{
  int verb = 0;
  int ret_break = 0;
  const ActSim::watchpt_bucket *nm;
  const char *nm2;
  if ((nm = _sc->chkWatchPt (type == 3 ? 2 : type, goff))) {
    verb = 1;
  }
  if ((nm2 = _sc->chkBreakPt (type == 3 ? 2 : type, goff))) {
    verb |= 2;
  }
  if (verb) {
    if (type == 0) {
      /* bool */
      int oval = _sc->getBool (goff);
      if (oval != v.getVal (0)) {
	if (verb & 1) {
	  msgPrefix ();
	  printf ("%s := %c", nm->s, (v.getVal (0) == 2 ? 'X' : ((char)v.getVal (0) + '0')));
	  if (cause) {
	    char buf[1024];
	    ActSimDES *x = (ActSimDES *) cause;
	    x->sPrintCause (buf, 1024);
	    printf ("   [by %s]", buf);
	  }
	  printf ("\n");
	  _sc->recordTrace (nm, type, ACT_CHAN_IDLE, v);
	}
	if (verb & 2) {
	  msgPrefix ();
	  printf ("*** breakpoint %s\n", nm2);
	  ret_break = 1;
	}
      }
    }
    else if (type == 1) {
      BigInt *otmp = _sc->getInt (goff);
      if (*otmp != v) {
	if (verb & 1) {
	  msgPrefix ();
	  printf ("%s := ", nm->s);
	  v.decPrint (stdout);
	  printf (" (0x");
	  v.hexPrint (stdout);
	  printf (")");
	  if (cause) {
	    char buf[1024];
	    ActSimDES *x = (ActSimDES *) cause;
	    x->sPrintCause (buf, 1024);
	    printf ("   [by %s]", buf);
	  }
	  printf ("\n");
	  _sc->recordTrace (nm, type, ACT_CHAN_IDLE, v);
	}
	if (verb & 2) {
	  msgPrefix ();
	  printf ("*** breakpoint %s\n", nm2);
	  ret_break = 1;
	}	    
      }
    }
    else if (type == 2) {
      /* chan-in--recv
	 recv (flag & 0x1 : fragmented channel operation)
	 (flag >> 1) : 0 = normal, 1 = blocked
      */
      if (verb & 1) {
	int umode;
	umode = (flag >> 1);
	msgPrefix();
	printf ("%s: recv", nm->s);

	if (umode == 1) {
	  printf ("-blocked");
	  _sc->recordTrace (nm, 2, ACT_CHAN_RECV_BLOCKED, v);
	}
	else if (umode == 0 || umode == 2) {
	  if (umode == 2) {
	    printf ("-wakeup");
	  }
	  printf (" value: ");
	  v.decPrint (stdout);
	  printf (" (0x");
	  v.hexPrint (stdout);
	  printf (")");
	}
	if (cause) {
	  char buf[1024];
	  ActSimDES *x = (ActSimDES *) cause;
	  x->sPrintCause (buf, 1024);
	  printf ("   [by %s]", buf);
	}
	printf ("\n");
      }
      if (verb & 2) {
	msgPrefix ();
	printf ("*** breakpoint %s\n", nm2);
	ret_break = 1;
      }
    }
    else if (type == 3) {
      /* send (flag & 0x1 : fragmented channel operation)
	 (flag >> 1) : 0 = normal, 1 = blocked, 2 = completed
      */
      if (verb & 1) {
	int umode;
	msgPrefix ();
	umode = (flag >> 1);
	if (umode == 0 || umode == 1) {
	  printf ("%s : send%s", nm->s, umode == 1 ? "-blocked" : "");
	  if (!(flag & 1)) {
	    /* not fragmented, display value */
	    printf (" value: ");
	    v.decPrint (stdout);
	    printf (" (0x");
	    v.hexPrint (stdout);
	    printf (")");
	  }
	  if (cause) {
	    char buf[1024];
	    ActSimDES *x = (ActSimDES *) cause;
	    x->sPrintCause (buf, 1024);
	    printf ("   [by %s]", buf);
	  }
	  printf ("\n");
	  if (umode == 1) {
	    _sc->recordTrace (nm, 2, ACT_CHAN_SEND_BLOCKED, v);
	    ChanTraceDelayed *obj = new ChanTraceDelayed (nm, v);
	    new Event (obj, SIM_EV_MKTYPE (0, 0), 1);
	  }
	  else {
	    _sc->recordTrace (nm, 2, ACT_CHAN_VALUE, v);
	    ChanTraceDelayed *obj = new ChanTraceDelayed (nm);
	    new Event (obj, SIM_EV_MKTYPE (0, 0), 1);
	  }
	}
	else {
	  ChanTraceDelayed *obj = new ChanTraceDelayed (nm);
	  new Event (obj, SIM_EV_MKTYPE (0, 0), 1);
	  printf ("%s : send complete", nm->s);
	  if (cause) {
	    char buf[1024];
	    ActSimDES *x = (ActSimDES *) cause;
	    x->sPrintCause (buf, 1024);
	    printf ("   [by %s]", buf);
	  }
	  printf ("\n");
	}
	
	if (verb & 2) {
	  msgPrefix ();
	  printf ("*** breakpoint %s\n", nm2);
	  ret_break = 1;
	}
      }
    }
  }
  return ret_break;
}

void ChpSim::_zeroStructure (struct chpsimderef *d)
{
  Assert (d, "Hmm");
  int *struct_info;
  int struct_len;

  state_counts ts;
  ActStatePass::getStructCount (d->d, &ts);
  
  if (d->range) {
    /* array deref */
    for (int i=0; i < d->range->nDims(); i++) {
      BigInt res = exprEval (d->chp_idx[i]);
      d->idx[i] = res.getVal (0);
    }
    int x = d->range->Offset (d->idx);
    if (x == -1) {
      fprintf (stderr, "In: ");
      if (getName()) {
	getName()->Print (stderr);
      }
      else {
	fprintf (stderr, "<>");
      }
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
    int off_i, off_b, off;
    MALLOC (struct_info, int, 3*(ts.numInts() + ts.numBools()));
    off_i = d->offset + x*ts.numInts();
    off_b = d->width + x*ts.numBools();
    off = 0;
    _add_deref_struct2 (d->d, struct_info, &off_i, &off_b, &off);
  }
  else {
    struct_info = d->idx;
  }
  struct_len = 3*(ts.numInts() + ts.numBools());
  for (int i=0; i < struct_len/3; i++) {
    int off = getGlobalOffset (struct_info[3*i],
			       struct_info[3*i+1] == 2 ?
			       1 : struct_info[3*i+1]);
    if (struct_info[3*i+1] == 1) {
      BigInt tmp;
      tmp.setWidth (struct_info[3*i+2]);
      tmp.toStatic ();
      _sc->setInt (off, tmp);
    }
    else if (struct_info[3*i+1] == 2) {
      /* enum */
      BigInt tmpv (64, 0, 0);
      tmpv.setVal (0, struct_info[3*i+2]);
      tmpv.setWidth (_ceil_log2 (struct_info[3*i+2]));
      tmpv.toStatic ();
      _sc->setInt (off, tmpv);
    }
    else {
      Assert (struct_info[3*i+1] == 0, "What?");
    }
  }

  if (struct_info != d->idx) {
    FREE (struct_info);
  }
}


void ChpSim::_zeroAllIntsChans (ChpSimGraph *g)
{
  int cnt;
  /* set bitwidths for integers */
  if (!g || !g->stmt) {
    return;
  }
  if (_addhash (g)) {
    return;
  }
  
  switch (g->stmt->type) {
  case CHPSIM_FORK:
    for (int i=0; i < g->stmt->u.fork; i++) {
      if (g->all[i]) {
	_zeroAllIntsChans (g->all[i]);
      }
    }
    break;

  case CHPSIM_ASSIGN:
    /* ok now get the int! */
    if (g->stmt->u.assign.is_struct) {
      _zeroStructure (&g->stmt->u.assign.d);
    }
    else {
      if (g->stmt->u.assign.isint != 0) {
	int off;
	BigInt v;
	off = computeOffset (&g->stmt->u.assign.d);
	off = getGlobalOffset (off, 1);
	v.setWidth (g->stmt->u.assign.isint);
	v.toStatic();
	_sc->setInt (off, v);
      }
    }
    break;

  case CHPSIM_SEND:
  case CHPSIM_RECV:
    {
      int off;
      off = getGlobalOffset (g->stmt->u.sendrecv.chvar, 2);
      act_channel_state *ch = _sc->getChan (off);
      ch->width = g->stmt->u.sendrecv.width;

      if (g->stmt->u.sendrecv.d) {
	if (g->stmt->u.sendrecv.d->d) {
	  _zeroStructure (g->stmt->u.sendrecv.d);
	}
	else {
	  off = computeOffset (g->stmt->u.sendrecv.d);
	  if (g->stmt->u.sendrecv.d_type == 0) {
	    // nothing to do
	  }
	  else {
	    BigInt v;
	    off = getGlobalOffset (off, 1);
	    v.setWidth (g->stmt->u.sendrecv.width);
	    v.toStatic();
	    _sc->setInt (off, v);
	  }
	}
      }
      if (g->stmt->u.sendrecv.e) {
	Expr *e = g->stmt->u.sendrecv.e;
	if (e->type == E_CHP_VARSTRUCT || e->type == E_CHP_VARSTRUCT_DEREF) {
	  _zeroStructure ((struct chpsimderef *)e->u.e.l);
	}
      }
    }
    break;

  case CHPSIM_COND:
  case CHPSIM_CONDARB:
  case CHPSIM_LOOP:
    cnt = 0;
    for (struct chpsimcond *x = &g->stmt->u.cond.c; x; x = x->next) {
      _zeroAllIntsChans (g->all[cnt]);
      cnt++;
    }
    break;

  case CHPSIM_FUNC: // log or assert
  case CHPSIM_NOP:
    break;

  default:
    fprintf (stderr, "TYPE = %d\n", g->stmt->type);
    Assert (0, "Unknown chpsim type?");
    break;
  }
  _zeroAllIntsChans (g->next);
}



void ChpSim::awakenDeadlockedGC ()
{
  if (_deadlock_pc) {
    int x;
    while (!list_isempty (_deadlock_pc)) {
      x = list_delete_ihead (_deadlock_pc);
      if (_pc[x]) {
	new Event (this, SIM_EV_MKTYPE (x,0), 0);
      }
    }
  }
}


void ChpSim::skipChannelAction (int is_send, int offset)
{
  act_channel_state *c = _sc->getChan (offset);
  int chk_pc;

  if (is_send) {
    chk_pc = c->send_here - 1;
  }
  else {
    chk_pc = c->recv_here - 1;
  }


  if (chk_pc < 0 || chk_pc >= _npc) {
    printf ("unexpected error!\n");
    return;
  }

  if (!_pc[chk_pc]) {
    printf ("unexpected error2!\n");
    return;
  }

  c->skip_action = 1;
  
  // clear the channel state
  if (is_send) {
    c->w->Notify (chk_pc);
    c->send_here = 0;
  }
  else {
    c->w->Notify (chk_pc);
    c->recv_here = 0;
  }
}

int ChpSim::jumpTo (const char *l)
{
  hash_bucket_t *b;
  if (!_labels) {
    fprintf (stderr, ">> goto operation failed; no labels in process!\n");
    return 0;
  }
  b = hash_lookup (_labels, l);
  if (!b) {
    fprintf (stderr, ">> goto operation failed; label `%s' does not exist!\n", l);
    return 0;
  }
  if (_pcused > 1) {
    fprintf (stderr, ">> goto operation failed; multiple (%d) active threads of execution.\n", _pcused);
    return 0;
  }
  int slot = -1;
  for (int i=0; i < _npc; i++) {
    if (_pc[i]) {
      if (slot == -1) {
	slot = i;
      }
      else {
	warning ("Internal issue with goto operation!");
      }
    }
  }
  if (slot == -1) {
    fprintf (stderr, ">> goto operation failed; no active program counter?\n");
    return 0;
  }
  _pc[slot] = (ChpSimGraph *) b->v;
  return 1;
}
