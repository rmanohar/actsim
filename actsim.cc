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

  if (map) {
    for (int i=0; i < map->size; i++) {
      for (ihash_bucket_t *b = map->head[i]; b; b = b->next) {
	ChpSimGraph *g = (ChpSimGraph *)b->v;
	delete g;
      }
    }
    ihash_free (map);
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
    sg = _build_chp_graph (c->c,  &stop);
    b->v = sg;
  }
  ChpSim *x = new ChpSim (sg, c->c, this);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
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
    sg = _build_chp_graph (c->c,  &stop);
    b->v = sg;
  }
  ChpSim *x = new ChpSim (sg, c->c, this);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
}

void ActSimCore::_add_prs (act_prs *p)
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

  /* need prs simulation graph */

  PrsSim *x = new PrsSim (NULL, p, this);
  x->setName (_curinst);
  x->setOffsets (&_curoffset);
  x->setPorts (_cur_abs_port_bool, _cur_abs_port_int, _cur_abs_port_chan);
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
    _add_prs (l->getprs());
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



ActSimState::ActSimState (int bools, int ints, int chantot)
{
#if 0
  printf ("# bools=%d, ints=%d, chans=%d\n",
	  bools, ints, chantot);
#endif
  nbools = bools;
  
  if (bools > 0) {
    bits = bitset_new (bools);
  }
  else {
    bits = NULL;
  }

  nints = ints;
  if (nints > 0) {
    MALLOC (ival, int, nints);
  }
  else {
    ival = NULL;
  }

  nchans = chantot;
  if (nchans > 0) {
    MALLOC (chans, act_channel_state, nchans);
    for (int i=0; i < nchans; i++) {
      chans[i].send_here = 0;
      chans[i].recv_here = 0;
      chans[i].len = 0;
      chans[i].data = 0;
      chans[i].data2 = 0;
      chans[i].w = new WaitForOne(0);
    }
  }
  else {
    chans = NULL;
  }
  gshared = new WaitForOne (10);
}

ActSimState::~ActSimState()
{
  if (bits) {
    bitset_free (bits);
  }
  if (ival) {
    FREE (ival);
  }
  if (chans) {
    for (int i=0; i < nchans; i++) {
      delete chans[i].w;
    }
    FREE (chans);
  }
  delete gshared;
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

  b = ihash_lookup (si->bnl->cH, (long)c);
  Assert (b, "What?");

  v = (act_booleanized_var_t *)b->v;
  x = mapIdToLocalOffset (c, si);

  if (type) {
    if (v->ischan) {
      *type = 2;
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


/*------------------------------------------------------------------------
 * The CHP simulation graph
 *------------------------------------------------------------------------
 */

Expr *expr_to_chp_expr (Expr *e, ActSimCore *s)
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
    /* l is an Id */
    ret->u.v = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), NULL);
    NEW (ret->u.e.r, Expr);
    ret->u.e.r->type = e->u.e.r->type;
    ret->u.e.r->u.e.l = expr_dup_const (e->u.e.r->u.e.l);
    ret->u.e.r->u.e.r = expr_dup_const (e->u.e.r->u.e.r);
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
      ret->u.v = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), &type);
      if (type == 2) {
	ret->type = E_CHP_VARCHAN;
      }
      else if (type == 1) {
	ret->type = E_CHP_VARINT;
      }
      else {
	ret->type = E_CHP_VARBOOL;
      }
    }
    break;

  case E_PROBE:
    ret->u.v = s->getLocalOffset ((ActId *)e->u.e.l, s->cursi(), NULL);
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
	tmp->type = e->type;
	tmp->u.e.l = expr_to_chp_expr (e->u.e.l, s);
	e = e->u.e.r;
      }
    }
    break;


  case E_SELF:
    /* FIXME: E_SELF! */
  default:
    fatal_error ("Unknown expression type %d\n", e->type);
    break;
  }
  return ret;
}


chpsimstmt *gc_to_chpsim (act_chp_gc_t *gc, ActSimCore *s)
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

/*---

  XXX: will need the state of the DES object too:
  For chp simulation, this corresponds to the program counter and the
  wait count for comma end nodes
  
  ---*/



ChpSimGraph::ChpSimGraph (ActSimCore *s)
{
  state = s;
  stmt = NULL;
  next = NULL;
  all = NULL;
  wait = 0;
  tot = 0;
}


