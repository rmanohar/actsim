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
#include "xycesim.h"
#include "config_pkg.h"
#include <common/config.h>
#include <ctype.h>
#include <lispCli.h>

#ifdef FOUND_N_CIR_XyceCInterface

#include <N_CIR_XyceCInterface.h>

#endif

XyceActInterface *XyceActInterface::_single_inst = NULL;


XyceActInterface::XyceActInterface()
{
  if (_single_inst) {
    fatal_error ("Creating multiple copies of a XyceActInterface?");
  }

  _xyce_ptr = NULL;
  _xycetime = 0.0;
  A_INIT (_wave_time);
  A_INIT (_wave_voltage);
  _to_xyce = NULL;
  _from_xyce = NULL;
  A_INIT (_analog_inst);
  _single_inst = this;

  _time_points = NULL;
  _voltage_points = NULL;
  _names = NULL;
  _max_points = 0;

  config_set_default_real ("sim.device.timescale", 1e-12);
  config_set_default_real ("sim.device.analog_window", 0.05);
  config_set_default_real ("sim.device.settling_time", 1e-12);
  config_set_default_real ("sim.device.waveform_time", 2e-12);
  config_set_default_int ("sim.device.digital_timestep", 10);
  config_set_default_int ("sim.device.waveform_steps", 10);
  config_set_default_int ("sim.device.case_for_sim", 0);
  config_set_default_int ("sim.device.dump_all", 0);
  config_set_default_string ("sim.device.output_format", "raw");

  char *fname = config_file_name ("lint.conf");
  if (fname) {
    config_read ("lint.conf");
    FREE (fname);
  }

  _Vdd = config_get_real ("lint.Vdd");

  /* units: V/ns */
  _slewrate = 1/(0.5*1.0/config_get_real ("lint.slewrate_fast_threshold") +
		 0.5*1.0/config_get_real ("lint.slewrate_slow_threshold") );

  /* simulation unit conversion between integers and analog sim */
  _timescale = config_get_real ("sim.device.timescale");
  _percent = config_get_real ("sim.device.analog_window");
  if (_percent < 0 || _percent > 1) {
    fatal_error ("sim.device.analog_window parameter must be in [0,1]\n");
  }

  _Vhigh = _Vdd*(1-_percent);
  _Vlow = _Vdd*_percent;
  
  _settling_time = config_get_real ("sim.device.settling_time");
  _step = config_get_int ("sim.device.digital_timestep");

  _case_for_sim = config_get_int ("sim.device.case_for_sim");

  _dump_all = config_get_int ("sim.device.dump_all");

  _output_fmt = config_get_string ("sim.device.output_format");

  /* wave approximation */
  int nsteps = config_get_int ("sim.device.waveform_steps");
  double dV = _Vdd/nsteps;
  double curV = 0;
  double curt = 0;
  double dT = config_get_real ("sim.device.waveform_time")/nsteps;

  /* simple ramp */
  for (int i=0; i <= nsteps; i++) {
    A_NEW (_wave_voltage, double);
    A_NEXT (_wave_voltage) = curV;
    curV += dV;
    
    A_INC (_wave_voltage);
    
    A_NEW (_wave_time, double);
    A_NEXT (_wave_time) = curt;
    curt += dT;
    A_INC (_wave_time);
  }
  
  _pending = NULL;
}


XyceActInterface::~XyceActInterface()
{
  if (A_LEN (_analog_inst) == 0) {
    return;
  }
  if (_xyce_ptr == NULL) {
    return;
  }
#ifdef FOUND_N_CIR_XyceCInterface
  xyce_close (&_xyce_ptr);
#endif
}


void XyceActInterface::_addProcess (XyceSim *xc)
{
  A_NEW (_analog_inst, XyceSim *);
  A_NEXT (_analog_inst) = xc;
  A_INC (_analog_inst);

  if (!_pending) {
    _pending = new Event (xc, SIM_EV_MKTYPE (0,0), 0);
  }
}

static  void (*old_hook) (void);

static void _cleanup_xyce (void)
{
  XyceActInterface::stopXyce ();
  if (old_hook) {
    (*old_hook) ();
  }
}

