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
#include "prssim.h"
#include "xycesim.h"
#include <time.h>
#include <math.h>
#include <ctype.h>

/*

  Core ACT simulation library.

  A simulation is initially empty.

  There will be a certain ACT point that is the root of the
  simulation. This root is either the global namespace, or a process.

  By default, the simulation engine will pull in all the pieces of the
  design, and build a discrete-event simulation engine for the entire
  circuit.

  If you want to do something more special, then the simulation engine
  can be initialized with the root, and then pieces can be added to
  the simulation engine.

*/

struct process_info {
  process_info() { chp = NULL; hse = NULL; prs = NULL; }
  chpsimgraph_info *chp, *hse;
  PrsSimGraph *prs;
};


char *ActSimCore::_trname[TRACE_NUM_FORMATS] =
  { NULL, NULL, NULL };


ActSimCore::ActSimCore (Process *p)
{
  _have_filter = 0;
  for (int i=0; i < TRACE_NUM_FORMATS; i++) {
    _trfn[i] = NULL;
    _tr[i] = NULL;
  }
  
  _black_box_mode = config_get_int ("net.black_box_mode");

  A_INIT (_rand_init);
  
  if (!p) {
    root_is_ns = 1;
    root_lang = ActNamespace::Global()->getlang();
    root_scope = ActNamespace::Global()->CurScope();
    simroot = NULL;
  }
  else {
    simroot = p;
    root_lang = p->getlang();
    root_scope = p->CurScope();
  }
  state = NULL;
  a = ActNamespace::Act();
  map = ihash_new (8);
  ewidths = phash_new (4);
  chan = NULL;
  I.H = NULL;
  I.obj = NULL;

  _is_internal_parallel = 0;

  if (!root_scope->isExpanded()) {
    fatal_error ("Need to expand ACT prior to starting a simulation");
  }

  ActPass *tmp;
  tmp = a->pass_find ("collect_state");
  if (!tmp) {
    fatal_error ("Please run the state pass prior to using the simulator");
  }
  sp = dynamic_cast<ActStatePass *> (tmp);
  Assert (sp, "what?");

  tmp = a->pass_find ("booleanize");
  if (!tmp) {
    fatal_error ("Please run the Booleanize pass!");
  }
  bp = dynamic_cast<ActBooleanizePass *> (tmp);
  Assert (bp, "What?");
  
  stateinfo_t *si = sp->getStateInfo (p);
  
  if (!si) {
    fatal_error ("No state information recorded for process `%s'",
		 p ? p->getName() : "-global-");
  }

  /* 
     Allocate state.
     We need:
     - all ports (all port bools, and chp ports)
     - all local vars including sub-instance local vars
  */
  state_counts globals = sp->getGlobals();

  state = new ActSimState (si->ports.numAllBools() + si->all.numAllBools()
			   + globals.numAllBools(),
			   si->ports.numInts() + si->all.numInts() +
			   globals.numInts(),
			   si->ports.numChans() +  si->all.numChans() +
			   globals.numChans());

  nfo_len = si->ports.numAllBools() + si->all.numAllBools()
    + globals.numAllBools() + si->ports.numInts() + si->all.numInts()
    + globals.numInts();

  nint_start = si->ports.numAllBools() + si->all.numAllBools()
    + globals.numAllBools();

  if (nfo_len > 0) {
    MALLOC (nfo, int, nfo_len);
    MALLOC (fo, SimDES **, nfo_len);
    for (int i=0; i < nfo_len; i++) {
      nfo[i] = 0;
      fo[i] = NULL;
    }
  }
  else {
    nfo = NULL;
    fo = NULL;
  }
  hfo = NULL;

  _seed = 0;
  _rand_min = 1;
  _rand_max = 100;
  _sim_rand_excl = 0;
  _sim_rand = 0;
  _prs_sim_mode = 0;
  _on_warning = 0;

  _chp_sim_objects = list_new ();
  
  _rootsi = si;


  _W = ihash_new (4);
  _B = ihash_new (4);

  _initSim();

  _register_prssim_with_excl (&I);

  _inf_loop_opt = 0;
  if (config_exists ("sim.chp.inf_loop_opt") &&
      (config_get_int ("sim.chp.inf_loop_opt") == 1)) {
    _inf_loop_opt = 1;
  }
}

static void _delete_sim_objs (ActInstTable *I, int del)
{
  hash_bucket_t *b;
  hash_iter_t hi;
  if (!I) return;

  if (I->obj) {
    delete I->obj;
  }
  I->obj = NULL;

  if (I->H) {
    hash_iter_init (I->H, &hi);
    while ((b = hash_iter_next (I->H, &hi))) {
      ActInstTable *x = (ActInstTable *)b->v;
      _delete_sim_objs (x, 1);
    }
    hash_free (I->H);
  }
  I->H = NULL;
  if (del) {
    FREE (I);
  }
}
      
ActSimCore::~ActSimCore()
{
  Assert (_rootsi, "What");

  A_FREE (_rand_init);
  
  for (int i=0; i < A_LEN (_rootsi->bnl->used_globals); i++) {
    act_booleanized_var_t *v;
    ihash_bucket_t *b;
    b = ihash_lookup (_rootsi->bnl->cH, (long)_rootsi->bnl->used_globals[i].c);
    Assert (b, "WHat?");
    v = (act_booleanized_var_t *) b->v;
    Assert (v, "What?");
    Assert (v->isglobal, "What?");
  }

  /* free chp */
  if (map) {
    for (int i=0; i < map->size; i++) {
      for (ihash_bucket_t *b = map->head[i]; b; b = b->next) {
	process_info *pgi = (process_info *)b->v;
	if (pgi->chp) {
	  delete pgi->chp->g;
	  FREE (pgi->chp);
	}
	if (pgi->hse) {
	  delete pgi->hse->g;
	  FREE (pgi->hse);
	}
	if (pgi->prs) {
	  delete pgi->prs;
	}
	delete pgi;
      }
    }
    ihash_free (map);
  }

  if (chan) {
    phash_iter_t iter;
    phash_bucket_t *b;
    phash_iter_init (chan, &iter);
    while ((b = phash_iter_next (chan, &iter))) {
      ChanMethods *ch = (ChanMethods *)b->v;
      delete ch;
    }
    phash_free (chan);
  }

  if (ewidths) {
    phash_free (ewidths);
  }

  if (state) {
    delete state;
  }

  /*-- fanout tables --*/
  if (nfo) {
    Assert (fo, "What?");
    for (int i=0; i < nfo_len; i++) {
      if (nfo[i] > 0) {
	Assert (fo[i], "What?");
	FREE (fo[i]);
      }
      else {
	Assert (!fo[i], "Hmm");
      }
    }
    FREE (fo);
    FREE (nfo);
  }
  if (hfo) {
    ihash_free (hfo);
  }

  /*-- chp objects --*/
  list_free (_chp_sim_objects);

  ihash_bucket_t *b;
  ihash_iter_t it;
  ihash_iter_init (_W, &it);
  while ((b = ihash_iter_next (_W, &it))) {
    FREE (b->v);
  }
  ihash_free (_W);
  ihash_iter_init (_B, &it);
  while ((b = ihash_iter_next (_B, &it))) {
    FREE (b->v);
  }
  ihash_free (_B);

  /*-- instance tables --*/
  _delete_sim_objs (&I, 0);

  /*-- close any pending trace files --*/
  for (int i=0; i < TRACE_NUM_FORMATS; i++) {
    if (_tr[i]) {
      act_trace_close (_tr[i]);
    }
  }
}


ChpSim *ActSimCore::_add_chp (act_chp *c)
{
  ihash_bucket_t *b;

#if 0  
  printf ("add-chp-inst: ");
  if (_curinst) {
    _curinst->Print (stdout);
  }
  else {
    printf ("-none-");
  }
  printf ("\n");
#endif

  ChpSim *x;
  process_info *pgi = NULL;
  
  if (c) {
    b = ihash_lookup (map, (long)_curproc);
    if (b) {
      pgi = (process_info *) b->v;
    }
    else {
      pgi = new process_info();
      b = ihash_add (map, (long)_curproc);
      b->v = pgi;
    }
    if (!pgi->chp) {
      if (c->c && c->c->type == ACT_CHP_COMMA) {
	setInternalParallel (1);
      }
      else {
	setInternalParallel (0);
      }
      pgi->chp = ChpSimGraph::buildChpSimGraph (this, c->c);
    }
    x = new ChpSim (pgi->chp, c->c, this, _curproc);
  }
  else {
    x = new ChpSim (NULL, NULL, this, _curproc);
  }
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
  x->zeroInit ();

  list_append (_chp_sim_objects, x);

  return x;
}

ActSimObj *ActSimCore::_add_dflow (act_dataflow *d)
{
  printf ("add-dflow-inst (not currently supported): ");
  if (_curinst) {
    _curinst->Print (stdout);
  }
  else {
    printf ("-none-");
  }
  printf ("\n");
  return NULL;
}


ChpSim *ActSimCore::_add_hse (act_chp *c)
{
  ihash_bucket_t *b;
#if 0 
  printf ("add-hse-inst: ");
  if (_curinst) {
    _curinst->Print (stdout);
  }
  else {
    printf ("-none-");
  }
  printf ("\n");
#endif  

  process_info *pgi;
  b = ihash_lookup (map, (long)_curproc);
  if (b) {
    pgi = (process_info *)b->v;
  }
  else {
    pgi = new process_info();
    b = ihash_add (map, (long)_curproc);
    b->v = pgi;
  }

  if (!pgi->hse) {
    if (c->c && c->c->type == ACT_CHP_COMMA) {
      setInternalParallel (1);
    }
    else {
      setInternalParallel (0);
    }
    pgi->hse = ChpSimGraph::buildChpSimGraph (this, c->c);
  }
  ChpSim *x = new ChpSim (pgi->hse, c->c, this, _curproc);
  x->setHseMode ();
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
  
  return x;
}

