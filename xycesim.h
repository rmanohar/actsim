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

class XyceSim;

struct xycefanout {
  int val;
  A_DECL (char *, dac_id);
};

class xyceIO;

class XyceActInterface {

public:
  XyceActInterface ();
  ~XyceActInterface ();
  
  static XyceActInterface *getXyceInterface () {
    if (!_single_inst) {
      _single_inst = new XyceActInterface ();
    }
    return _single_inst;
  }
  
  static void addProcess (XyceSim *inst) {
    getXyceInterface()->_addProcess (inst);
  }

  static void stopXyce () {
    if (_single_inst) {
      delete _single_inst;
    }
    _single_inst = NULL;
  }

  void initGlobalFanout (ActSimCore *, XyceSim *);

  /* -- initialize simulator -- */
  void initXyce ();
  void updateDAC ();

  /* -- run one timestep block -- */
  void step ();

  inline int digital(int oval, double x) {
    if (x >= _Vhigh) {
      return 1;
    }
    else if (x <= _Vlow) {
      return 0;
    }
    else {
      return oval;
    }
  }

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

  double _Vhigh, _Vlow;		// high/low  values

  double _settling_time;	// settling time for simulation

  A_DECL (double, _wave_time);	// template waveform [0..1] for
				// digital to analog conversion
  A_DECL (double, _wave_voltage);

  int _case_for_sim;
  int _dump_all;		// dump all analog signals

  const char *_output_fmt;	// output format

  struct iHashtable *_to_xyce;	// global bool ID to Xyce DAC (xycefanout)
  struct Hashtable *_from_xyce; // Xyce ADC output to global bool ID

  A_DECL (XyceSim *, _analog_inst);

  static XyceActInterface *_single_inst;

  /* allocated state for xyce interface query functions */

  double **_time_points;
  double **_voltage_points;
  int *_num_points;
  char **_names;
  int _max_points;

  Event *_pending;		// current pending event

  xyceIO *_ioiface;

  A_DECL (netlist_global_port, xyce_glob);
  
};


class XyceSim : public ActSimObj {
 public:
  XyceSim (ActSimCore *sim, Process *p);
     /* initialize simulation, and create initial event */
  ~XyceSim ();

  int Step (Event *ev);		/* run a step of the simulation */

  void computeFanout ();

  int getBool (int lid) {
    int off = getGlobalOffset (lid, 0);
    return _sc->getBool (off);
  }

  int getGlobalBool (int off) {
    return _sc->getBool (off);
  }

  ActSimCore *getSimCore() { return _sc; }

  int hasOffset (act_connection *cx) {
    return _sc->hasLocalOffset (cx, _si);
  }

  int getOffset (act_connection *cx) {
    int off = _sc->getLocalOffset (cx, _si, NULL);
    return getGlobalOffset (off, 0);
  }

  int getPortOffset (int idx) {
    return _abs_port_bool[idx];
  }
    
  void setBool (int lid, int v);
  void setGlobalBool (int off, int v);

  void propagate ();

  stateinfo_t *getSI () { return _si; }
  act_boolean_netlist_t *getBNL() { return _bnl; }

 private:
  stateinfo_t *_si;
  act_boolean_netlist_t *_bnl;
};



#endif /* __ACT_XYCE_SIM_H__ */