void XyceActInterface::initXyce ()
{
  if (A_LEN (_analog_inst) == 0) {
    return;
  }
	     
#ifndef FOUND_N_CIR_XyceCInterface
  fatal_error ("Xyce interface not found at compile time.");
#else
  xyce_open (&_xyce_ptr);

  if (lisp_cli_exit_hook) {
    old_hook = lisp_cli_exit_hook;
  }
  else {
    old_hook = NULL;
  }
  lisp_cli_exit_hook = _cleanup_xyce;

  /* -- create spice netlist -- */

  Act *a = actsim_Act();
  ActPass *ap;
  ActNetlistPass *nl;
  netlist_t *top_nl;
  char buf[10240], buf2[10240];

  ap = a->pass_find ("prs2net");
  if (ap) {
    nl = dynamic_cast<ActNetlistPass *> (ap);
    Assert (nl, "Hmm");
  }
  else {
    nl = new ActNetlistPass (a);
  }
  nl->run (actsim_top());

  nl->mkStickyVisited ();

  FILE *sfp = fopen ("_xyce.sp", "w");
  if (!sfp) {
    fatal_error ("Could not open file `_xyce.sp' for writing");
  }
  fprintf (sfp, "*\n* auto-generated by actsim\n*\n");

  top_nl = nl->getNL (actsim_top());

  int found_vdd = 0, found_gnd = 0;

  const char *Vddname, *GNDname;
  Vddname = config_get_string ("net.global_vdd");
  GNDname = config_get_string ("net.global_gnd");

  for (int i=0; i < A_LEN (top_nl->bN->used_globals); i++) {
    ActId *tid = top_nl->bN->used_globals[i]->toid();
    tid->sPrint (buf, 10240);
    delete tid;
    if (strcmp (buf, Vddname) == 0) {
      found_vdd = 1;
    }
    else if (strcmp (buf, GNDname) == 0) {
      found_gnd = 1;
    }
    fprintf (sfp, ".global ");
    a->mfprintf (sfp, "%s", buf);
    fprintf (sfp, "\n");
  }

  if (!found_vdd) {
    fprintf (sfp, ".global %s\n", Vddname);
  }
  fprintf (sfp, "vvs0 %s 0 dc %gV\n", Vddname, _Vdd);

  if (!found_gnd) {
    fprintf (sfp, ".global %s\n", GNDname);
  }
  fprintf (sfp, "vvs1 %s 0 dc 0.0V\n\n", GNDname);

  fprintf (sfp, "* --- include models ---\n\n");
  fprintf (sfp, ".inc \"%s\"\n", config_get_string ("sim.device.model_files"));

  fprintf (sfp, "*\n* -- printing any spice bodies needed --\n*\n");

  for (int i=0; i < A_LEN (_analog_inst); i++) {
    XyceSim *xs = _analog_inst[i];
    nl->Print (sfp, xs->getProc());
  }
  nl->clrStickyVisited ();

  fprintf (sfp, "*\n* instances\n*\n");
  
  for (int i=0; i < A_LEN (_analog_inst); i++) {
    XyceSim *xs = _analog_inst[i];
    ActId *inst = xs->getName();
    netlist_t *n = nl->getNL (xs->getProc());

    Assert (n, "What?");

    fprintf (sfp, "X%d ", i);
    for (int j=0; j < A_LEN (n->bN->ports); j++) {
      int len;
      if (n->bN->ports[j].omit) continue;

      inst->sPrint (buf2, 10240);
      len = strlen (buf2);
      snprintf (buf2+len, 10240-len, ".");
      len += strlen (buf2+len);
      
      ActId *tid;
      tid = n->bN->ports[j].c->toid();
      tid->sPrint (buf2+len, 10240-len);
      delete tid;

      a->mangle_string (buf2, buf, 10240);
      fprintf (sfp, "%s ", buf);

      /* circuit simulators are case insensitive... */
      for (int k=0; buf[k]; k++) {
	buf[k] = tolower (buf[k]);
      }

      int off = xs->getOffset (n->bN->ports[j].c);

      if (n->bN->ports[j].input) {
	/* DAC */
	ihash_bucket_t *b;
	struct xycefanout *xf;
	if (!_to_xyce) {
	  _to_xyce = ihash_new (4);
	}
	b = ihash_lookup (_to_xyce, off);
	if (b) {
	  xf = (struct xycefanout *) b->v;
	}
	else {
	  b = ihash_add (_to_xyce, off);
	  NEW (xf, struct xycefanout);
	  A_INIT (xf->dac_id);
	  xf->val = 2; /* X */
	  b->v = xf;
	}
	A_NEW (xf->dac_id, char *);
	A_NEXT (xf->dac_id) = Strdup (buf);
	A_INC (xf->dac_id);
      }
      else {
	/* ADC */
	hash_bucket_t *b;
	
	if (!_from_xyce) {
	  _from_xyce = hash_new (4);
	}

	b = hash_lookup (_from_xyce, buf);
	if (b) {
	  warning ("Signal `%s' has a duplicate driver in Xyce!", buf);
	}
	else {
	  
	  b = hash_add (_from_xyce, buf);
	  b->i = off;
	}
      }
    }
    a->mfprintfproc (sfp, xs->getProc());
    fprintf (sfp, "\n");
  }

  fprintf (sfp, "*\n* ADCs and DACs\n*\n");

  fprintf (sfp, ".model myADC ADC (settlingtime=%g uppervoltagelimit=%g lowervoltagelimit=%g)\n", _settling_time, _Vdd*(1-_percent), _Vdd*_percent);
  fprintf (sfp, ".model myDAC DAC (tr=%g tf=%g)\n",
	   (_Vdd/_slewrate)*1e-9, (_Vdd/_slewrate)*1e-9);
  
  /* -- now we need the ADCs and DACs -- */
  if (_to_xyce) {
    ihash_iter_t it;
    ihash_bucket_t *b;
    ihash_iter_init (_to_xyce, &it);
    while ((b = ihash_iter_next (_to_xyce, &it))) {
      struct xycefanout *xf;
      xf = (struct xycefanout *) b->v;
      for (int i=0; i < A_LEN (xf->dac_id); i++) {
	fprintf (sfp, "YDAC %s %s GND myDAC\n", xf->dac_id[i], xf->dac_id[i]);
      }
    }
  }

  int max_sig_sz = 0;
  
  if (_from_xyce) {
    hash_iter_t it;
    hash_bucket *b;
    hash_iter_init (_from_xyce, &it);
    while ((b = hash_iter_next (_from_xyce, &it))) {
      fprintf (sfp, "YADC %s %s GND myADC\n", b->key, b->key);
      int tmplen = strlen (b->key);
      if (tmplen > max_sig_sz) {
	max_sig_sz = tmplen;
      }
    }
  }

  fprintf (sfp, ".tran 0 1\n");

  if (strcmp (_output_fmt, "prn") == 0) {
    fprintf (sfp, ".print tran\n");
  }
  else {
    fprintf (sfp, ".print tran format=%s\n", _output_fmt);
  }

  if (_dump_all) {
    fprintf (sfp, "+ v(*)\n");
  }
  else {
    if (_to_xyce) {
      ihash_iter_t it;
      ihash_bucket_t *b;
      ihash_iter_init (_to_xyce, &it);
      while ((b = ihash_iter_next (_to_xyce, &it))) {
	struct xycefanout *xf;
	xf = (struct xycefanout *) b->v;
	for (int i=0; i < A_LEN (xf->dac_id); i++) {
	  fprintf (sfp, "+ v(");
	  fprintf (sfp, "%s", xf->dac_id[i]);
	  fprintf (sfp, ")\n");
	}
      }
    }
    if (_from_xyce) {
      hash_iter_t it;
      hash_bucket *b;
      hash_iter_init (_from_xyce, &it);
      while ((b = hash_iter_next (_from_xyce, &it))) {
	fprintf (sfp, "+ v(");
	fprintf (sfp, "%s", b->key);
	fprintf (sfp, ")\n");
      }
    }
  }
  fprintf (sfp, "\n");

  fprintf (sfp, ".end\n");
  fclose (sfp);

  /* -- initialize waveforms -- */
  const char *_cinit_xyce[] =
    { "Xyce", "-quiet", "-l", "_xyce.log", "_xyce.sp" };
  
  int num = 5;
  char **_init_xyce;

  MALLOC (_init_xyce, char *, num+1);

  for (int i=0; i < num; i++) {
    _init_xyce[i] = Strdup (_cinit_xyce[i]);
  }
  _init_xyce[num] = NULL;
  xyce_initialize (&_xyce_ptr, num, _init_xyce);
  for (int i=0; i < num; i++) {
    FREE (_init_xyce[i]);
  }
  FREE (_init_xyce);

  if (_from_xyce) {
    Assert (_from_xyce->n > 0, "What?");

    MALLOC (_time_points, double *, _from_xyce->n);
    MALLOC (_voltage_points, double *, _from_xyce->n);
    MALLOC (_num_points, int, _from_xyce->n);
    MALLOC (_names, char *, _from_xyce->n);

    for (int i=0; i < _from_xyce->n; i++) {
      _num_points[i] = 0;
      _time_points[i] = NULL;
      _voltage_points[i] = NULL;
      MALLOC (_names[i], char, max_sig_sz + 6);
      _names[i][0] = '\0';
    }
  }
#endif
}