PrsSim *ActSimCore::_add_prs (act_prs *p)
{
#if 0  
  printf ("add-prs-inst: ");
  if (_curinst) {
    _curinst->Print (stdout);
  }
  else {
    printf ("-none-");
  }
  printf ("\n");
#endif

  process_info *pgi;
  ihash_bucket_t *b;
  b = ihash_lookup (map, (long)_curproc);
  if (b) {
    pgi = (process_info *)b->v;
  }
  else {
    b = ihash_add (map, (long)_curproc);
    pgi = new process_info ();
    b->v = pgi;
  }

  if (!pgi->prs) {
    pgi->prs = PrsSimGraph::buildPrsSimGraph (this, p);
  }
  
  /* need prs simulation graph */

  PrsSim *x = new PrsSim (pgi->prs, this, _curproc);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);

  return x;
}

XyceSim *ActSimCore::_add_xyce ()
{
  XyceSim *x = new XyceSim (this, _curproc);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);

  return x;
}


/*
 *      sc = Scope of parent 
 *  parent = root of identifier
 *      tl = tail pointer of parent id
 *    rest = additional bit of ID to be appended
 *
 *  Returns canonical pointer in "sc" scope of "parent . rest",
 *  assuming "tl" is the parent tail pointer
 *
 */
static act_connection *_parent_canonical (Scope *sc,
					  ActId *parent, ActId *tl, ActId *rest)
{
  act_connection *ret;
  
  if (tl) {
    tl->Append (rest);
    ret = parent->Canonical (sc);
    tl->prune ();
  }
  else {
    ret = rest->Canonical (sc);
  }
  return ret;
}


void ActSimCore::_add_spec (ActSimObj *obj, act_spec *s)
{
  stateinfo_t *si;
  ActId *p_tl;
  int type;
  A_DECL (int, idx);
  
  if (!s) return;

  si = sp->getStateInfo (obj->getProc());
  /* XXX: this can happen for wiring processes? */
  Assert (si, "No stateinfo found?");

  if (_cursuffix) {
    p_tl = _cursuffix->Tail();
  }
  else {
    p_tl = NULL;
  }

  Scope *sc = si->bnl->cur;
  A_INIT (idx);
  
  while (s) {
    if (ACT_SPEC_ISTIMINGFORK (s)) {
      InstType *it[3];
      Array *aref[3];
      act_connection *r, *a, *b;
      ActId *tl[2];
      Arraystep *as[2];
      int roff, aoff, boff;
      int valid_constr = 1;

      for (int i=0; i < 3; i++) {
	if (p_tl) {
	  p_tl->Append (s->ids[i]);
	  it[i] = sc->FullLookup (_cursuffix, &aref[i]);
	  p_tl->prune();
	}
	else {
	  it[i] = sc->FullLookup (s->ids[i], &aref[i]);
	}
	if (!it[i]) {
	  valid_constr = 0;
	}
      }
      
      if (!valid_constr) {
	/* constraint with unknown signals */
	s = s->next;
	continue;
      }

      if (it[0]->arrayInfo() && (!aref[0] || !aref[0]->isDeref())) {
	fprintf (stderr, "Timing constraint error for `%s'\n",
		 obj->getProc()->getName());
	fprintf (stderr, "  LHS `");
	s->ids[0]->Print (stderr);
	fprintf (stderr, "' is an array.\n");
	s = s->next;
	continue;
      }
      
      r = _parent_canonical (sc, _cursuffix, p_tl, s->ids[0]);
      if (!hasLocalOffset (r, si)) {
	/* just skip it! */
	s = s->next;
	continue;
      }
      roff = getLocalOffset (r, si, &type, NULL);
      Assert (type == 0, "Non-boolean in timing spec");
      //roff = obj->getGlobalOffset (roff, 0);

      for (int i=1; i < 3; i++) {
	if (it[i]->arrayInfo() && (!aref[i] || !aref[i]->isDeref())) {
	  if (aref[i]) {
	    as[i-1] = it[i]->arrayInfo()->stepper (aref[i]);
	  }
	  else {
	    as[i-1] = it[i]->arrayInfo()->stepper ();
	  }
	  tl[i-1] = s->ids[i]->Tail();
	}
	else {
	  as[i-1] = NULL;
	  tl[i-1] = NULL;
	}
      }

      if (!as[0] && !as[1]) {
	a = _parent_canonical (sc, _cursuffix, p_tl, s->ids[1]);
	b = _parent_canonical (sc, _cursuffix, p_tl, s->ids[2]);

	if (hasLocalOffset (a, si) && hasLocalOffset (b, si)) {
	  aoff = getLocalOffset (a, si, &type, NULL);
	  Assert (type == 0, "Non-boolean in timing spec");
	  //aoff = obj->getGlobalOffset (aoff, 0);
	  boff = getLocalOffset (b, si, &type, NULL);
	  Assert (type == 0, "Non-boolean in timing spec");
	  //boff = obj->getGlobalOffset (boff, 0);

	  _add_timing_fork (obj, si, roff, aoff, boff, (Expr *)s->ids[3], s->extra);
	}
      }
      else {
	for (int i=1; i < 3; i++) {
	  if (aref[i]) {
	    tl[i-1]->setArray (NULL);
	  }
	}

	while ((as[0] && !as[0]->isend()) ||
	       (as[1] && !as[1]->isend())) {
	  Array *ta[2];

	  for (int i=1; i < 3; i++) {
	    if (as[i-1]) {
	      ta[i-1] = as[i-1]->toArray();
	      tl[i-1]->setArray (ta[i-1]);
	    }
	    else {
	      ta[i-1] = NULL;
	    }
	  }

	  a = _parent_canonical (sc, _cursuffix, p_tl, s->ids[1]);
	  b = _parent_canonical (sc, _cursuffix, p_tl, s->ids[2]);

	  if (hasLocalOffset (a, si) && hasLocalOffset (b, si)) {
	    aoff = getLocalOffset (a, si, &type, NULL);
	    Assert (type == 0, "Non-boolean in timing spec");
	    //aoff = obj->getGlobalOffset (aoff, 0);
	    boff = getLocalOffset (b, si, &type, NULL);
	    Assert (type == 0, "Non-boolean in timing spec");
	    //boff = obj->getGlobalOffset (boff, 0);

	    _add_timing_fork (obj, si, roff, aoff, boff, (Expr *)s->ids[3], s->extra);
	  }

	  for (int i=1; i < 3; i++) {
	    if (ta[i-1]) {
	      delete ta[i-1];
	    }
	  }
	  
	  if (as[1]) {
	    as[1]->step();
	    if (as[1]->isend()) {
	      if (as[0]) {
		as[0]->step();
		if (!as[0]->isend()) {
		  delete as[1];
		  if (aref[2]) {
		    as[1] = it[2]->arrayInfo()->stepper (aref[2]);
		  }
		  else {
		    as[1] = it[2]->arrayInfo()->stepper ();
		  }
		}
	      }
	    }
	  }
	  else {
	    if (as[0]) {
	      as[0]->step();
	    }
	  }
	}

	if (as[0]) {
	  delete as[0];
	}
	if (as[1]) {
	  delete as[1];
	}

	for (int i=1; i < 3; i++) {
	  if (tl[i-1]) {
	    tl[i-1]->setArray (aref[i]);
	  }
	}
      }
    }
    else if (ACT_SPEC_ISTIMING(s)) {
      /* ignore timing graph specifiers */
    }
    else {
      const char *tmp = act_spec_string (s->type);
      if (strcmp (tmp, "mk_exclhi") == 0 || strcmp (tmp, "mk_excllo") == 0 ||
	  strcmp (tmp, "rand_init") == 0 || strcmp (tmp, "hazard") == 0) {

	/* helper function to add the ID to the list */
	auto add_id_to_idx = [&] (ActId *id_val, ValueIdx *myvx)
        {
	  Array *aref;
	  InstType *it;
	  Arraystep *astep;
	  int off;
	  act_connection *cx;
	    
	  if (p_tl) {
	    p_tl->Append (id_val);
	    it = sc->FullLookup (_cursuffix, &aref);
	    p_tl->prune();
	  }
	  else {
	    it = sc->FullLookup (id_val, &aref);
	  }

	  astep = NULL;
	  if (it->arrayInfo() && (!aref || !aref->isDeref())) {
	    if (aref) {
	      astep = it->arrayInfo()->stepper (aref);
	    }
	    else {
	      astep = it->arrayInfo()->stepper ();
	    }
	  }
	  if (!astep) {
	    cx = _parent_canonical (sc, _cursuffix, p_tl, id_val);
	    if (hasLocalOffset (cx, si)) {
	      off = getLocalOffset (cx, si, &type, NULL);
	      Assert (type == 0, "Non-boolean in signal list");
	      off = obj->getGlobalOffset (off, 0);
	      A_NEW (idx, int);
	      A_NEXT (idx) = off;
	      A_INC (idx);
	    }
	  }
	  else {
	    ActId *tl = id_val->Tail();
	    int cnt = 0;
	    while (!astep->isend()) {
	      if (!myvx || myvx->isPrimary (cnt)) {
		Array *a = astep->toArray();
		tl->setArray (a);
		cx = _parent_canonical (sc, _cursuffix, p_tl, id_val);
		if (hasLocalOffset (cx, si)) {
		  off = getLocalOffset (cx, si, &type, NULL);
		  Assert (type == 0, "Non-boolean in signal list");
		  off = obj->getGlobalOffset (off, 0);
		  A_NEW (idx, int);
		  A_NEXT (idx) = off;
		  A_INC (idx);
		}
		tl->setArray (NULL);
		delete a;
	      }
	      cnt++;
	      astep->step();
	    }
	  }
	};
	
	/*-- signal list --*/
	for (int i=0; i < s->count; i++) {
	  add_id_to_idx (s->ids[i], NULL);
	}
	if (s->count < 0) {
	  ActInstiter it(sc);
	  for (it = it.begin(); it != it.end(); it++) {
	    ValueIdx *vx = *it;
	    if (!vx->isPrimary()) {
	      continue;
	    }
	    if (TypeFactory::isBoolType (vx->t) &&
		(vx->t->getDir() != Type::IN)) {
	      ActId *tmp = new ActId (vx->getName());
	      add_id_to_idx (tmp, vx);
	      delete tmp;
	    }
	  }
	}
	if (strcmp (tmp, "mk_exclhi") == 0) {
	  _add_excl (1, idx, A_LEN (idx));
	}
	else if (strcmp (tmp, "mk_excllo") == 0) {
	  _add_excl (0, idx, A_LEN (idx));
	}
	else if (strcmp (tmp, "rand_init") == 0) {
	  _add_rand_init (idx, A_LEN (idx));
	}
	else {
	  _add_hazard (idx, A_LEN (idx));
	}
	A_LEN_RAW (idx) = 0;
      }
    }
    s = s->next;
  }
  A_FREE (idx);
}

  

