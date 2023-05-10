/*************************************************************************
 *
 *  Copyright (c) 2021 Rajit Manohar
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
#ifndef __ACTSIM_STATE_H__
#define __ACTSIM_STATE_H__

#include <act/act.h>

class ActSimCore;

class expr_multires {
 public:
  expr_multires(Data *d = NULL) {
    _d = NULL;
    nvals = 0;
    v = NULL;
    _init (d);
  }
  ~expr_multires() {
    _delete_objects ();
  }

  void setSingle (BigInt &x) {
    _d = NULL;
    if (nvals != 1) {
      _delete_objects ();
      nvals = 1;
      NEW (v, BigInt);
      new (v) BigInt;
    }
    *v = x;
  }

  void setSingle (unsigned long val) {
    _d = NULL;
    if (nvals != 1) {
      _delete_objects ();
      nvals = 1;
      NEW (v, BigInt);
      new (v) BigInt;
    }
    v->setWidth (BIGINT_BITS_ONE);
    v->setVal (0, val);
  }

  expr_multires (expr_multires &&m) {
    v = m.v;
    nvals = m.nvals;
    m.nvals = 0;
    m.v = NULL;
    _d = m._d;
  }
  
  expr_multires (expr_multires &m) {
    nvals = m.nvals;
    if (nvals > 0) {
      MALLOC (v, BigInt, nvals);
      for (int i=0; i < nvals; i++) {
	new (&v[i]) BigInt;
	v[i] = m.v[i];
      }
    }
    _d = m._d;
  }

  void Print (FILE *fp);
  
  expr_multires &operator=(expr_multires &&m) {
    if (nvals > 0) {
      FREE (v);
    }
    v = m.v;
    nvals = m.nvals;
    m.nvals = 0;
    _d = m._d;
    return *this;
  }
  
  expr_multires &operator=(expr_multires &m) {
    if (nvals != m.nvals) {
      _delete_objects ();
      if (m.nvals > 0) {
	MALLOC (v, BigInt, m.nvals);
	for (int i=0; i < m.nvals; i++) {
	  new (&v[i]) BigInt;
	}
      }
    }
    nvals = m.nvals;
    if (nvals > 0) {
      for (int i=0; i < nvals; i++) {
	v[i] = m.v[i];
      }
    }
    _d = m._d;
    return *this;
  }

  void fillValue (Data *d, ActSimCore *sc, int off_i, int off_b);

  void setField (ActId *field, BigInt *v);
  void setField (ActId *field, expr_multires *v);
  BigInt *getField (ActId *x);
  expr_multires getStruct (ActId *x);

  BigInt *v;
  int nvals;

private:
  void _delete_objects () {
    if (nvals > 0) {
      for (int i=0; i < nvals; i++) {
	v[i].~BigInt();
      }
      FREE (v);
    }
    v = NULL;
    nvals = 0;
  }
  int _count (Data *d);
  void _init_helper (Data *d, int *pos);
  void _init (Data *d);
  void _fill_helper (Data *d, ActSimCore *sc, int *pos, int *oi, int *ob);
  Data *_d;
};

struct extra_state_alloc {
  void *space;
  int sz;
};


#endif /* __ACTSIM_STATE_H__ */