void XyceActInterface::updateDAC ()
{
#ifdef FOUND_N_CIR_XyceCInterface
  if (_to_xyce) {
    ihash_iter_t it;
    ihash_bucket_t *b;
    ihash_iter_init (_to_xyce, &it);

    for (int i=0; i < A_LEN (_wave_time); i++) {
      _wave_time[i] += _xycetime;
    }
    
    while ((b = ihash_iter_next (_to_xyce, &it))) {
      struct xycefanout *xf;
      int val;
      xf = (struct xycefanout *) b->v;

      val = _analog_inst[0]->getGlobalBool (b->key);

      if (val != xf->val) {
	/* change! */
#if 0
	printf ("%lu dac: %d -> %d @ %g\n", b->key, xf->val, val, _xycetime*1e12);
#endif	

	xf->val = val;

	if (val == 0) {	
	    /* flip waveform */
	  for (int j=0; j < A_LEN (_wave_time); j++) {
	    _wave_voltage[j] = _Vdd - _wave_voltage[j];
	  }
	}
	
	for (int i=0; i < A_LEN (xf->dac_id); i++) {
	  char buf[10240];
	  snprintf (buf, 10240, "ydac!%s", xf->dac_id[i]);
	  if (_case_for_sim) {
	    for (int j=0; buf[j]; j++) {
	      buf[j] = toupper (buf[j]);
	    }
	  }

	  if (val == 2) {
	    if (xyce_updateTimeVoltagePairs (&_xyce_ptr,
					     buf,
					     1, 
					     _wave_time + A_LEN (_wave_time)/2,
					     _wave_voltage + A_LEN (_wave_voltage)/2) == false) {
	      warning ("Xyce: updateTimeVoltagePairs failed! Aborting.");
	      _pending = NULL;
	      return;
	    }
	  }
	  else {
	    if (xyce_updateTimeVoltagePairs (&_xyce_ptr,
					     buf,
					     A_LEN (_wave_time),
					     _wave_time,
					     _wave_voltage) == false) {
	      warning ("Xyce: updateTimeVoltagePairs failed! Aborting.");
	      _pending = NULL;
	      return;
	    }
	  }
	}

	if (val == 0) {	
	    /* flip waveform back */
	  for (int j=0; j < A_LEN (_wave_time); j++) {
	    _wave_voltage[j] = _Vdd - _wave_voltage[j];
	  }
	}
      }

    }

    for (int i=0; i < A_LEN (_wave_time); i++) {
      _wave_time[i] -= _xycetime;
    }
  }
#endif  
}