void ActSimCore::_check_fragmentation (ChpSim *c)
{
  ChpSimGraph::checkFragmentation (this, c, _curproc->getlang()->gethse()->c);
}

void ActSimCore::_check_fragmentation (PrsSim *p)
{
  PrsSimGraph::checkFragmentation (this, p, _curproc->getlang()->getprs());
}

void ActSimCore::_check_fragmentation (XyceSim *x)
{
  stateinfo_t *si = x->getSI();
  act_boolean_netlist_t *bnl = x->getBNL();

  Assert (bnl, "Hmm");
  for (int i=0; i < A_LEN (bnl->ports); i++) {
    if (bnl->ports[i].omit) continue;

    ActId *tmp = bnl->ports[i].c->toid();

    if (!tmp->isFragmented (bnl->cur)) continue;

    if (!si) { continue; }

    ActId *un = tmp->unFragment (bnl->cur);
    int type;
    int loff = getLocalOffset (un, si, &type);

    if (type == 2 || type == 3) {
      loff = x->getGlobalOffset (loff, 2);
      act_channel_state *ch = getChan (loff);

      if (type == 2) {
	/* input */
	ch->fragmented |= 1;
      }
      else {
	ch->fragmented |= 2;
	/* output */
      }
      sim_recordChannel (this, x, un);
      registerFragmented (ch->ct);
      ch->cm = getFragmented (ch->ct);
    }
    delete un;
    delete tmp;
  }
}

int ActSimCore::_getlevel ()
{
  int lev;

  lev = -1;
  
  if (_curinst) {
    lev = ActNamespace::Act()->getLevel (_curinst);
  }
  if (lev == -1 && _curproc) {
    lev = ActNamespace::Act()->getLevel (_curproc);
  }
  if (lev == -1) {
    lev = ActNamespace::Act()->getLevel ();
  }
  
  return lev;
}

void ActSimCore::_add_language (int lev, act_languages *l)
{
  if (!l) {
    _curI->obj = _add_chp (NULL);
    return;
  }

  /*- we have the full path here -*/
  if ((l->getchp() || l->getdflow()) && lev == ACT_MODEL_CHP) {
    /* chp or dataflow */
    if (l->getchp()) {
      _curI->obj = _add_chp (l->getchp());
    }
    else {
      _curI->obj = _add_dflow (l->getdflow());
    }
  }
  else if (l->gethse() && lev == ACT_MODEL_HSE) {
    ChpSim *x;
    /* hse */
    _check_fragmentation ((x = _add_hse (l->gethse())));
    _curI->obj = x;
  }
  else if (l->getprs() && lev == ACT_MODEL_PRS) {
    /* prs */
    PrsSim *x;
    _check_fragmentation ((x = _add_prs (l->getprs())));
    _curI->obj = x;
  }
  else if (lev == ACT_MODEL_DEVICE) {
    XyceSim *x;
    _check_fragmentation ((x = _add_xyce ()));
    _curI->obj = x;
  }
  else {
    /* substitute a less detailed model, if possible */
    if (l->gethse() && lev == ACT_MODEL_PRS) {
      ChpSim *x;
      if (Act::lang_subst) {
	warning ("%s: substituting hse model (requested %s, not found)", _curproc ? _curproc->getName() : "-top-", act_model_names[lev]);
      }
      _check_fragmentation ((x = _add_hse (l->gethse())));
      _curI->obj = x;
    }
    else if ((l->getdflow() || l->getchp()) &&
	     (lev == ACT_MODEL_PRS || lev == ACT_MODEL_HSE)) {
      if (l->getdflow() && !l->getchp() /* XXX: currently we don't
					   directly simulate dataflow */) {
	if (Act::lang_subst) {
	  warning ("%s: substituting dataflow model (requested %s, not found)", _curproc ? _curproc->getName() : "-top-", act_model_names[lev]);
	}
	_curI->obj = _add_dflow (l->getdflow());
      }
      else {
	Assert (l->getchp(), "What?");
	if (Act::lang_subst) {
	  warning ("%s: substituting chp model (requested %s, not found)", _curproc ? _curproc->getName() : "-top-", act_model_names[lev]);
	}
	_curI->obj = _add_chp (l->getchp());
      }
    }
    else {
      if (l->getchp() || l->getdflow() || l->gethse() || l->getprs()) {
	fprintf (stderr, "%s: circuit found, but not at the modeling level requested [level=%s]\n", _curproc ? _curproc->getName() : "-top-", act_model_names[lev]);
	fprintf (stderr, "  found:");
	if (l->getchp()) {
	  fprintf (stderr, " %s", act_model_names[ACT_MODEL_CHP]);
	}
	if (l->getdflow()) {
	  fprintf (stderr, " %s[d-flow]", act_model_names[ACT_MODEL_CHP]);
	}
	if (l->gethse()) {
	  fprintf (stderr, " %s", act_model_names[ACT_MODEL_HSE]);
	}
	if (l->getprs()) {
	  fprintf (stderr, " %s", act_model_names[ACT_MODEL_PRS]);
	}
	fprintf (stderr, "\n");
	fatal_error ("Model level requirement inconsistent with ACT");
      }
      else {
	_curI->obj = _add_chp (NULL);
      }
    }
  }
  if (_curI->obj) {
    _cursuffix = NULL;
    _add_spec (_curI->obj, l->getspec());
  }
}

void ActSimCore::_check_add_spec (const char *name, InstType *it,
				  ActSimObj *obj) 
{
  UserDef *u;
  if (!TypeFactory::isUserType (it)) {
    return;
  }
  u = dynamic_cast<UserDef *> (it->BaseType());
  Arraystep *as;
  if (it->arrayInfo()) {
    as = new Arraystep (it->arrayInfo());
  }
  else {
    as = NULL;
  }
  
  ActId *tmpid, *previd;

  if (!_cursuffix) {
    _cursuffix = new ActId (name);
    tmpid = _cursuffix;
    previd = NULL;
  }
  else {
    tmpid = _cursuffix->Tail();
    tmpid->Append (new ActId (name));
    previd = tmpid;
    tmpid = tmpid->Rest();
  }
  
  do {
    if (as) {
      tmpid->setArray (as->toArray ());
    }

    /* _cursuffix has the name */
    if (u->getlang() && u->getlang()->getspec()) {
      _add_spec (obj, u->getlang()->getspec());
    }
    for (int i=0; i < u->getNumPorts(); i++) {
      InstType *it = u->getPortType (i);
      if (TypeFactory::isUserType (it)) {
	_check_add_spec (u->getPortName (i), it, obj);
      }
    }

    if (as) {
      Array *atmp = tmpid->arrayInfo();
      delete atmp;
      tmpid->setArray (NULL);
    }
    if (as) {
      as->step();
    }
  } while (as && !as->isend());

  if (previd) {
    previd->prune();
    delete tmpid;
  }
  else {
    delete _cursuffix;
    _cursuffix = NULL;
  }
  if (as) {
    delete as;
  }
}

static void _prop_used_flags (act_boolean_netlist_t *bnl, act_connection *c)
{
  for (int i=0; i < A_LEN (bnl->ports); i++) {
    if (bnl->ports[i].c == c) {
      bnl->ports[i].used = 1;
      break;
    }
  }
  for (int i=0; i < A_LEN (bnl->chpports); i++) {
    if (bnl->chpports[i].c == c) {
      bnl->chpports[i].used = 1;
      break;
    }
  }
}


