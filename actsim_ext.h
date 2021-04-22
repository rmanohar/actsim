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

#include <common/misc.h>


struct expr_res {
  unsigned long v;		/* value */
  int width;			/* bitwidth */
};

#define ACT_EXPR_RES_PRINTF "%lu"

#ifdef __cplusplus

class expr_result {
 public:
  expr_result() { nvals = 0; }
  ~expr_result() { if (v) { FREE (v); } }

  int nvals;
  expr_res *v;
};

#endif

#endif /* __ACTSIM__EXT_H__ */