void XyceActInterface::step()
{
  bool status;
  double tm;
  double actual;
  unsigned long ns;

#ifdef FOUND_N_CIR_XyceCInterface

  ns = (unsigned long)((_xycetime+0.95*_timescale)/_timescale);

  int sim_dt;

  if ((ns % _step) != 0) {
    sim_dt = _step - (ns % _step);
  }
  else {
    sim_dt = _step;
  }
  ns += sim_dt;
  tm = ns*_timescale;

  /* digital signals are shipped to Xyce in an event-based fashion */

  status = xyce_simulateUntil (&_xyce_ptr, tm,  &actual);
  if (status == false) {
    warning ("Xyce: simulateUntil failed. Stopping Xyce.");
    _pending = NULL;
    return;
  }

  _xycetime = actual;

  /* ship analog signals back to actsim */
  if (_from_xyce) {

    int npts = 0;
    if (xyce_getTimeVoltagePairsADCsz (&_xyce_ptr, &npts) != true) {
      warning ("Xyce: getTimeVoltagePairs call failed! Stopping Xyce.");
      _pending = NULL;
      return;
    }

    if (npts > 0) {
      if (npts > _max_points) {
	if (_max_points == 0) {
	  for (int i=0; i < _from_xyce->n; i++) {
	    MALLOC (_time_points[i], double, npts+1);
	    MALLOC (_voltage_points[i], double, npts+1);
	    _time_points[i][0] = -1;
	    _num_points[i] = 1;
	  }
	}
	else {
	  for (int i=0; i < _from_xyce->n; i++) {
	    REALLOC (_time_points[i], double, npts+1);
	    REALLOC (_voltage_points[i], double, npts+1);
	  }
	}
	_max_points = npts;
      }
      for (int i=0; i < _from_xyce->n; i++) {
	_time_points[i][_max_points] = _time_points[i][_num_points[i]-1];
	_voltage_points[i][_max_points] = _voltage_points[i][_num_points[i]-1];
      }

      int num_adcs;
      if (xyce_getTimeVoltagePairsADC (&_xyce_ptr,
				       &num_adcs, _names, _num_points,
				       _time_points, _voltage_points) == false) {
	warning ("Xyce: getTimeVoltagePairsADC call failed! Stopping Xyce.");
	_pending = NULL;
	return;
      }

      Assert (_from_xyce->n == num_adcs, "ADC count mismatch");

      for (int i=0; i < _from_xyce->n; i++) {
	for (int k=0; _names[i][k]; k++) {
	  _names[i][k] =  tolower(_names[i][k]);
	}

	int old_val, new_val;

	if (_time_points[i][_max_points] == -1) {  // initial
	  /* X */
	  old_val = 2;
	}
	else {
	  old_val = digital (2, _voltage_points[i][_max_points]);
	}

	new_val = digital (old_val, _voltage_points[i][_num_points[i]-1]);
#if 0
	printf (" >> %s   old %d: (%.4g,%.4g); ", _names[i], old_val,
		_time_points[i][_max_points]*1e12,
		_voltage_points[i][_max_points]);

	printf (" new %d:", new_val);
	
	for (int k=0; k < _num_points[i]; k++) {
	  printf (" (%.4g,%.4g)", _time_points[i][k]*1e12, _voltage_points[i][k]);
	}
	printf ("\n");
#endif	

	if (strncmp (_names[i], "yadc!", 5) != 0) {
	  warning ("Expected a yadc! name, got `%s'. Aborting.", _names[i]);
	  _pending = NULL;
	  return;
	}
	hash_bucket_t *b = hash_lookup (_from_xyce, _names[i]+5);
	if (!b) {
	  warning ("Name `%s' not found in the Xyce interface? Aborting.", _names[i]+5);
	  _pending = NULL;
	  return;
	}

	if (old_val != new_val) {
#if 0
	  printf ("%d adc: %d -> %d\n", b->i, old_val, new_val);
#endif
	  if (new_val == 2) {
	    _analog_inst[0]->msgPrefix();
	    printf ("WARNING: adc set `");
	    actsim_Act()->ufprintf (stdout, "%s", _names[i]+5);
	    printf ("' to X\n");
	  }
	  _analog_inst[0]->setGlobalBool (b->i, new_val);
	  _voltage_points[i][_num_points[i]] = new_val ? _Vdd : 0.0;
	}
      }
    }
  }
  _pending = new Event (_analog_inst[0], SIM_EV_MKTYPE (0, 0), sim_dt);
#endif
}

