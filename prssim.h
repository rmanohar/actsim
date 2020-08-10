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
#ifndef __ACT_PRS_SIM_H__
#define __ACT_PRS_SIM_H__

#include <simdes.h>
#include "actsim.h"

class PrsSimGraph;

class PrsSim : public ActSimObj {
 public:
  PrsSim (PrsSimGraph *, act_prs *, ActSimCore *sim);
     /* initialize simulation, and create initial event */

  void Step (int ev_type);	/* run a step of the simulation */
  
 private:
  void varSet (int id, int type, expr_res v);
  int varSend (int pc, int wakeup, int id, expr_res v);
  int varRecv (int pc, int wakeup, int id, expr_res *v);

};



#endif /* __ACT_CHP_SIM_H__ */
