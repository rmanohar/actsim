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
#ifndef __ACTSIM__EXT_H__
#define __ACTSIM__EXT_H__

#include <act/act.h>
#include <common/misc.h>


struct expr_res {
  unsigned long v;		/* value */
  int width;			/* bitwidth */
};

#define ACT_EXPR_RES_PRINTF "%lu"

#ifdef __cplusplus

class ActSimCore;

class expr_multires {
 public:
  expr_multires(Data *d = NULL) { _d = NULL; nvals = 0; v = NULL; _init (d); }
  ~expr_multires() { if (nvals > 0) { FREE (v); } nvals = 0; v = NULL; }

  void setSingle (expr_res &x) {
    _d = NULL;
    if (nvals != 1) {
      if (nvals > 0) {
	FREE (v);
      }
      nvals = 1;
      NEW (v, expr_res);
    }
    *v = x;
  }

  void setSingle (unsigned long val) {
    _d = NULL;
    if (nvals != 1) {
      if (nvals > 0) {
	FREE (v);
      }
      nvals = 1;
      NEW (v, expr_res);
    }
    v->v = val;
    v->width = 64;
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
      MALLOC (v, expr_res, nvals);
      bcopy (m.v, v, sizeof (expr_res)*nvals);
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
      if (nvals > 0) {
	FREE (v);
      }
      if (m.nvals > 0) {
	MALLOC (v, expr_res, m.nvals);
      }
    }
    nvals = m.nvals;
    if (nvals > 0) {
      bcopy (m.v, v, sizeof (expr_res)*nvals);
    }
    _d = m._d;
    return *this;
  }

  void fillValue (Data *d, ActSimCore *sc, int off_i, int off_b);

  void setField (ActId *field, expr_res *v);
  void setField (ActId *field, expr_multires *v);

  expr_res *v;
  int nvals;

private:
  int _count (Data *d);
  void _init_helper (Data *d, int *pos);
  void _init (Data *d);
  void _fill_helper (Data *d, ActSimCore *sc, int *pos, int *oi, int *ob);
  Data *_d;
  int _find_offset (ActId *x, Data *d);
  
};

#endif

#endif /* __ACTSIM__EXT_H__ */