XyceSim::XyceSim (ActSimCore *sim, Process *p) : ActSimObj (sim, p)
{
  XyceActInterface::addProcess (this);
  _si = sim->cursi();
}

XyceSim::~XyceSim()
{
  XyceActInterface::stopXyce();
}

int XyceSim::Step (int ev_type)
{
  /* run simulation for X units of delay */
  XyceActInterface::getXyceInterface()->step ();
  return 1;
}

void XyceSim::computeFanout()
{
  stateinfo_t *si;
  si = _sc->getsi (_proc);
  Assert (si, "Hmm");
  for (int i=0; i < A_LEN (si->bnl->ports); i++) {
    if (si->bnl->ports[i].omit) continue;
    if (si->bnl->ports[i].input) {
      _sc->incFanout (getOffset (si->bnl->ports[i].c), 0, this);
    }
  }
}

void XyceSim::propagate()
{
  XyceActInterface::getXyceInterface()->updateDAC();
}


void XyceSim::setBool (int lid, int v)
{
  int off = getGlobalOffset (lid, 0);
  _sc->setBool (off, v);
}

void XyceSim::setGlobalBool (int off, int v)
{
  _sc->setBool (off, v);
  SimDES **arr = _sc->getFO (off, 0);
  for (int i=0; i < _sc->numFanout (off, 0); i++) {
    ActSimDES *p = dynamic_cast<ActSimDES *>(arr[i]);
    Assert (p, "What?");
    p->propagate ();
  }
}