void ActSimCore::_add_all_inst (Scope *sc)
{
  int lev;
  int iportbool, iportchp;
  act_boolean_netlist_t *mynl;
  int *_my_port_int, *_my_port_chan, *_my_port_bool;
  stateinfo_t *mysi;
  state_counts myoffset;
  ActInstTable *myI;

  Assert (sc->isExpanded(), "What?");

  iportbool = 0;
  iportchp = 0;
  mynl = bp->getBNL (_curproc);
  _my_port_int = _cur_abs_port_int;
  _my_port_bool = _cur_abs_port_bool;
  _my_port_chan = _cur_abs_port_chan;
  mysi = _cursi;
  myoffset = _curoffset;
  myI = _curI;


  /* -- increment cur offset after allocating all the items -- */
  if (!_cursi) {
    return;
  }

  stack_push (_si_stack, mysi);
  stack_push (_obj_stack, myI->obj);
  
  _curoffset.addVar (_cursi->local);

  ActUniqProcInstiter ipt(sc);

  if (ipt.begin() != ipt.end()) {
    myI->H = hash_new (4);
  }

  for (ipt = ipt.begin(); ipt != ipt.end(); ipt++) {
    ValueIdx *vx = (*ipt);
    stateinfo_t *si;
    Process *x = dynamic_cast<Process *> (vx->t->BaseType());
    Arraystep *as = NULL;
    Assert (x->isExpanded(), "What?");

    if (vx->t->arrayInfo()) {
      as = new Arraystep (vx->t->arrayInfo());
    }
    else {
      as = NULL;
    }

    ActId *tmpid, *previd;

    if (!_curinst) {
      _curinst = new ActId (vx->getName());
      tmpid = _curinst;
      previd = NULL;
    }
    else {
      tmpid = _curinst->Tail();
      tmpid->Append (new ActId (vx->getName()));
      previd = tmpid;
      tmpid = tmpid->Rest();
    }

    si = sp->getStateInfo (x);
    _cursi = si;

    do {
      _curproc = x;
      _cursi = si;

      if (!as || vx->isPrimary (as->index())) {
	if (as) {
	  tmpid->setArray (as->toArray());
	}

	/*-- tmpid = name of the id --*/

	char buf[1024];
	hash_bucket_t *ib;
	ActInstTable *iT;
	tmpid->sPrint (buf, 1024);
	ib = hash_add (myI->H, buf);
	NEW (iT, ActInstTable);
	iT->obj = NULL;
	iT->H = NULL;
	ib->v = iT;
	_curI = iT;

	/*-- compute ports for this process --*/
	lev = _getlevel();
	act_boolean_netlist_t *bnl = bp->getBNL (_curproc);

	int ports_exist = 0;
	int chpports_exist_int = 0;
	int chpports_exist_bool = 0;
	int chpports_exist_chan = 0;

	for (int i=0; i < A_LEN (bnl->ports); i++) {
	  if (bnl->ports[i].omit == 0) {
	    ports_exist++;
	  }
	}
	for (int i=0; i < A_LEN (bnl->chpports); i++) {
	  if (bnl->chpports[i].omit == 0) {
	    ValueIdx *lvx = bnl->chpports[i].c->getvx();
	    Assert (lvx, "What?");
	    if (TypeFactory::isChanType (lvx->t)) {
	      chpports_exist_chan++;
	    }
	    else if (TypeFactory::isBoolType (lvx->t)) {
	      chpports_exist_bool++;
	    }
	    else {
	      chpports_exist_int++;
	    }
	  }
	}

	/* compute port bool, int and chan ports */
	_cur_abs_port_bool = NULL;
	_cur_abs_port_int = NULL;
	_cur_abs_port_chan = NULL;

	if (chpports_exist_bool || chpports_exist_int || chpports_exist_chan ||
	    ports_exist) {
	  if (chpports_exist_chan) {
	    MALLOC (_cur_abs_port_chan, int, chpports_exist_chan);
	  }
	  if (chpports_exist_int) {
	    MALLOC (_cur_abs_port_int, int, chpports_exist_int);
	  }
	  if (chpports_exist_bool || ports_exist) {
	    MALLOC (_cur_abs_port_bool, int, chpports_exist_bool + ports_exist);
	  }
	}

	int ibool = 0;
	int iportbool_orig = iportbool;
	if (ports_exist) {
	  for (int i=0; i < A_LEN (bnl->ports); i++) {
	    if (bnl->ports[i].omit) continue;
	    Assert (iportbool < A_LEN (mynl->instports), "What?");

	    act_connection *c = mynl->instports[iportbool];

	    if (bnl->ports[i].used) {
	      checkFragmentation (c, NULL, myI->obj, mysi, bnl->ports[i].input);
	    }

	    int off = getLocalOffset (c, mysi, NULL);

	    if (sp->isGlobalOffset (off)) {
	      off = sp->globalIdx (off);
	    }
	    else if (sp->isPortOffset (off)) {
	      off = sp->portIdx (off);
	      off = _my_port_bool[off];
	    }
	    else {
	      /* local state */
	      off += myoffset.numAllBools();
	    }
	    _cur_abs_port_bool[ibool++] = off;
	    iportbool++;
	  }
	}
	
	int ichan = 0;
	int iint = 0;
	int iportchp_orig = iportchp;
	if (chpports_exist_int|| chpports_exist_bool || chpports_exist_chan) {
	  /* then we use the chp instports */
	  for (int i=0; i < A_LEN (bnl->chpports); i++) {
	    if (bnl->chpports[i].omit) continue;
	    Assert (iportchp < A_LEN (mynl->instchpports), "What?");
	    ValueIdx *lvx = bnl->chpports[i].c->getvx();
	    Assert (lvx, "Hmm");

	    act_connection *c = mynl->instchpports[iportchp];

	    if (bnl->chpports[i].used) {
	      checkFragmentation (c, NULL, myI->obj, mysi, bnl->chpports[i].input);
	    }

	    iportchp++;

	    ihash_bucket_t *xb = ihash_lookup (bnl->cH, (long)bnl->chpports[i].c);
	    if (xb) {
	      act_booleanized_var_t *v;
	      v = (act_booleanized_var_t *)xb->v;
	      if (v->used) continue; /* already covered */
	    }

	    int type;
	    int off = getLocalOffset (c, mysi, &type);

	    if (sp->isGlobalOffset (off)) {
	      off = sp->globalIdx (off);
	    }
	    else if (sp->isPortOffset (off)) {
	      off = sp->portIdx (off);
	      if (type == 2 || type == 3) {
		off = _my_port_chan[off];
	      }
	      else if (type == 1) {
		off = _my_port_int[off];
	      }
	      else {
		off = _my_port_bool[off];
	      }
	    }
	    else {
	      /* local state */
	      if (type == 2 || type == 3) {
		off += myoffset.numChans();
	      }
	      else if (type == 1) {
		off += myoffset.numInts();
	      }
	      else {
		off += myoffset.numAllBools();
	      }
	    }
	    if (TypeFactory::isChanType (lvx->t)) {
	      _cur_abs_port_chan[ichan++] = off;
	    }
	    else if (TypeFactory::isBoolType (lvx->t)) {
	      _cur_abs_port_bool[ibool++] = off;
	    }
	    else {
	      _cur_abs_port_int[iint++] = off;
	    }
	  }
	}

	for (int i=0; i < ibool/2; i++) {
	  int x = _cur_abs_port_bool[i];
	  _cur_abs_port_bool[i] = _cur_abs_port_bool[ibool-1-i];
	  _cur_abs_port_bool[ibool-1-i] = x;
	}
	for (int i=0; i < iint/2; i++) {
	  int x = _cur_abs_port_int[i];
	  _cur_abs_port_int[i] = _cur_abs_port_int[iint-1-i];
	  _cur_abs_port_int[iint-1-i] = x;
	}
	for (int i=0; i < ichan/2; i++) {
	  int x = _cur_abs_port_chan[i];
	  _cur_abs_port_chan[i] = _cur_abs_port_chan[ichan-1-i];
	  _cur_abs_port_chan[ichan-1-i] = x;
	}

#if 0
	printf ("inst: "); _curinst->Print (stdout); printf ("\n");
	printf ("  [bool %d] ", ibool);
	for (int i=0; i < ibool; i++) {
	  printf (" %d", _cur_abs_port_bool[i]);
	}
	printf ("\n");
	printf ("  [int %d] ", iint);
	for (int i=0; i < iint; i++) {
	  printf (" %d", _cur_abs_port_int[i]);
	}
	printf ("\n");
	printf ("  [chan %d] ", ichan);
	for (int i=0; i < ichan; i++) {
	  printf(" %d", _cur_abs_port_chan[i]);
	}
	printf ("\n");
#endif

	_add_language (lev, x->getlang());

	if (lev != ACT_MODEL_DEVICE) {
	  /* a device level model applies to the *entire* sub-tree */
	  _add_all_inst (x->CurScope());
	}


	iportbool = iportbool_orig;
	if (ports_exist) {
	  for (int i=0; i < A_LEN (bnl->ports); i++) {
	    if (bnl->ports[i].omit) continue;
	    Assert (iportbool < A_LEN (mynl->instports), "What?");
	    act_connection *c = mynl->instports[iportbool];

	    if (bnl->ports[i].used) {
	      //_prop_used_flags (mysi->bnl, c);
	      checkFragmentation (c, NULL, myI->obj, mysi, bnl->ports[i].input);
	    }
	    iportbool++;
	  }
	}
	iportchp = iportchp_orig;
	if (chpports_exist_int|| chpports_exist_bool || chpports_exist_chan) {
	  /* then we use the chp instports */
	  for (int i=0; i < A_LEN (bnl->chpports); i++) {
	    if (bnl->chpports[i].omit) continue;
	    Assert (iportchp < A_LEN (mynl->instchpports), "What?");
	    act_connection *c = mynl->instchpports[iportchp];

	    if (bnl->chpports[i].used) {
	      //_prop_used_flags (mysi->bnl, c);
	      checkFragmentation (c, NULL, myI->obj, mysi, bnl->chpports[i].input);
	    }

	    iportchp++;
	  }
	}

	if (as) {
	  Array *atmp = tmpid->arrayInfo();
	  delete atmp;
	  tmpid->setArray (NULL);
	}

      }
      
      if (as) {
	as->step();
      }
    } while (as && !as->isend());

    if (previd) {
      previd->prune();
      delete tmpid;
    }
    else {
      delete _curinst;
      _curinst = NULL;
    }
    if (as) {
      delete as;
    }
  }
  Assert (iportbool == A_LEN (mynl->instports), "What?");
  Assert (iportchp == A_LEN (mynl->instchpports), "What?");

  _cursuffix = NULL;
  ActInstiter it(sc);
  for (it = it.begin(); it != it.end(); it++) {
    ValueIdx *vx = (*it);
    if (!TypeFactory::isProcessType (vx->t)) {
      _check_add_spec (vx->getName(), vx->t, myI->obj);
    }
  }

  _cursi = (stateinfo_t *) stack_pop (_si_stack);
  stack_pop (_obj_stack);
}


