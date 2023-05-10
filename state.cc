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
#include "actsim.h"
#include <common/misc.h>

ActSimState::ActSimState (int bools, int ints, int chantot)
{
#if 0
  printf ("# bools=%d, ints=%d, chans=%d\n",
	  bools, ints, chantot);
#endif
  nbools = bools;
  
  if (bools > 0) {
    bits = bitset_new (bools*3);
    for (int i=0; i < bools; i++) {
      bitset_set (bits, 3*i+1);
    }
  }
  else {
    bits = NULL;
  }
  hazards = NULL;

  nints = ints;
  if (nints > 0) {
    MALLOC (ival, BigInt, nints);
    for (int i=0; i < nints; i++) {
      new (&ival[i]) BigInt;
    }
  }
  else {
    ival = NULL;
  }

  nchans = chantot;
  if (nchans > 0) {
    expr_multires vinit;
    MALLOC (chans, act_channel_state, nchans);
    for (int i=0; i < nchans; i++) {
      new (&chans[i]) act_channel_state (vinit);
    }
  }
  else {
    chans = NULL;
  }

  extra_state = list_new ();
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
      chans[i].~act_channel_state();
    }
    FREE (chans);
  }

  for (listitem_t *li = list_first (extra_state); li; li = list_next (li)) {
    struct extra_state_alloc *s;
    s = (struct extra_state_alloc *) list_value (li);
    FREE (s->space);
    FREE (s);
  }
  list_free (extra_state);
}
		 
BigInt *ActSimState::getInt (int x)
{
  Assert (0 <= x && x < nints, "What");
  return &ival[x];
}

void ActSimState::setInt (int x, BigInt &v)
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
  if (bitset_tst (bits, 3*x+1)) {
    /* X */
    return 2;
  }
  if (bitset_tst (bits, 3*x)) {
    return 1;
  }
  else {
    return 0;
  }
}

bool ActSimState::setBool (int x, int v)
{
  int special = 0;
  if (isSpecialBool (x)) {
    special = 1;
  }

  if (special) {
    if (!ActExclConstraint::safeChange (this, x, v)) {
      return false;
    }

    ActTimingConstraint *tc = ActTimingConstraint::findBool (x);
    while (tc) {
      tc->update (x, v);
      tc = tc->getNext (x);
    }
  }

  if (v == 1) {
    bitset_set (bits, 3*x);
    bitset_clr (bits, 3*x+1);
  }
  else if (v == 0) {
    bitset_clr (bits, 3*x);
    bitset_clr (bits, 3*x+1);
  }
  else {
    bitset_set (bits, 3*x+1);
  }
  return true;
}

void *ActSimState::allocState (int sz)
{
  struct extra_state_alloc *s;

  NEW (s, struct extra_state_alloc);
  s->sz = sz;
  MALLOC (s->space, char, sz);
  list_append (extra_state, s);
  return s->space;
}



int expr_multires::_count (Data *d)
{
  int n = 0;
  Assert (d, "What?");
  for (int i=0; i < d->getNumPorts(); i++) {
    int sz = 1;
    InstType *it = d->getPortType (i);
    if (it->arrayInfo()) {
      sz = it->arrayInfo()->size();
    }
    if (TypeFactory::isStructure (it)) {
      n += _count (dynamic_cast<Data *>(d->getPortType(i)->BaseType()))*sz;
    }
    else {
      n += sz;
    }
  }
  return n;
}

void expr_multires::_init_helper (Data *d, int *pos)
{
  Assert (d, "What?");
  for (int i=0; i < d->getNumPorts(); i++) {
    int sz = 1;
    InstType *it = d->getPortType (i);
    if (it->arrayInfo()) {
      sz = it->arrayInfo()->size();
    }
    if (TypeFactory::isStructure (it)) {
      while (sz > 0) {
	_init_helper (dynamic_cast<Data *>(it->BaseType()), pos);
	sz--;
      }
    }
    else if (TypeFactory::isBoolType (it)) {
      while (sz > 0) {
	v[*pos].setWidth (1);
	v[*pos].setVal (0, 0);
	*pos = (*pos) + 1;
	sz--;
      }
    }
    else if (TypeFactory::isIntType (it)) {
      while (sz > 0) {
	v[*pos].setWidth (1);
	v[*pos].setVal (0, 0);
	v[*pos].setWidth (TypeFactory::bitWidth (it));
	*pos = *pos + 1;
	sz--;
      }
    }
  }
}

