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
      chans[i].send_here = 0;
      chans[i].recv_here = 0;
      chans[i].sender_probe = 0;
      chans[i].receiver_probe = 0;
      chans[i].len = 0;
      chans[i].data.nvals = 0;
      chans[i].data2.nvals = 0;
      chans[i].data = vinit;
      chans[i].data2 = vinit;
      chans[i].w = new WaitForOne(0);
      chans[i].probe = NULL;
      chans[i].fragmented = 0;
      chans[i].frag_st = 0;
      chans[i].ufrag_st = 0;
      chans[i].ct = NULL;
      chans[i].fH = NULL;
      chans[i].cm = NULL;
      chans[i]._dummy = NULL;
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
      delete chans[i].w;
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

void ActSimState::setBool (int x, int v)
{
  int special = 0;
  if (isSpecialBool (x)) {
    special = 1;
  }

  if (special) {
    if (!ActExclConstraint::safeChange (this, x, v)) {
      return;
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