void ActSimCore::computeFanout (ActInstTable *I)
{
  stateinfo_t *si = _cursi;
  if (I->obj) {
    _cursi = sp->getStateInfo (I->obj->getProc());
    I->obj->computeFanout();
  }
  if (I->H) {
    hash_bucket_t *b;
    hash_iter_t it;
    hash_iter_init (I->H, &it);
    while ((b = hash_iter_next (I->H, &it))) {
      computeFanout ((ActInstTable *)b->v);
    }
  }
  _cursi = si;
}

void ActSimCore::_initSim ()
{
  /* 
     We need to add:
      - top level languages, if any
      - all instances (recursively)
  */
  _curproc = simroot;
  _curinst = NULL;
  _cursuffix = NULL;
  _cursi = sp->getStateInfo (_curproc);
  _curoffset = sp->getGlobals();
  _curI = &I;

  /*
    Allocate top-level ports
  */
  act_boolean_netlist_t *bnl = bp->getBNL (_curproc);

  int ports_exist = 0;
  int chpports_exist_int = 0;
  int chpports_exist_bool = 0;
  int chpports_exist_chan = 0;

  for (int i=0; i < A_LEN (bnl->ports); i++) {
    if (bnl->ports[i].omit == 0) {
      ports_exist++;
    }
  }
  for (int i=0; i < A_LEN (bnl->chpports); i++) {
    if (bnl->chpports[i].omit == 0) {
      ihash_bucket_t *xb = ihash_lookup (bnl->cH, (long)bnl->chpports[i].c);
      act_booleanized_var_t *v;
      Assert (xb, "What?");
      v = (act_booleanized_var_t *)xb->v;
      if (v->ischan) {
	chpports_exist_chan++;
      }
      else if (v->isint) {
	chpports_exist_int++;
      }
      else {
	chpports_exist_bool++;
      }
    }
  }

#if 0
  Assert (chpports_exist_bool >= ports_exist, "What?");
  chpports_exist_bool -= ports_exist;
  printf ("ports_exist = %d\n", ports_exist);
  printf ("chpports_exist_chan = %d\n", chpports_exist_chan);
  printf ("chpports_exist_int = %d\n", chpports_exist_int);
  printf ("chpports_exist_bool = %d\n", chpports_exist_bool);
#endif

  /* compute port bool, int and chan ports */
  _cur_abs_port_bool = NULL;
  _cur_abs_port_int = NULL;
  _cur_abs_port_chan = NULL;

  if (chpports_exist_bool || chpports_exist_int || chpports_exist_chan ||
      ports_exist) {
    if (chpports_exist_chan) {
      MALLOC (_cur_abs_port_chan, int, chpports_exist_chan);
    }
    if (chpports_exist_int) {
      MALLOC (_cur_abs_port_int, int, chpports_exist_int);
    }
    if (chpports_exist_bool || ports_exist) {
      MALLOC (_cur_abs_port_bool, int, chpports_exist_bool + ports_exist);
    }
  }

  int i;
  for (i=0; i < ports_exist + chpports_exist_bool; i++) {
    _cur_abs_port_bool[i] = i + _curoffset.numAllBools();
  }
  _curoffset.addBool (i);
  for (i=0; i < chpports_exist_int; i++) {
    _cur_abs_port_int[i] = i + _curoffset.numInts();
  }
  _curoffset.addInt (i);
  for (i=0; i < chpports_exist_chan; i++) {
    _cur_abs_port_chan[i] = i + _curoffset.numChans();
  }
  _curoffset.addChan (i);

  if (_getlevel() == ACT_MODEL_DEVICE) {
    warning ("Modeling the entire design at device level is unsupported; use a circuit simulator instead!");
  }

  _si_stack = list_new ();
  _obj_stack = list_new ();

  _add_language (_getlevel(), root_lang);
  _add_all_inst (root_scope);

  list_free (_si_stack);
  list_free (_obj_stack);

  /*
    Now compute all the fanout dependencies
  */
  computeFanout(&I);

  /* 
     Add the initialization environment, if needed:

     1. For any top-level dangling channels, add the channel environment
     process.

     2. Add any top-level initialize block from the global namespace
  */

  

  /* at this point, all the fanout tables have been updated */
#if 0
  {
    int i;
    printf ("*-- bools --\n");
    for (i=0; i < nint_start; i++) {
      printf ("b[%d] : fo=%d", i, nfo[i]);
      if (nfo[i] > 0) {
	for (int j=0; j < nfo[i]; j++) {
	  printf (" ");
	  dynamic_cast<ActSimObj *>(fo[i][j])->getName()->Print (stdout);
	}
      }
      printf ("\n");
    }
    printf ("*-- ints --\n");
    for (; i < nfo_len; i++) {
      printf ("i[%d] : fo=%d", i-nint_start, nfo[i]);
      if (nfo[i] > 0) {
	for (int j=0; j < nfo[i]; j++) {
	  printf (" ");
	  dynamic_cast<ActSimObj *>(fo[i][j])->getName()->Print (stdout);
	}
      }
      printf ("\n");
    }
  }
#endif
}


act_connection *ActSim::runSim (act_connection **cause)
{
  Event *ret;

  if (SimDES::isEmpty()) {
    warning ("Empty simulation!");
    return NULL;
  }

  ret = SimDES::Run ();
  
  return NULL;
}

act_connection *ActSim::Step (long nsteps)
{
  Event *ret;
  if (SimDES::isEmpty()) {
    warning ("Empty simulation!");
    return NULL;
  }

  ret = SimDES::Advance (nsteps);

  return NULL;
}

act_connection *ActSim::Advance (long delay)
{
  Event *ret;
  if (SimDES::isEmpty()) {
    warning ("Empty simulation!");
    return NULL;
  }

  ret = SimDES::AdvanceTime (delay);

  return NULL;
}

void ActSimCore::_register_prssim_with_excl (ActInstTable *t)
{
  if (t->obj && dynamic_cast <PrsSim *> (t->obj)) {
    ((PrsSim *)t->obj)->registerExcl ();
  }
  if (t->H) {
    hash_bucket_t *b;
    hash_iter_t it;
    hash_iter_init (t->H, &it);
    while ((b = hash_iter_next (t->H, &it))) {
      _register_prssim_with_excl ((ActInstTable *)b->v);
    }
  }
}

ActSim::ActSim (Process *root) : ActSimCore (root)
{
  /* nothing */
  _init_simobjs = NULL;
  if (config_exists ("sim.device.timescale")) {
    _int_to_float_timescale = config_get_real ("sim.device.timescale");
  }
  else {
    /* 10ps default */
    _int_to_float_timescale = 10e-12;
  }
}

ActSim::~ActSim()
{
  /* stuff here */
  if (_init_simobjs) {
    listitem_t *li;
    for (li = list_first (_init_simobjs); li; li = list_next (li)) {
      ChpSim *x = (ChpSim *) list_value (li);
      li = list_next (li);
      ChpSimGraph *g = (ChpSimGraph *) list_value (li);
      delete g;
      delete x;
    }
  }
  list_free (_init_simobjs);
}

int ActSimCore::getLocalOffset (act_connection *c, stateinfo_t *si, int *type,
				int *width)
{
  int res;
  int offset;

  res = sp->getTypeOffset (si, c, &offset, type, width);
  if (res) {
    return offset;
  }
  else {
    ActId *t = c->toid();
    fprintf (stderr, "Identifier `");
    t->Print (stderr);
    fprintf (stderr, "' used, but not used in the design hierarchy selected.\n");
    fatal_error ("Error in initializer block?");
  }
  return 0;
}

int ActSimCore::hasLocalOffset (ActId *id, stateinfo_t *si)
{
  act_connection *c = id->Canonical (si->bnl->cur);
  return hasLocalOffset (c, si);
}

int ActSimCore::hasLocalOffset (act_connection *c, stateinfo_t *si)
{
  int res;
  int offset, type;

  res = sp->getTypeOffset (si, c, &offset, &type, NULL);

  if (res) {
    return 1;
  }
  else {
    return 0;
  }
}


int ActSimCore::getLocalDynamicStructOffset (act_connection *c,
					     stateinfo_t *si,
					     int *offset_i, int *offset_b)
{
  int res;

  if (!sp->connExists (si, c)) {
    ActId *t = c->toid();
    fprintf (stderr, "Identifier `");
    t->Print (stderr);
    fprintf (stderr, "' used, but not used in the design hierarchy selected.\n");
    fatal_error ("Error in initializer block?");
  }

  res = sp->getTypeDynamicStructOffset (si, c, offset_i, offset_b);
  if (res) {
    return 1;
  }
  else {
    fatal_error ("getLocalOffset() failed!");
  }
  return 0;
}

act_connection *ActSimCore::getConnFromOffset (Process *p, int off, int type,
					       int *dy)
{
  return sp->getConnFromOffset (p, off, type, dy);
}


