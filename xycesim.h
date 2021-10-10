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
#ifndef __ACT_XYCE_SIM_H__
#define __ACT_XYCE_SIM_H__

#include <common/simdes.h>
#include "actsim.h"

struct waveform_step {
  double tm;
  double val;
};

class XyceSim;

struct xycefanout {
  A_DECL (char *, dac_id);
};

class XyceActInterface {

public:
  XyceActInterface ();
  
  static XyceActInterface *getXyceInterface () {
    if (!_single_inst) {
      _single_inst = new XyceActInterface ();
    }
    return _single_inst;
  }
  
  static void addProcess (XyceSim *inst) {
    getXyceInterface()->_addProcess (inst);
  }

  /* -- initialize simulator -- */
  void initXyce ();

private:
  void _addProcess (XyceSim *);

  /* simulation state goes here */

  void *_xyce_ptr;		// Xyce pointer (C interface)
  
  double _xycetime;		// When Xyce is not running, this is
				// the last time that was simulated

  double _Vdd;			// power supply
  double _slewrate;		// fast slew threshold
  
  double _timescale;		// Conversion factor between actsim
				// integer time and Xyce time

  double _percent;		// threshold

  double _settling_time;	// settling time for simulation
    
  A_DECL (waveform_step, _wave); // template waveform [0..1] for
				// digital to analog conversion

  struct iHashtable *_to_xyce;	// global bool ID to Xyce DAC (xycefanout)
  struct Hashtable *_from_xyce; // Xyce ADC output to global bool ID

  A_DECL (XyceSim *, _analog_inst);

  static XyceActInterface *_single_inst;
};


class XyceSim : public ActSimObj {
 public:
  XyceSim (ActSimCore *sim, Process *p);
     /* initialize simulation, and create initial event */
  ~XyceSim ();

  int Step (int ev_type);	/* run a step of the simulation */

  void computeFanout ();

  int getBool (int lid) {
    int off = getGlobalOffset (lid, 0);
    return _sc->getBool (off);
  }

  int getOffset (act_connection *cx) {
    int off = _sc->getLocalOffset (cx, _si, NULL);
    return getGlobalOffset (off, 0);
  }
    
  void setBool (int lid, int v);

 private:
  stateinfo_t *_si;
};



#endif /* __ACT_XYCE_SIM_H__ */
