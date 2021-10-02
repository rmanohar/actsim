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

struct chpsimgraph_info {
  ChpSimGraph *g;
  Expr *e;			/* for probes, if needed */
  int max_count;
};

ActSimCore::ActSimCore (Process *p)
{
  _have_filter = 0;
  
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
  pmap = ihash_new (8);
  chan = NULL;
  I.H = NULL;
  I.obj = NULL;

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
  
  _rootsi = si;
  _initSim();
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
  
  for (int i=0; i < A_LEN (_rootsi->bnl->used_globals); i++) {
    act_booleanized_var_t *v;
    ihash_bucket_t *b;
    b = ihash_lookup (_rootsi->bnl->cH, (long)_rootsi->bnl->used_globals[i]);
    Assert (b, "WHat?");
    v = (act_booleanized_var_t *) b->v;
    Assert (v, "What?");
    Assert (v->isglobal, "What?");
  }

  /* free chp */
  if (map) {
    for (int i=0; i < map->size; i++) {
      for (ihash_bucket_t *b = map->head[i]; b; b = b->next) {
	struct chpsimgraph_info *sg = (struct chpsimgraph_info *)b->v;
	delete sg->g;
	FREE (sg);
      }
    }
    ihash_free (map);
  }

  /* free prs */
  if (pmap) {
    ihash_iter_t iter;
    ihash_bucket_t *b;
    ihash_iter_init (pmap, &iter);
    while ((b = ihash_iter_next (pmap, &iter))) {
      PrsSimGraph *g = (PrsSimGraph *)b->v;
      delete g;
    }
    ihash_free (pmap);
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

  /*-- instance tables --*/
  _delete_sim_objs (&I, 0);
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

  chpsimgraph_info *sg = NULL;
  ChpSim *x;
  
  if (c) {
    b = ihash_lookup (map, (long)_curproc);
    if (b) {
      sg = (chpsimgraph_info *)b->v;
    }
    else {
      ChpSimGraph *stop;
      b = ihash_add (map, (long)_curproc);
      NEW (sg, chpsimgraph_info);
      sg->g = ChpSimGraph::buildChpSimGraph (this, c->c,  &stop);
      sg->e = NULL;
      sg->max_count = ChpSimGraph::max_pending_count;
      b->v = sg;
    }
    x = new ChpSim (sg->g, sg->max_count, c->c, this, _curproc);
  }
  else {
    x = new ChpSim (NULL, 0, NULL, this, _curproc);
  }
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);

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

  chpsimgraph_info *sg;
  b = ihash_lookup (map, (long)_curproc);
  if (b) {
    sg = (chpsimgraph_info *)b->v;
  }
  else {
    ChpSimGraph *stop;
    b = ihash_add (map, (long)_curproc);
    NEW (sg, chpsimgraph_info);
    sg->g = ChpSimGraph::buildChpSimGraph (this, c->c,  &stop);
    sg->max_count = ChpSimGraph::max_pending_count;
    b->v = sg;
  }
  ChpSim *x = new ChpSim (sg->g, sg->max_count, c->c, this, _curproc);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
  
  return x;
}

PrsSim *ActSimCore::_add_prs (act_prs *p, act_spec *spec)
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

  PrsSimGraph *pg;
  ihash_bucket_t *b;
  b = ihash_lookup (pmap, (long)_curproc);
  if (b) {
    pg = (PrsSimGraph *)b->v;
  }
  else {
    b = ihash_add (pmap, (long)_curproc);
    pg = PrsSimGraph::buildPrsSimGraph (this, p, spec);
    b->v = pg;
  }
  /* need prs simulation graph */

  PrsSim *x = new PrsSim (pg, this, _curproc);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);

  return x;
}


void ActSimCore::_check_fragmentation (ChpSim *c)
{
  ChpSimGraph::checkFragmentation (this, c, _curproc->getlang()->gethse()->c);
}