int ActSimCore::getLocalOffset (ActId *id, stateinfo_t *si, int *type, int *width)
{
  act_connection *c;
  Scope *sc = si->bnl->cur;

  c = id->Canonical (sc);
  return getLocalOffset (c, si, type, width);
}

ActSimObj::ActSimObj (ActSimCore *sim, Process *p)
{
  _sc = sim;
  _proc = p;
  _abs_port_bool = NULL;
  _abs_port_int = NULL;
  _abs_port_chan = NULL;
  name = NULL;
  _shared = new WaitForOne(0);
}


int ActSimObj::getGlobalOffset (int loc, int type)
{
  int locoff;
  int *portoff;

#if 0
  printf ("[get glob: %d, type=%d] ", loc, type);
#endif  
  
  switch (type) {
  case 0:
    locoff = _o.numAllBools();
    portoff = _abs_port_bool;
    break;
  case 1:
    locoff = _o.numInts();
    portoff = _abs_port_int;
    break;
  case 2:
    locoff = _o.numChans();
    portoff = _abs_port_chan;
    break;
  default:
    fatal_error ("Unknown type: has to be 0, 1, or 2");
    break;
  }

  if (loc >= 0) {
#if 0
    printf (" -> %d var\n", locoff + loc);
#endif
    return locoff + loc;
  }
  else {
    loc = -loc;
    if (loc & 1) {
      /* local port */
      loc = (loc + 1)/2 - 1;
#if 0
      printf (" -> %d localport @ %d\n", portoff[loc], loc);
#endif
      return portoff[loc];
    }
    else {
      /* global */
      loc = loc/2 - 1;
#if 0
      printf (" -> %d global\n", loc);
#endif
      return loc;
    }
  }
}


void ActSimCore::incFanout (int off, int type, SimDES *who)
{
  if (type == 0) {
    /* bool */
    Assert (off >=0 && off < nint_start, "What?");
  }
  else {
    Assert (type == 1, "What?");
    Assert (off >= 0 && off < nfo_len - nint_start, "What?");
    off = off + nint_start;
  }
  
  if (nfo[off] == 0) {
    /* first entry! */
    NEW (fo[off], SimDES *);
  }
  else if (nfo[off] >= 8) {
    for (int i=nfo[off]-1; i >= 0 && i >= nfo[off]-10; i--) {
      if (fo[off][i] == who) {
	return;
      }
    }
    
    /* this is a tentative high fanout net */
    ihash_bucket_t *b;
    if (!hfo) {
      hfo = ihash_new (4);
    }
    b = ihash_lookup (hfo, off);
    if (!b) {
      b = ihash_add (hfo, off);
      b->i = nfo[off]; // the allocated slots
    }
    if (nfo[off] == b->i) {
      b->i = b->i*2;
      REALLOC (fo[off], SimDES *, b->i);
    }
  }
  else {
    for (int i=nfo[off]-1; i >= 0 ; i--) {
      if (fo[off][i] == who) {
	return;
      }
    }
    REALLOC (fo[off], SimDES *, nfo[off]+1);
  }
  fo[off][nfo[off]] = who;
  nfo[off]++;
}

void ActSimObj::propagate ()
{
  /* by default, wake me up if stalled on something shared */
  sWakeup ();
}


ActSimObj::~ActSimObj()
{
  if (_abs_port_bool) {
    FREE (_abs_port_bool);
  }
  if (_abs_port_int) {
    FREE (_abs_port_int);
  }
  if (_abs_port_chan) {
    FREE (_abs_port_chan);
  }
  delete name;
  delete _shared;
}


void ActSimObj::addWatchPoint (int type, int offset, const char *name)
{
  if (type == 3) {
    type = 2;
  }

  int glob = getGlobalOffset (offset, type);
  _sc->addWatchPt (type, glob, name);
}

void ActSimObj::toggleBreakPt (int type, int offset, const char *name)
{
  if (type == 3) {
    type = 2;
  }

  int glob = getGlobalOffset (offset, type);
  _sc->toggleBreakPt (type, glob, name);
}


void ActSimObj::delWatchPoint (int type, int offset)
{
  if (type == 3) {
    type = 2;
  }

  int glob = getGlobalOffset (offset, type);
  _sc->delWatchPt (type, glob);
}

void ActSimObj::msgPrefix (FILE *fp)
{
  if (fp == NULL) {
    fp = stdout;
  }
  BigInt tm = CurTime();
  fprintf (fp, "[");
  tm.decPrint (fp, 20);
  fprintf (fp, "] <");
  if (name) {
    name->Print (fp);
  }
  fprintf (fp, ">  ");
}

bool _match_hseprs (Event *e)
{
  if (dynamic_cast <OnePrsSim *> (e->getObj())) {
    return true;
  }
  ChpSim *x = dynamic_cast <ChpSim *> (e->getObj());
  if (x && x->isHseMode()) {
    return true;
  }
  return false;
}

void ActSim::runInit ()
{
  ActNamespace *g = ActNamespace::Global();
  act_initialize *x;
  int fragmented_set = 0;
  listitem_t *li;

  setMode (1);

  XyceActInterface::getXyceInterface()->initXyce();

  /*-- random init --*/
  for (int i=0; i < A_LEN (_rand_init); i++) {
    int v = random() % 2;
    if (getBool (_rand_init[i]) == 2) {
      if (setBool (_rand_init[i], v)) {
	SimDES **arr = getFO (_rand_init[i], 0);
	for (int j=0; j < numFanout (_rand_init[i], 0); j++) {
	  ActSimDES *x = dynamic_cast<ActSimDES *>(arr[j]);
	  Assert (x, "What?");
	  x->propagate ();
	}
      }
    }
  }
  
  /*-- reset channels that are fragmented --*/
  for (int i=0; i < state->numChans(); i++) {
    act_channel_state *ch = state->getChan (i);
    if ((ch->fragmented & 0x1) != (ch->fragmented >> 1)) {
      if (ch->fragmented & 0x1) {
	/* input fragmented, so do sender reset protocol */
	if (ch->cm->runMethod (this, ch, ACT_METHOD_SEND_INIT, 0) != -1) {
	  warning ("Failed to initialize fragmented channel!");
	  fprintf (stderr, "   Type: %s; inst: `", ch->ct->getName());
	  ch->inst_id->Print (stderr);
	  fprintf (stderr, "'\n");
	}
	fragmented_set = 1;
      }
      else {
	/* output fragmented, so do receiver fragmented protocol */
	if (ch->cm->runMethod (this, ch, ACT_METHOD_RECV_INIT, 0) != -1) {
	  warning ("Failed to initialize fragmented channel!");
	  fprintf (stderr, "   Type: %s; inst: `", ch->ct->getName());
	  ch->inst_id->Print (stderr);
	  fprintf (stderr, "'\n");
	}
	fragmented_set = 1;
      }
    }
  }

  /* -- initialize blocks -- */
  if (!g->getlang() || !g->getlang()->getinit()) {
    if (fragmented_set) {
      int count = 0;
      while (SimDES::matchPendingEvent (_match_hseprs) && count < 100) {
	count++;
	if (SimDES::AdvanceTime (10) != NULL) {
	  warning ("breakpoint?");
	}
      }
      if (count == 100) {
	warning ("Pending production rule events during reset phase?");
      }
    }
    setMode (0);
    for (li = list_first (_chp_sim_objects); li; li = list_next (li)) {
      ChpSim *cx = (ChpSim *) list_value (li);
      cx->sWakeup ();
    }
    return;
  }
  int num = 1;
  x = g->getlang()->getinit();

  while (x->next) {
    num++;
    x = x->next;
  }

  listitem_t **lia;
  MALLOC (lia, listitem_t *, num);

  x = g->getlang()->getinit();
  for (int i=0; i < num; i++) {
    Assert (x, "What?");
    lia[i] = list_first (x->actions);
    x = x->next;
  }
  Assert (!x, "What?");

  /* -- everything except the last action -- */
  int more_steps = 1;

  if (!_init_simobjs) {
    _init_simobjs = list_new ();
  }

  while (more_steps) {
    more_steps = 0;
    for (int i=0; i < num; i++) {
      if (lia[i] && list_next (lia[i])) {
	act_chp_lang_t *c = (act_chp_lang_t *) list_value (lia[i]);
	chpsimgraph_info *ci = ChpSimGraph::buildChpSimGraph (this, c);
	ChpSim *sim_init =
	  new ChpSim (ci, c, this, NULL);

	list_append (_init_simobjs, sim_init);
	list_append (_init_simobjs, ci->g);
	
	delete ci;
	  
	lia[i] = list_next (lia[i]);
	
	more_steps = 1;

	int count = 0;
	do {
	  if (SimDES::AdvanceTime (100) != NULL) {
	    warning ("breakpoint?");
	  }
	  count++;
	  if (!SimDES::matchPendingEvent (_match_hseprs)) {
	    break;
	  }
	} while (count < 100);
	if (count == 100) {
	  warning ("Pending production rule events during reset phase?");
	}
      }
    }
  }
  for (int i=0; i < num; i++) {
    if (lia[i]) {
      act_chp_lang_t *c = (act_chp_lang_t *) list_value (lia[i]);
      chpsimgraph_info *ci = ChpSimGraph::buildChpSimGraph (this, c);
      
      ChpSim *sim_init =
	new ChpSim (ci, c, this, NULL);

      list_append (_init_simobjs, sim_init);
      list_append (_init_simobjs, ci->g);
      
      delete ci;
	  
      lia[i] = list_next (lia[i]);
    }
  }
  FREE (lia);
  setMode (0);
  for (li = list_first (_chp_sim_objects); li; li = list_next (li)) {
    ChpSim *cx = (ChpSim *) list_value (li);
    cx->sWakeup ();
  }
}