void expr_multires::_init (Data *d)
{
  if (!d) return;
  _d = d;
  nvals = _count (d);
  MALLOC (v, BigInt, nvals);
  for (int i=0; i < nvals; i++) {
    new (&v[i]) BigInt;
  }
  int pos = 0;
  _init_helper (d, &pos);
  Assert (pos == nvals, "What?");
}

void expr_multires::_fill_helper (Data *d, ActSimCore *sc, int *pos, int *oi, int *ob)
{
  Assert (d, "Hmm");
  for (int i=0; i < d->getNumPorts(); i++) {
    int sz = 1;
    InstType *it = d->getPortType (i);
    if (it->arrayInfo()) {
      sz = it->arrayInfo()->size();
    }
    if (TypeFactory::isStructure (it)) {
      while (sz > 0) {
	_fill_helper (dynamic_cast<Data *>(it->BaseType()),
		      sc, pos, oi, ob);
	sz--;
      }
    }
    else if (TypeFactory::isBoolType (it)) {
      while (sz > 0) {
	if (sc->getBool (*ob)) {
	  v[*pos].setVal (0, 1);
	}
	else {
	  v[*pos].setVal (0, 0);
	}
	*ob = *ob + 1;
	*pos = (*pos) + 1;
	sz--;
      }
    }
    else if (TypeFactory::isIntType (it)) {
      while (sz > 0) {
	v[*pos] = *(sc->getInt (*oi));
	*oi = *oi + 1;
	*pos = *pos + 1;
	sz--;
      }
    }
  }
}

void expr_multires::fillValue (Data *d, ActSimCore *sc, int off_i, int off_b)
{
  int pos = 0;
  _fill_helper (d, sc, &pos, &off_i, &off_b);
}


BigInt *expr_multires::getField (ActId *x)
{
  Assert (x, "setField with scalar called with NULL ID value");
  int off = _d->getStructOffset (x, NULL);
  Assert (0 <= off && off < nvals, "Hmm");
  return &v[off];
}

void expr_multires::setField (ActId *x, BigInt *val)
{
  Assert (x, "setField with scalar called with NULL ID value");
  int off = _d->getStructOffset (x, NULL);
  Assert (0 <= off && off < nvals, "Hmm");

  int w = v[off].getWidth();

  v[off] = *val;
  v[off].setWidth (w);
}

void expr_multires::setField (ActId *x, expr_multires *m)
{
  if (!x) {
    //m->Print (stdout);
    //printf ("\n");
    *this = *m;
  }
  else {
    int off = _d->getStructOffset (x, NULL);
    Assert (0 <= off && off < nvals, "Hmm");
    Assert (off + m->nvals <= nvals, "What?");
    for (int i=0; i < m->nvals; i++) {
      int w = v[off + i].getWidth ();
      v[off + i] = m->v[i];
      v[off + i].setWidth (w);
    }
  }
}


void expr_multires::Print (FILE *fp)
{
  fprintf (fp, "v:%d;", nvals);
  for (int i=0; i < nvals; i++) {
    fprintf (fp, " %d(w=%d):%lu", i, v[i].getWidth(), v[i].getVal (0));
  }
}


expr_multires expr_multires::getStruct (ActId *x)
{
  InstType *it;
  int off, sz;

  off = _d->getStructOffset (x, &sz, &it);

  if (off == -1) {
    warning ("expr_multires::getStruct(): failed for field starting with %s", x->getName());
    return *this;
  }
  if (!TypeFactory::isStructure (it)) {
    warning ("expr_multires::getStruct(): not a structure! Failed for field starting with %s", x->getName());
    return *this;
  }
  Data *d = dynamic_cast<Data *> (it->BaseType());
  Assert (d, "Hmm");

  expr_multires m(d);

  Assert (m.nvals == sz, "What are we doing?");

  for (int i=off; i < off + sz; i++) {
    m.v[i-off] = v[i];
  }

  return m;
}