void ActSimCore::_check_fragmentation (PrsSim *p)
{
  PrsSimGraph::checkFragmentation (this, p, _curproc->getlang()->getprs());
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
    _check_fragmentation ((x = _add_prs (l->getprs(), l->getspec())));
    _curI->obj = x;
  }
  else if (lev == ACT_MODEL_DEVICE) {
    fatal_error ("Xyce needs to be integrated");
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
    _curI->obj->computeFanout();
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
  _curoffset.addVar (_cursi->local);

  ActInstiter it(sc);

  if (it.begin() != it.end()) {
    myI->H = hash_new (4);
  }
  
  for (it = it.begin(); it != it.end(); it++) {
    ValueIdx *vx = (*it);
    stateinfo_t *si;
    if (TypeFactory::isProcessType (vx->t)) {
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
	tmpid = _curinst;
	while (tmpid->Rest()) { tmpid = tmpid->Rest(); }
	tmpid->Append (new ActId (vx->getName()));
	previd = tmpid;
	tmpid = tmpid->Rest();
      }

      si = sp->getStateInfo (x);
      Assert (si, "What?");
      _cursi = si;

      do {
	_curproc = x;
	_cursi = si;
	
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
	if (ports_exist) {
	  for (int i=0; i < A_LEN (bnl->ports); i++) {
	    if (bnl->ports[i].omit) continue;
	    Assert (iportbool < A_LEN (mynl->instports), "What?");

	    act_connection *c = mynl->instports[iportbool];
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
	if (chpports_exist_int|| chpports_exist_bool || chpports_exist_chan) {
	  /* then we use the chp instports */
	  for (int i=0; i < A_LEN (bnl->chpports); i++) {
	    if (bnl->chpports[i].omit) continue;
	    Assert (iportchp < A_LEN (mynl->instchpports), "What?");

	    ihash_bucket_t *xb = ihash_lookup (bnl->cH, (long)bnl->chpports[i].c);
	    act_booleanized_var_t *v;
	    Assert (xb, "What?");
	    v = (act_booleanized_var_t *)xb->v;
	    act_connection *c = mynl->instchpports[iportchp];
	    iportchp++;
	    if (v->used) continue; /* already covered */

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
	    if (v->ischan) {
	      _cur_abs_port_chan[ichan++] = off;
	    }
	    else if (v->isint) {
	      _cur_abs_port_int[iint++] = off;
	    }
	    else {
	      _cur_abs_port_bool[ibool++] = off;
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
	_add_all_inst (x->CurScope());

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
	delete _curinst;
	_curinst = NULL;
      }
      if (as) {
	delete as;
      }
    }
  }
  Assert (iportbool == A_LEN (mynl->instports), "What?");
  Assert (iportchp == A_LEN (mynl->instchpports), "What?");
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

  _add_language (_getlevel(), root_lang);
  _add_all_inst (root_scope);

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

ActSim::ActSim (Process *root) : ActSimCore (root)
{
  /* nothing */
  _init_simobjs = NULL;
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

  if (!sp->connExists (si, c)) {
    ActId *t = c->toid();
    fprintf (stderr, "Identifier `");
    t->Print (stderr);
    fprintf (stderr, "' used, but not used in the design hierarchy selected.\n");
    fatal_error ("Error in initializer block?");
  }

  res = sp->getTypeOffset (si, c, &offset, type, width);
  if (res) {
    return offset;
  }
  else {
    fatal_error ("getLocalOffset() failed!");
  }
  return 0;
}

int ActSimCore::getLocalDynamicStructOffset (act_connection *c,
					     stateinfo_t *si,
					     int *offset_i, int *offset_b)
{
  int res;
  int offset;

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
  _W = NULL;
  _B = NULL;
  name = NULL;
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
  /* by default, just wake up all globally stalled processs */
  _sc->gWakeup ();
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
  if (_W) {
    ihash_free (_W);
  }
  delete name;
}


void ActSimObj::addWatchPoint (int type, int offset, const char *name)
{
  ihash_bucket_t *b;
  
  if (type == 3) {
    type = 2;
  }

  if (!_W) {
    _W = ihash_new (4);
  }
  b = ihash_lookup (_W, (unsigned long)type | ((unsigned long)offset << 2));
  if (!b) {
    b = ihash_add (_W, (unsigned long)type | ((unsigned long)offset << 2));
    b->v = Strdup (name);
  }
}

void ActSimObj::toggleBreakPt (int type, int offset, const char *name)
{
  ihash_bucket_t *b;
  
  if (type == 3) {
    type = 2;
  }

  if (!_B) {
    _B = ihash_new (4);
  }
  b = ihash_lookup (_B, (unsigned long)type | ((unsigned long)offset << 2));
  if (!b) {
    b = ihash_add (_B, (unsigned long)type | ((unsigned long)offset << 2));
    b->v = Strdup (name);
  }
  else {
    FREE (b->v);
    ihash_delete (_B, (unsigned long)type | ((unsigned long)offset << 2));
  }
}


void ActSimObj::delWatchPoint (int type, int offset)
{
  ihash_bucket_t *b;
  
  if (type == 3) {
    type = 2;
  }

  if (!_W) {
    return;
  }
  b = ihash_lookup (_W, (unsigned long)type | ((unsigned long)offset << 2));
  if (b) {
    ihash_delete (_W, (unsigned long)type | ((unsigned long)offset << 2));
    FREE (b->v);

    if (_W->n == 0) {
      ihash_free (_W);
      _W = NULL;
    }
  }
}

void ActSimObj::msgPrefix (FILE *fp)
{
  if (fp == NULL) {
    fp = stdout;
  }
  fprintf (fp, "[%20lu] <", CurTimeLo());
  name->Print (fp);
  fprintf (fp, ">  ");
}

void ActSim::runInit ()
{
  ActNamespace *g = ActNamespace::Global();
  act_initialize *x;

  setMode (1);
  
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
      }
      else {
	/* output fragmented, so do receiver fragmented protocol */
	if (ch->cm->runMethod (this, ch, ACT_METHOD_RECV_INIT, 0) != -1) {
	  warning ("Failed to initialize fragmented channel!");
	  fprintf (stderr, "   Type: %s; inst: `", ch->ct->getName());
	  ch->inst_id->Print (stderr);
	  fprintf (stderr, "'\n");
	}
      }
    }
  }

  /* -- initialize blocks -- */
  if (!g->getlang() || !g->getlang()->getinit()) {
    setMode (0);
    gWakeup ();
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
	ChpSimGraph *stop;
	ChpSimGraph *sg = ChpSimGraph::buildChpSimGraph (this, c, &stop);
	ChpSim *sim_init =
	  new ChpSim (sg, ChpSimGraph::max_pending_count, c, this, NULL);

	list_append (_init_simobjs, sim_init);
	list_append (_init_simobjs, sg);
	  
	lia[i] = list_next (lia[i]);
	
	more_steps = 1;
	
	if (SimDES::AdvanceTime (100) != NULL) {
	  warning ("More events?");
	}
      }
    }
  }
  for (int i=0; i < num; i++) {
    if (lia[i]) {
      act_chp_lang_t *c = (act_chp_lang_t *) list_value (lia[i]);
      ChpSimGraph *stop;
      ChpSimGraph *sg = ChpSimGraph::buildChpSimGraph (this, c, &stop);
      ChpSim *sim_init =
	new ChpSim (sg, ChpSimGraph::max_pending_count, c, this, NULL);

      list_append (_init_simobjs, sim_init);
      list_append (_init_simobjs, sg);
	  
      lia[i] = list_next (lia[i]);
    }
  }
  FREE (lia);
  setMode (0);
  gWakeup ();
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