void ActSimCore::logFilter (const char *s)
{
  if (s[0] == '\0') {
    _have_filter = 0;
  }
  else {
    _have_filter = 1;
    if (regcomp (&match, s, REG_EXTENDED) != 0) {
      _have_filter = 0;
      warning ("Regular expression `%s' didn't compile; skipped.", s);
    }
  }
}

int ActSimCore::isFiltered (const char *s)
{
  if (!_have_filter) {
    return 1;
  }
  else {
    if (regexec (&match, s, 0, NULL, 0) == 0) {
      return 1;
    }
    else {
      return 0;
    }
  }
}


void ActSimCore::registerFragmented (Channel *c)
{
  phash_bucket_t *b;
  Assert (c, "Hmm");
  if (!chan) {
    chan = phash_new (4);
  }
  b = phash_lookup (chan, c);
  if (b) {
    return;
  }
  b = phash_add (chan, c);
  b->v = new ChanMethods (c);
}

ChanMethods *ActSimCore::getFragmented (Channel *c)
{
  phash_bucket_t *b;
  Assert (c, "Hmm");
  if (!chan) {
    chan = phash_new (4);
  }
  b = phash_lookup (chan, c);
  if (b) {
    return (ChanMethods *)b->v;
  }
  return NULL;
}


/*
  root, a, b are all global state offsets! 
*/
void ActSimCore::_add_timing_fork (ActSimObj *obj, stateinfo_t *si,
				   int root, int a, int b, Expr *e, int *extra)
{
  ActTimingConstraint *tc;
  int rg, ag, bg;
  Assert (!e || e->type == E_INT, "Internal error on timing forks");

  rg = obj->getGlobalOffset (root, 0);
  ag = obj->getGlobalOffset (a, 0);
  bg = obj->getGlobalOffset (b, 0);
  
  tc = new ActTimingConstraint (obj, rg, ag, bg, e ? e->u.ival.v : 0, extra);

  if (tc->isDup()) {
    delete tc;
    return;
  }

  int dy;
  act_connection *c = sp->getConnFromOffset (si, root, 0, &dy);
  
  tc->setConn (0, c);
  c = sp->getConnFromOffset (si, a, 0, &dy);
  tc->setConn (1, c);
  c = sp->getConnFromOffset (si, b, 0, &dy);
  tc->setConn (2, c);
  
  state->mkSpecialBool (rg);
  state->mkSpecialBool (ag);
  state->mkSpecialBool (bg);
  return;
}

/*
  ids is a list of global state ids 
*/
void ActSimCore::_add_excl (int type, int *ids, int sz)
{
  ActExclConstraint *ec = new ActExclConstraint (ids, sz, type);
  if (ec->illegal()) {
    delete ec;
  }
  for (int i=0; i < sz; i++) {
    state->mkSpecialBool (ids[i]);
  }
  return;
}

/*
  ids for rand init
*/
void ActSimCore::_add_rand_init (int *ids, int sz)
{
  for (int i=0; i < sz; i++) {
    A_NEW (_rand_init, int);
    A_NEXT (_rand_init) = ids[i];
    A_INC (_rand_init);
  }
}

void ActSimCore::_add_hazard (int *ids, int sz)
{
  for (int i=0; i < sz; i++) {
    state->mkHazard (ids[i]);
  }
}


/*-------------------------------------------------------------------------
 * Logging
 *-----------------------------------------------------------------------*/
static FILE *alog_fp = NULL;

FILE *actsim_log_fp (void)
{
  return alog_fp ? alog_fp : stdout;
}

void actsim_log (const char *s, ...)
{
  va_list ap;

  va_start (ap, s);
  vfprintf (actsim_log_fp(), s, ap);
  va_end (ap);
  if (alog_fp) {
    fflush (alog_fp);
  }
}

void actsim_close_log (void)
{
  if (alog_fp) {
    fclose (alog_fp);
  }
  alog_fp = NULL;
}

void actsim_set_log (FILE *fp)
{
  actsim_close_log ();
  alog_fp = fp;
}

void actsim_log_flush (void)
{
  fflush (actsim_log_fp ());
}

/*--- Excl Constraints -- */

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



/*--- Timing Constraints -- */

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

void ActSimCore::recordTrace (const watchpt_bucket *w, int type, 
			      act_chan_state_t state, const BigInt &val)
{
  if (w->ignore_fmt == ~0U) {
    return;
  }
  
  float cur_time = curTimeMetricUnits();

  BigInt tm = SimDES::CurTime();
  int len = tm.getLen();
  unsigned long v;
  unsigned long *ptm;
  if (len == 1) {
    v = tm.getVal (0);
    ptm = &v;
  }
  else {
    MALLOC (ptm, unsigned long, len);
    for (int i=0; i < len; i++) {
      ptm[i] = tm.getVal (i);
    }
  }
  
  if (type == 0) {
    int v = val.getVal (0);
    if (v == 0) {
      v = ACT_SIG_BOOL_FALSE;
    }
    else if (v == 1) {
      v = ACT_SIG_BOOL_TRUE;
    }
    else if (v == 2) {
      v = ACT_SIG_BOOL_X;
    }
    for (int fmt=0; fmt < TRACE_NUM_FORMATS; fmt++) {
      if (!((w->ignore_fmt >> fmt) & 1)) {
	if (act_trace_has_alt (_trfn[fmt])) {
	  act_trace_digital_change_alt (_tr[fmt], w->node[fmt],
					len, ptm, v);
	}
	else {
	  act_trace_digital_change (_tr[fmt], w->node[fmt], cur_time, v);
	}
      }
    }
  }
  else if (type == 1) {
    if (ACT_TRACE_WIDE_NUM (val.getWidth()) <= 1) {
      for (int fmt=0; fmt < TRACE_NUM_FORMATS; fmt++) {
	if (!((w->ignore_fmt >> fmt) & 1)) {
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_digital_change_alt (_tr[fmt], w->node[fmt], len, ptm,
					  val.getVal (0));
	  }
	  else {
	    act_trace_digital_change (_tr[fmt], w->node[fmt], cur_time,
				      val.getVal (0));
	  }
	}
      }
    }
    else {
      unsigned long *valp;
      MALLOC (valp, unsigned long, val.getLen());
      for (int i=0; i < val.getLen(); i++) {
	valp[i] = val.getVal (i);
      }
      for (int fmt=0; fmt < TRACE_NUM_FORMATS; fmt++) {
	if (!((w->ignore_fmt >> fmt) & 1)) {
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_wide_digital_change_alt (_tr[fmt], w->node[fmt], len, ptm,
					   val.getLen(), valp);
	  }
	  else {
	    act_trace_wide_digital_change (_tr[fmt], w->node[fmt], cur_time,
					   val.getLen(), valp);
	  }
	}
      }
      FREE (valp);
    }
  }
  else if (type == 2) {
    if (ACT_TRACE_WIDE_NUM (val.getWidth()) > 1) {
      unsigned long *valp;
      MALLOC (valp, unsigned long, ACT_TRACE_WIDE_NUM (val.getWidth()));
      for (int i=0; i < ACT_TRACE_WIDE_NUM(val.getWidth()); i++) {
	if (i < val.getLen()) {
	  valp[i] = val.getVal (i);
	}
	else {
	  valp[i] = 0;
	}
      }
      for (int fmt = 0; fmt < TRACE_NUM_FORMATS; fmt++) {
	if (!((w->ignore_fmt >> fmt) & 1)) {
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_wide_chan_change_alt (_tr[fmt], w->node[fmt], len, ptm,
					    state,
					    ACT_TRACE_WIDE_NUM (val.getWidth()),
					    valp);
	  }
	  else {
	    act_trace_wide_chan_change (_tr[fmt], w->node[fmt], cur_time,
					state,
					ACT_TRACE_WIDE_NUM (val.getWidth()),
					valp);
	  }
	}
      }
      FREE (valp);
    }
    else {
      for (int fmt = 0; fmt < TRACE_NUM_FORMATS; fmt++) {
	if (!((w->ignore_fmt >> fmt) & 1)) {
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_chan_change_alt (_tr[fmt], w->node[fmt], len, ptm, state,
				       val.getVal (0));
	  }
	  else {
	    act_trace_chan_change (_tr[fmt], w->node[fmt], cur_time, state,
				   val.getVal (0));
	  }
	}
      }
    }
  }
  if (len > 1) {
    FREE (ptm);
  }
}

