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


ActSimCore::ActSimCore (Process *p)
{
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


  act_boolean_netlist_t *bnl = si->bnl;
  Assert (bnl, "What are we doing here");

  globals.bools = 0;
  globals.ints = 0;
  globals.chans = 0;

  for (int i=0; i < A_LEN (bnl->used_globals); i++) {
    act_booleanized_var_t *v;
    ihash_bucket_t *b;
    b = ihash_lookup (bnl->cH, (long)bnl->used_globals[i]);
    Assert (b, "WHat?");
    v = (act_booleanized_var_t *) b->v;
    Assert (v, "What?");
    Assert (v->isglobal, "What?");
    if (v->isint) {
      globals.ints++;
    }
    else if (v->ischan) {
      globals.chans++;
    }
    else {
      globals.bools++;
    }
  }

  /* now put in negative indices so that you have the right mapping! */
  chp_offsets idx;
  idx.bools = 0;
  idx.ints = 0;
  idx.chans = 0;
  for (int i=0; i < A_LEN (bnl->used_globals); i++) {
    act_booleanized_var_t *v;
    ihash_bucket_t *b;
    b = ihash_lookup (bnl->cH, (long)bnl->used_globals[i]);
    Assert (b, "WHat?");
    v = (act_booleanized_var_t *) b->v;
    Assert (v, "What?");
    Assert (v->isglobal, "What?");

    if (v->used) {
      b = ihash_add (si->map, (long)bnl->used_globals[i]);
      b->i = idx.bools - globals.bools;
      idx.bools++;
    }
    else if (v->usedchp) {
      b = ihash_add (si->chpmap, (long)bnl->used_globals[i]);
      if (v->isint) {
	b->i = idx.ints - globals.ints;
	idx.ints++;
      }
      else if (v->ischan) {
	b->i = idx.chans - globals.chans;
	idx.chans++;
      }
      else {
	b->i = idx.bools - globals.bools;
	idx.bools++;
      }
    }
  }

  /* 
     Allocate state.
     We need:
     - all ports (all port bools, and chp ports)
     - all local vars including sub-instance local vars
  */
  state = new ActSimState (si->nportbools + si->allbools
			   + si->nportchp.bools +  si->chp_all.bools
			   + globals.bools,
			   si->nportchp.ints + si->chp_all.ints
			   + globals.ints,
			   si->nportchp.chans + si->chp_all.chans
			   + globals.chans);

  nfo_len = si->nportbools + si->allbools +
    si->nportchp.bools + si->chp_all.bools + globals.bools +
    si->nportchp.ints + si->chp_all.ints + globals.ints;
  nint_start = si->nportbools + si->allbools +
    si->nportchp.bools + si->chp_all.bools + globals.bools;
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
  
  _rootsi = si;
  _initSim();
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

    if (v->used) {
      ihash_delete (_rootsi->map, (long)_rootsi->bnl->used_globals[i]);
    }
    else if (v->usedchp) {
      ihash_delete (_rootsi->chpmap, (long)_rootsi->bnl->used_globals[i]);
    }
  }

  /* free chp */
  if (map) {
    for (int i=0; i < map->size; i++) {
      for (ihash_bucket_t *b = map->head[i]; b; b = b->next) {
	ChpSimGraph *g = (ChpSimGraph *)b->v;
	delete g;
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

  if (state) {
    delete state;
  }
}


void ActSimCore::_add_chp (act_chp *c)
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

  ChpSimGraph *sg;
  b = ihash_lookup (map, (long)_curproc);
  if (b) {
    sg = (ChpSimGraph *)b->v;
  }
  else {
    ChpSimGraph *stop;
    b = ihash_add (map, (long)_curproc);
    sg = ChpSimGraph::buildChpSimGraph (this, c->c,  &stop);
    b->v = sg;
  }
  ChpSim *x = new ChpSim (sg, _cursi, c->c, this);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
  x->computeFanout ();
}

void ActSimCore::_add_dflow (act_dataflow *d)
{

  printf ("add-dflow-inst: ");
  if (_curinst) {
    _curinst->Print (stdout);
  }
  else {
    printf ("-none-");
  }
  printf ("\n");

}


void ActSimCore::_add_hse (act_chp *c)
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

  ChpSimGraph *sg;
  b = ihash_lookup (map, (long)_curproc);
  if (b) {
    sg = (ChpSimGraph *)b->v;
  }
  else {
    ChpSimGraph *stop;
    b = ihash_add (map, (long)_curproc);
    sg = ChpSimGraph::buildChpSimGraph (this, c->c,  &stop);
    b->v = sg;
  }
  ChpSim *x = new ChpSim (sg, _cursi, c->c, this);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
  x->computeFanout ();
}