ChpSimGraph *ActSimCore::_build_chp_graph (act_chp_lang_t *c, ChpSimGraph **stop)
{
  ChpSimGraph *ret = NULL;
  ChpSimGraph *tmp2;
  int i, count;
  
  if (!c) return NULL;

  switch (c->type) {
  case ACT_CHP_SEMI:
    if (list_length (c->u.semi_comma.cmd)== 1) {
      return _build_chp_graph
	((act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop);
    }
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ChpSimGraph *tmp = _build_chp_graph (t, &tmp2);
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
      return _build_chp_graph
	((act_chp_lang_t *)list_value (list_first (c->u.semi_comma.cmd)), stop);
    }
    ret = new ChpSimGraph (this);
    *stop = new ChpSimGraph (this);
    ret->next = *stop; // not sure we need this, but this is the fork/join
		       // connection

    count = 0;
    MALLOC (ret->all, ChpSimGraph *, list_length (c->u.semi_comma.cmd));
    i = 0;
    for (listitem_t *li = list_first (c->u.semi_comma.cmd);
	 li; li = list_next (li)) {
      act_chp_lang_t *t = (act_chp_lang_t *) list_value (li);
      ret->all[i] = _build_chp_graph ((act_chp_lang_t *)list_value (li), &tmp2);
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
    ret = new ChpSimGraph (this);
    ret->stmt = gc_to_chpsim (c->u.gc, this);
    i = 0;
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      i++;
    }
    Assert (i >= 1, "What?");
    MALLOC (ret->all, ChpSimGraph *, i);
      
    (*stop) = new ChpSimGraph (this);

    if (c->type == ACT_CHP_LOOP) {
      ret->next = (*stop);
    }
    i = 0;
    for (act_chp_gc_t *gc = c->u.gc; gc; gc = gc->next) {
      ret->all[i] = _build_chp_graph (gc->s, &tmp2);
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
    ret = new ChpSimGraph (this);
    ret->stmt = gc_to_chpsim (c->u.gc, this);
    (*stop) = new ChpSimGraph (this);
    ret->next = (*stop);
    MALLOC (ret->all, ChpSimGraph *, 1);
    ret->all[0] = _build_chp_graph (c->u.gc->s, &tmp2);
    tmp2->next = ret;
    break;
    
  case ACT_CHP_SKIP:
    break;
    
  case ACT_CHP_SEND:
    ret = new ChpSimGraph (this);
    NEW (ret->stmt, chpsimstmt);
    ret->stmt->type = CHPSIM_SEND;
    {
      listitem_t *li;
      ret->stmt->u.send.el = list_new ();
      for (li = list_first (c->u.comm.rhs); li; li = list_next (li)) {
	Expr *e = (Expr *) list_value (li);
	list_append (ret->stmt->u.send.el, expr_to_chp_expr (e, this));
      }
    }    
    ret->stmt->u.send.chvar = getLocalOffset (c->u.comm.chan, cursi(), NULL);
    (*stop) = ret;
    break;
    
    
  case ACT_CHP_RECV:
    ret = new ChpSimGraph (this);
    NEW (ret->stmt, chpsimstmt);
    ret->stmt->type = CHPSIM_RECV;
    {
      listitem_t *li;
      ret->stmt->u.recv.vl = list_new ();
      for (li = list_first (c->u.comm.rhs); li; li = list_next (li)) {
	ActId *id = (ActId *) list_value (li);
	int type;
	int x = getLocalOffset (id, cursi(), &type);
	list_append (ret->stmt->u.recv.vl, (void *)(long)type);
	list_append (ret->stmt->u.recv.vl, (void *)(long)x);
      }
    }
    ret->stmt->u.recv.chvar = getLocalOffset (c->u.comm.chan, cursi(), NULL);
    (*stop) = ret;
    break;

  case ACT_CHP_FUNC:
    if (strcmp (string_char (c->u.func.name), "log") != 0) {
      warning ("Built-in function `%s' is not known; valid values: log",
	       c->u.func.name);
    }
    else {
      listitem_t *li;
      ret = new ChpSimGraph (this);
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
	  x->u.e = expr_to_chp_expr (tmp->u.e, this);
	}
	list_append (ret->stmt->u.fn.l, x);
      }
      (*stop) = ret;
    }
    break;
    
  case ACT_CHP_ASSIGN:
    ret = new ChpSimGraph (this);
    NEW (ret->stmt, chpsimstmt);
    ret->stmt->type = CHPSIM_ASSIGN;
    ret->stmt->u.assign.e = expr_to_chp_expr (c->u.assign.e, this);
    {
      int type;
      ret->stmt->u.assign.var = getLocalOffset (c->u.assign.id, cursi(), &type);
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


int ActSimState::getInt (int x)
{
  Assert (0 <= x && x < nints, "What");
  return ival[x];
}

void ActSimState::setInt (int x, int v)
{
  Assert (0 <= x && x < nints, "What");
  ival[x] = v;
}

act_channel_state *ActSimState::getChan (int x)
{
  Assert (0 <= x && x < nchans, "What");
  return &chans[x];
}

int ActSimState::getBool (int x)
{
  if (bitset_tst (bits, x)) {
    return 1;
  }
  else {
    return 0;
  }
}

void ActSimState::setBool (int x, int v)
{
  if (v) {
    bitset_set (bits, x);
  }
  else {
    bitset_clr (bits, x);
  }
}