int ActSimCore::initTrace (int fmt, const char *file)
{
  double cur_time = curTimeMetricUnits ();
  double tm = cur_time + 1;

  Assert (0 <= fmt && fmt < TRACE_NUM_FORMATS, "Illegal format!");

  if (_tr[fmt]) {
    act_trace_close (_tr[fmt]);
  }
  if (!file) {
    _tr[fmt] = NULL;

    if (_W) {
      ihash_bucket_t *b;
      watchpt_bucket *w;
      ihash_iter_t it;

      ihash_iter_init (_W, &it);
      while ((b = ihash_iter_next (_W, &it))) {
	w = (watchpt_bucket *) b->v;
	w->ignore_fmt |= (1 << fmt);
      }
    }
    return 1;
  }

  if (!_trfn[fmt]) {
    _trfn[fmt] = act_trace_load_format (_trname[fmt], NULL);
  }
  if (!_trfn[fmt]) {
    return 0;
  }

  _tr[fmt] = act_trace_create (_trfn[fmt], file, tm, _int_to_float_timescale,
			       act_trace_has_alt (_trfn[fmt]) ? 1 : 0);
  
  if (!_tr[fmt]) {
    return 0;
  }
  
  if (_W) {
    ihash_bucket_t *b;
    watchpt_bucket *w;
    ihash_iter_t it;

    ihash_iter_init (_W, &it);
    while ((b = ihash_iter_next (_W, &it))) {
      unsigned long off = b->key;
      int type = off & 0x3;
      off >>= 2;

      w = (watchpt_bucket *) b->v;
      w->ignore_fmt &= ~(1 << fmt);
      if (type == 0) {
	w->node[fmt] = act_trace_add_signal (_tr[fmt], ACT_SIG_BOOL, w->s, 0);
      }
      else if (type == 1) {
	BigInt *tmp = getInt (off);
	w->node[fmt] = act_trace_add_signal (_tr[fmt], ACT_SIG_INT, w->s,
					     tmp->getWidth());
      }
      else if (type == 2) {
	act_channel_state *ch = getChan (off);
	w->node[fmt] = act_trace_add_signal (_tr[fmt], ACT_SIG_CHAN, w->s,
					     ch->width);
      }
    }

    // now dump current  value
    BigInt tm = SimDES::CurTime();
    int tmlen = tm.getLen ();
    unsigned long tmpv;
    unsigned long *ptm;
    if (tmlen == 1) {
      tmpv = tm.getVal (0);
      ptm = &tmpv;
    }
    else {
      MALLOC (ptm, unsigned long, tmlen);
      for (int i=0; i < tmlen; i++) {
	ptm[i] = tm.getVal (i);
      }
    }
    
    act_trace_init_start (_tr[fmt]);
    ihash_iter_init (_W, &it);
    while ((b = ihash_iter_next (_W, &it))) {
      unsigned long off = b->key;
      int type = off & 0x3;
      off >>= 2;

      w = (watchpt_bucket *) b->v;
      if (type == 0) {
	int v = getBool (off);
	if (v == 0) {
	  v = ACT_SIG_BOOL_TRUE;
	}
	else if (v == 1) {
	  v = ACT_SIG_BOOL_FALSE;
	}
	else if (v == 2) {
	  v = ACT_SIG_BOOL_X;
	}
	if (act_trace_has_alt (_trfn[fmt])) {
	  act_trace_digital_change_alt (_tr[fmt], w->node[fmt], tmlen, ptm, v);
	}
	else {
	  act_trace_digital_change (_tr[fmt], w->node[fmt], cur_time, v);
	}
      }
      else if (type == 1) {
	BigInt *tmp = getInt (off);
	if (ACT_TRACE_WIDE_NUM (tmp->getWidth()) <= 1) {
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_digital_change_alt (_tr[fmt], w->node[fmt], tmlen, ptm,
				      tmp->getVal (0));
	  }
	  else {
	    act_trace_digital_change (_tr[fmt], w->node[fmt], cur_time,
				      tmp->getVal (0));
	  }
	}
	else {
	  unsigned long *v;
	  MALLOC (v, unsigned long, tmp->getLen());
	  for (int i=0; i < tmp->getLen(); i++) {
	    v[i] = tmp->getVal (i);
	  }
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_wide_digital_change_alt (_tr[fmt], w->node[fmt],
					       tmlen, ptm, 
					       tmp->getLen(), v);
	  }
	  else {
	    act_trace_wide_digital_change (_tr[fmt], w->node[fmt], cur_time,
					   tmp->getLen(), v);
	  }
	  FREE (v);
	}
      }
      else if (type == 2) {
	act_channel_state *ch = getChan (off);
	act_chan_state_t state;
	if (WAITING_SENDER (ch)) {
	  state = ACT_CHAN_SEND_BLOCKED;
	}
	else if (WAITING_RECEIVER(ch)) {
	  state = ACT_CHAN_RECV_BLOCKED;
	}
	else {
	  state = ACT_CHAN_IDLE;
	}
	if (ACT_TRACE_WIDE_NUM (ch->width) > 1) {
	  unsigned long *v;
	  MALLOC (v, unsigned long, ACT_TRACE_WIDE_NUM (ch->width));
	  for (int i=0; i < ACT_TRACE_WIDE_NUM(ch->width); i++) {
	    v[i] = 0;
	  }
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_wide_chan_change_alt (_tr[fmt], w->node[fmt],
					    tmlen, ptm, state,
					    ACT_TRACE_WIDE_NUM (ch->width), v);
	  }
	  else {
	    act_trace_wide_chan_change (_tr[fmt], w->node[fmt], cur_time,
					state, ACT_TRACE_WIDE_NUM (ch->width),
					v);
	  }
	  FREE (v);
	}
	else {
	  if (act_trace_has_alt (_trfn[fmt])) {
	    act_trace_chan_change_alt (_tr[fmt], w->node[fmt], tmlen, ptm,
				       state, 0);
	  }
	  else {
	    act_trace_chan_change (_tr[fmt], w->node[fmt], cur_time, state, 0);
	  }
	}
      }
    }
    if (tmlen > 1) {
      FREE (ptm);
    }
    act_trace_init_end (_tr[fmt]);
  }
  return 1;
}


void ActSimCore::checkFragmentation (ActId *id, ActSimObj *obj, stateinfo_t *si, int read_only)
{
  act_boolean_netlist_t *bn = si->bnl;
  act_connection *tmpc = id->Canonical (bn->cur);
  checkFragmentation (tmpc, id, obj, si, read_only);
}

void ActSimCore::checkFragmentation (act_connection *idc, ActId *rid, ActSimObj *obj, stateinfo_t *si, int read_only)
{
  act_boolean_netlist_t *bn = si->bnl;
  int is_global;

  _prop_used_flags (bn, idc);

  ActConniter it(idc);

#if 0
  printf ("[ %s ] check frag: ", si->bnl->p ? si->bnl->p->getName() : "-none-");
  idc->toid()->Print (stdout);
  printf ("\n");
#endif

  is_global = idc->isglobal();

  /*
     If the connection is a global, then we are actually walking
     through different scopes as we walk through the connection list
  */

  for (it = it.begin(); it != it.end(); it++) {
    act_connection *c = (*it);
    ActId *id;

    /*
       If the canonical name is a global, then only check global
       scopes for this particular degragmentation check. Other
       elements may not be in the current scope.
    */
    
    if (is_global && !c->isglobal()) {
      if (!rid) {
	continue;
      }
      id = c->toid();
      if (!rid->isEqual (id)) {
	delete id;
	continue;
      }
    }
    else {
      id = c->toid();
    }

    if (id->isFragmented (si->bnl->cur)) {
      ActId *tmp = id->unFragment (si->bnl->cur);
      /*--  tmp is the unfragmented identifier --*/

      int type;

      if (hasLocalOffset (tmp, si)) {
	int loff = getLocalOffset (tmp, si, &type);

#if 0
	printf ("[ %s ] Unfragment: ", si->bnl->p ? si->bnl->p->getName() : "-none-");
	tmp->Print (stdout);
	printf ("\n");
	printf ("  -> original: ");
	id->Print (stdout);
	printf (" (type = %d, read_only = %d)\n", type, read_only);
#endif

	if (type == 2 || type == 3) {
	  stateinfo_t *mysi = cursi();
	  setsi (si);
	  loff = obj->getGlobalOffset (loff, 2);

	  act_channel_state *ch = getChan (loff);

	  sim_recordChannel (this, obj, tmp);
	  registerFragmented (ch->ct);
	  ch->cm = getFragmented (ch->ct);
	  setsi (mysi);

	  ActId *xtmp = tmp;
	  ActId *tail = id;
	  while (tmp) {
	    tmp = tmp->Rest();
	    tail = tail->Rest();
	    Assert (tail, "What?");
	  }
	  tmp = xtmp;

	  InstType *chit = ch->ct->CurScope()->FullLookup (tail, NULL);
	  Assert (chit, "Channel didn't have pieces?");
	  if (chit->getDir() == Type::INOUT) {
	    if (read_only) {
	      ch->fragmented |= 1;
	    }
	    else {
	      ch->fragmented |= 2;
	    }
	  }
	  else if (chit->getDir() == Type::OUTIN) {
	    if (read_only) {
	      ch->fragmented |= 2;
	    }
	    else {
	      ch->fragmented |= 1;
	    }
	  }
	  else {
	    /* guessing channel fragmentation */
	    if (type == 2) {
	      /* input */
	      ch->fragmented |= 1;
	    }
	    else {
	      /* output: reading a fragment */
	      ch->fragmented |= 2;
	    }
	  }
	}
      }
      delete tmp;
    }
    delete id;
  }
}


/* pending events */

static int _pend_count;
static int _pend_prs;

static bool _match_count (Event *e)
{
  _pend_count++;
  if (dynamic_cast <OnePrsSim *> (e->getObj())) {
    _pend_prs++;
  }
  return false;
}

static bool _match_verbose (Event *e)
{
  OnePrsSim *op = dynamic_cast<OnePrsSim *> (e->getObj());
  if (op) {
    return false;
  }
  ChpSim *ch = dynamic_cast<ChpSim *> (e->getObj());
  if (ch) {
    return false;
  }
  return false;
}

void runPending (bool verbose)
{
  _pend_count = 0;
  _pend_prs = 0;
  SimDES::matchPendingEvent (_match_count);
  printf ("Pending events: %d", _pend_count);
  if (_pend_prs > 0) {
    printf ("; prs events: %d", _pend_prs);
  }
  printf ("\n");
#if 0  
  if (verbose) {
    SimDES::matchPendingEvent (_match_verbose);
  }
#endif  
}