void ActSimCore::_add_prs (act_prs *p, act_spec *spec)
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

  PrsSim *x = new PrsSim (pg, this);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
  x->computeFanout ();
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
  if (!l) return;

  /*- we have the full path here -*/
  if ((l->getchp() || l->getdflow()) && lev == ACT_MODEL_CHP) {
    /* chp or dataflow */
    if (l->getchp()) {
      _add_chp (l->getchp());
    }
    else {
      _add_dflow (l->getdflow());
    }
  }
  else if (l->gethse() && lev == ACT_MODEL_HSE) {
    /* hse */
    _add_chp (l->gethse());
  }
  else if (l->getprs() && lev == ACT_MODEL_PRS) {
    /* prs */
    _add_prs (l->getprs(), l->getspec());
  }
  else if (lev == ACT_MODEL_DEVICE) {
    fatal_error ("Xyce needs to be integrated");
  }
  else {
    /* substitute a less detailed model, if possible */
    if (l->gethse() && lev == ACT_MODEL_PRS) {
      warning ("%s: substituting hse model (requested %s, not found)", _curproc ? _curproc->getName() : "-top-", act_model_names[lev]);
      _add_chp (l->gethse());
    }
    else if ((l->getdflow() || l->getchp()) &&
	     (lev == ACT_MODEL_PRS || lev == ACT_MODEL_HSE)) {
      if (l->getdflow()) {
	warning ("%s: substituting dataflow model (requested %s, not found)", _curproc ? _curproc->getName() : "-top-", act_model_names[lev]);
	_add_dflow (l->getdflow());
      }
      else {
	Assert (l->getchp(), "What?");
	warning ("%s: substituting chp model (requested %s, not found)", _curproc ? _curproc->getName() : "-top-", act_model_names[lev]);
	_add_chp (l->getchp());
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
  chp_offsets myoffset;

  Assert (sc->isExpanded(), "What?");

  iportbool = 0;
  iportchp = 0;
  mynl = bp->getBNL (_curproc);
  _my_port_int = _cur_abs_port_int;
  _my_port_bool = _cur_abs_port_bool;
  _my_port_chan = _cur_abs_port_chan;
  mysi = _cursi;
  myoffset = _curoffset;

  /* -- increment cur offset after allocating all the items -- */
  _curoffset.bools += _cursi->localbools + _cursi->chp_local.bools;
  _curoffset.ints += _cursi->chp_local.ints;
  _curoffset.chans += _cursi->chp_local.chans;

  ActInstiter it(sc);
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
	if (as) {
	  tmpid->setArray (as->toArray());
	}

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
	      
	    if (off < 0) {
	      /* port or global */
	      off = -off;
	      if ((off & 1) == 0) {
		/* global */
		off = off/2 - 1;
	      }
	      else {
		off = (off + 1)/2 - 1;
		off = _my_port_bool[off];
	      }
	    }
	    else {
	      /* local state */
	      off += myoffset.bools;
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
	      
	    if (off < 0) {
	      /* port or global */
	      off = -off;
	      if ((off & 1) == 0) {
		/* global */
		off = off/2 - 1;
	      }
	      else {
		off = (off + 1)/2 - 1;
		if (type == 2) {
		  off = _my_port_chan[off];
		}
		else if (type == 1) {
		  off = _my_port_int[off];
		}
		else {
		  off = _my_port_bool[off];
		}
	      }
	    }
	    else {
	      /* local state */
	      if (type == 2) {
		off += myoffset.chans;
	      }
	      else if (type == 1) {
		off += myoffset.ints;
	      }
	      else {
		off += myoffset.bools;
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
  _curoffset = globals;
  _cursi = sp->getStateInfo (_curproc);

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
    _cur_abs_port_bool[i] = i + _curoffset.bools;
  }
  _curoffset.bools += i;
  for (i=0; i < chpports_exist_int; i++) {
    _cur_abs_port_int[i] = i + _curoffset.ints;
  }
  _curoffset.ints += i;
  for (i=0; i < chpports_exist_chan; i++) {
    _cur_abs_port_chan[i] = i + _curoffset.chans;
  }
  _curoffset.chans += i;
  
  _add_language (_getlevel(), root_lang);
  _add_all_inst (root_scope);

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




ActSim::ActSim (Process *root) : ActSimCore (root)
{
  /* nothing */
}

ActSim::~ActSim()
{
  /* stuff here */
}


int ActSimCore::mapIdToLocalOffset (act_connection *c, stateinfo_t *si)
{
  ihash_bucket_t *b;
  int glob;

  if (c->isglobal())  {
    glob = 1;
    si = _rootsi;
  }
  else {
    glob = 0;
  }

  b = ihash_lookup (si->map, (long)c);
  if (!b) {
    b = ihash_lookup (si->chpmap, (long)c);
  }
  if (!b) {
    fatal_error ("Could not map ID to local offset?");
  }
  if (glob) {
    Assert (b->i < 0, "What?");
    return 2*b->i;
  }
  else {
    if (b->i < 0) {
      return 2*b->i + 1;
    }
    else {
      return b->i;
    }
  }
}

int ActSimCore::getLocalOffset (act_connection *c, stateinfo_t *si, int *type)
{
  act_booleanized_var_t *v;
  ihash_bucket_t *b;
  int x;

  if ((b = ihash_lookup (si->bnl->cdH, (long)c))) {
    act_dynamic_var_t *dv = (act_dynamic_var_t *)b->v;
    if (dv->isint) {
      b = ihash_lookup (si->chpmap, (long)c);
      Assert (b, "What?");
      if (type) {
	*type = 1;
      }
    }
    else {
      b = ihash_lookup (si->map, (long)c);
      if (type) {
	*type = 0;
      }
    }
    return b->i;
  }

  b = ihash_lookup (si->bnl->cH, (long)c);
  Assert (b, "What?");

  v = (act_booleanized_var_t *)b->v;
  x = mapIdToLocalOffset (c, si);

  if (type) {
    if (v->ischan) {
      if (v->input) {
	*type = 2;
      }
      else {
	*type = 3;
      }
    }
    else if (v->isint) {
      *type = 1;
    }
    else {
      *type = 0;
    }
  }

  if (v->isport || v->ischpport) {
    /* this is a port... */
#if 0    
    printf ("port-'");
#endif    
  }
#if 0  
  printf ("Id: ");
  c->toid()->Print (stdout);
  printf ("; loc-offset: %d\n", x);
#endif  
  return x;
}  


int ActSimCore::getLocalOffset (ActId *id, stateinfo_t *si, int *type)
{
  act_connection *c;
  Scope *sc = si->bnl->cur;

  

  c = id->Canonical (sc);
  return getLocalOffset (c, si, type);
}

ActSimObj::ActSimObj (ActSimCore *sim)
{
  _sc = sim;
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
    locoff = _o.bools;
    portoff = _abs_port_bool;
    break;
  case 1:
    locoff = _o.ints;
    portoff = _abs_port_int;
    break;
  case 2:
    locoff = _o.chans;
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
