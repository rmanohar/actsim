/*************************************************************************
 *
 *  This file is part of the ACT library
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
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <act/act.h>
#include <act/passes.h>
#include <common/config.h>
#include "actsim.h"
#include "chpsim.h"
#include <lisp.h>
#include <lispCli.h>
#include <ctype.h>

static ActId *my_parse_id (const char *s)
{
   return ActId::parseId (s);
}

static void signal_handler (int sig)
{
  LispInterruptExecution = 1;
  SimDES::interrupt();
}

static void clr_interrupt (void)
{
  LispInterruptExecution = 0;
  SimDES::resume();
}

static void usage (char *name)
{
  fprintf (stderr, "Usage: %s <actfile> <process>\n", name);
  exit (1);
}

static ActStatePass *glob_sp;
ActSim *glob_sim;
static Act *glob_act;
static Process *glob_top;

int is_rand_excl()
{
  return glob_sim->isRandomChoice();
}

int process_cycle (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (glob_sim->isResetMode()) {
    while (!LispInterruptExecution) {
      if (!SimDES::matchPendingEvent (_match_hseprs)) {
	// no prs or hse pending events!
	break;
      }
      glob_sim->Step (1); // ignores breakpoints
    }
  }
  else {
    glob_sim->runSim (NULL);
  }
  return LISP_RET_TRUE;
}

int process_step (int argc, char **argv)
{
  long nsteps;
  if (argc != 1 && argc != 2) {
    fprintf (stderr, "Usage: %s [num]\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (argc == 1) {
    nsteps = 1;
  }
  else {
    sscanf (argv[1], "%ld", &nsteps);
    if (nsteps <= 0) {
      fprintf (stderr, "%s: zero/negative steps?\n", argv[0]);
      return LISP_RET_ERROR;
    }
  }
  glob_sim->Step (nsteps);
  if (SimDES::hasPendingEvent()) {
    return LISP_RET_TRUE;
  }
  else {
    return LISP_RET_FALSE;
  }
}

int process_advance (int argc, char **argv)
{
  long nsteps;
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <delay>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  sscanf (argv[1], "%ld", &nsteps);
  if (nsteps <= 0) {
    fprintf (stderr, "%s: zero/negative delay?\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->Advance (nsteps);
  if (SimDES::hasPendingEvent()) {
    return LISP_RET_TRUE;
  }
  else {
    return LISP_RET_FALSE;
  }
}

int process_initialize (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <process>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  Process *p = glob_act->findProcess (argv[1]);
  if (!p) {
    fprintf (stderr, "%s: could not find process %s\n", argv[0], argv[1]);
    return LISP_RET_ERROR;
  }
  if (!p->isExpanded()) {
    fprintf (stderr, "%s: `%s' is not an expanded process\n", argv[0], argv[1]);
    return LISP_RET_ERROR;
  }
  if (glob_sim) {
    delete glob_sim;
    delete glob_sp;
  }
  SimDES::Init ();
  glob_sp = new ActStatePass (glob_act);
  glob_sp->run (p);
  glob_sim = new ActSim (p);
  glob_sim->runInit ();
  return LISP_RET_TRUE;
}

static void dump_state (FILE *fp, ActInstTable *x)
{
  if (!x) {
    warning ("Didn't find info; is this a valid instance?");
    return;
  }
  
  if (x->obj) {
    x->obj->dumpState (fp);
  }
  if (x->H) {
    hash_bucket_t *b;
    hash_iter_t i;
    hash_iter_init (x->H, &i);
    while ((b = hash_iter_next (x->H, &i))) {
      ActInstTable *tmp = (ActInstTable *) b->v;
      dump_state (fp, tmp);
    }
  }
}


static void dump_coverage (FILE *fp, ActInstTable *x)
{
  if (!x) {
    warning ("Didn't find info; is this a valid instance?");
    return;
  }
  
  if (x->obj) {
    ChpSim *cobj = dynamic_cast <ChpSim *> (x->obj);
    if (cobj) {
      cobj->dumpStats (fp);
    }
  }
  if (x->H) {
    hash_bucket_t *b;
    hash_iter_t i;
    hash_iter_init (x->H, &i);
    while ((b = hash_iter_next (x->H, &i))) {
      ActInstTable *tmp = (ActInstTable *) b->v;
      dump_coverage (fp, tmp);
    }
  }
}


static unsigned long _get_energy (FILE *fp,
				  ActInstTable *x, double *lk,
				  unsigned long *area,
				  int ts = 0)
{
  unsigned long tot;
  unsigned long sub;
  double totl, subl;
  unsigned long suba, tota;

  if (!x) {
    warning ("Didn't find info; is this a valid instance?");
    *lk = 0;
    return 0;
  }

  tot = 0;
  totl = 0;
  tota = 0;
  
  if (x->obj) {
    tot = x->obj->getEnergy ();
    totl = x->obj->getLeakage ();
    tota = x->obj->getArea ();

    if (tot > 0 || totl > 0 || tota > 0) {
      for (int i=0; i < ts; i++) {
	fprintf (fp, "  ");
      }
      fprintf (fp, " - ");
      if (x->obj->getName()) {
	x->obj->getName()->Print (fp);
      }
      else {
	fprintf (fp, "-top-");
      }
      fprintf (fp, " %lu  (%g W); area: %lu\n", tot, totl, tota);
    }
  }
  sub = tot;
  subl = totl;
  suba = tota;
  if (x->H) {
    hash_bucket_t *b;
    hash_iter_t i;
    double tmpl;
    unsigned long tmpa;
    hash_iter_init (x->H, &i);
    while ((b = hash_iter_next (x->H, &i))) {
      ActInstTable *tmp = (ActInstTable *) b->v;
      tot += _get_energy (fp, tmp, &tmpl, &tmpa, ts+1);
      totl += tmpl;
      tota += tmpa;
    }
    if ((tot - sub) > 0 || ((totl - subl) > 0) || ((tota - suba) > 0)) {
      for (int i=0; i < ts; i++) {
	fprintf (fp, "  ");
      }
      fprintf (fp, " ---:subtree %lu (%g W); area: %lu\n",
	       tot - sub, totl - subl, tota - suba);
    }
  }
  *lk = totl;
  *area = tota;
  return tot;
}

ActInstTable *find_table (ActId *id, ActInstTable *x)
{
  char buf[1024];
  hash_bucket_t *b;
  
  if (!id) { return x; }

  if (!x->H) { return NULL; }

  ActId *tmp = id->Rest();
  id->prune();
  id->sPrint (buf, 1024);
  id->Append (tmp);

  b = hash_lookup (x->H, buf);
  if (!b) {
    return NULL;
  }
  else {
    return find_table (id->Rest(), (ActInstTable *)b->v);
  }
}

ActSimObj *find_object (ActId **id, ActInstTable *x)
{
  char buf[1024];
  hash_bucket_t *b;
  
  if (!(*id)) { return x->obj; }
  if (!x->H) { return x->obj; }

  if ((*id)->isNamespace()) {
    return x->obj;
  }

  ActId *tmp = (*id)->Rest();
  (*id)->prune();
  (*id)->sPrint (buf, 1024);
  (*id)->Append (tmp);

  b = hash_lookup (x->H, buf);
  if (!b) {
    return x->obj;
  }
  else {
    (*id) = (*id)->Rest();
    return find_object (id, (ActInstTable *)b->v);
  }
}



int process_procinfo (int argc, char **argv)
{
  ActId *id;
  FILE *fp;
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <filename> [<instance-name>]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (strcmp (argv[1], "-") == 0) {
    fp = stdout;
  }
  else {
    fp = fopen (argv[1], "w");
    if (!fp) {
      fprintf (stderr, "%s: could not open file `%s' for writing\n",
	       argv[0], argv[1]);
      return LISP_RET_ERROR;
    }
  }

  if (argc == 2) {
    /* all */
    id = NULL;
  }
  else {
    id = my_parse_id (argv[1]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[1]);
      return LISP_RET_ERROR;
    }
  }

  if (!id) {
    /*-- print state of each process --*/
    dump_state (fp, glob_sim->getInstTable ());
  }
  else {
    /*-- find this process --*/
    ActInstTable *inst = find_table (id, glob_sim->getInstTable());
    dump_state (fp, inst);
    delete id;
  }

  if (fp != stdout) {
    fclose (fp);
  }
  
  return LISP_RET_TRUE;
}

int process_getenergy (int argc, char **argv)
{
  ActId *id;
  double lk;
  unsigned long area;
  FILE *fp;
    
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <filename> [<instance-name>]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (strcmp (argv[1], "-") == 0) {
    fp = stdout;
  }
  else {
    fp = fopen (argv[1], "w");
    if (!fp) {
      fprintf (stderr, "%s: could not open file `%s' for writing\n",
	       argv[0], argv[1]);
      return LISP_RET_ERROR;
    }
  }
  
  if (argc == 2) {
    /* all */
    id = NULL;
  }
  else {
    id = my_parse_id (argv[2]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[2]);
      return LISP_RET_ERROR;
    }
  }

  if (!id) {
    /*-- print state of each process --*/
    fprintf (fp, "Total: %lu", _get_energy (fp,
				       glob_sim->getInstTable (), &lk, &area));
    fprintf (fp, "  (%g W); area: %lu\n", lk, area);
  }
  else {
    /*-- find this process --*/
    ActInstTable *inst = find_table (id, glob_sim->getInstTable());
    fprintf (fp, "Total: %lu", _get_energy (fp, inst, &lk, &area));
    fprintf (fp, "  (%g W); area: %lu\n", lk, area);
    delete id;
  }
  if (fp != stdout) {
    fclose (fp);
  }
  return LISP_RET_TRUE;
}

int process_coverage (int argc, char **argv)
{
  ActId *id;
  FILE *fp;
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <filename> [<instance-name>]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (strcmp (argv[1], "-") == 0) {
    fp = stdout;
  }
  else {
    fp = fopen (argv[1], "w");
    if (!fp) {
      fprintf (stderr, "%s: could not open file `%s' for writing\n",
	       argv[0], argv[1]);
      return LISP_RET_ERROR;
    }
  }

  if (argc == 2) {
    /* all */
    id = NULL;
  }
  else {
    id = my_parse_id (argv[1]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[1]);
      return LISP_RET_ERROR;
    }
  }

  if (!id) {
    /*-- print state of each process --*/
    dump_coverage (fp, glob_sim->getInstTable ());
  }
  else {
    /*-- find this process --*/
    ActInstTable *inst = find_table (id, glob_sim->getInstTable());
    dump_coverage (fp, inst);
    delete id;
  }

  if (fp != stdout) {
    fclose (fp);
  }
  
  return LISP_RET_TRUE;
}

int process_goto (int argc, char **argv)
{
  ActId *id;
  FILE *fp;
  if (argc != 3 && argc != 2) {
    fprintf (stderr, "Usage: %s [<inst-name>] <label>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (argc == 3) {
    id = my_parse_id (argv[1]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[1]);
      return LISP_RET_ERROR;
    }
  }
  else {
    id = NULL;
  }

  /*-- find this process --*/
  ActInstTable *inst = find_table (id, glob_sim->getInstTable());
  if (!inst) {
    fprintf (stderr, "Could not find instance `%s'\n", argv[1]);
    return LISP_RET_ERROR;
  }
  ChpSim *chp = dynamic_cast<ChpSim *>(inst->obj);
  if (!chp) {
    fprintf (stderr, "Instance `%s' is not a CHP process.\n", argv[1]);
    return LISP_RET_ERROR;
  }
  if (chp->jumpTo (argv[argc-1])) {
    return LISP_RET_TRUE;
  }
  else {
    return LISP_RET_ERROR;
  }
}


static int id_to_siminfo_raw (char *s, int *ptype, int *poffset, ActSimObj **pobj)
{
  ActId *id = my_parse_id (s);
  if (!id) {
    fprintf (stderr, "Could not parse `%s' into an identifier\n", s);
    return 0;
  }

  /* -- find object / id combo -- */
  ActId *tmp = id;
  ActSimObj *obj = find_object (&tmp, glob_sim->getInstTable());
  stateinfo_t *si;
  int offset, type;
  int res;
  act_connection *c;
  
  if (!obj) {
    fprintf (stderr, "Could not find `%s' in simulation\n", s);
    delete id;
    return 0;
  }

  /* -- now convert tmp into a local offset -- */

  si = glob_sp->getStateInfo (obj->getProc ());
  if (!si) {
    fprintf (stderr, "Could not find info for process `%s'\n", obj->getProc()->getName());
    delete id;
    return 0;
  }

  if (!si->bnl->cur->FullLookup (tmp, NULL)) {
    fprintf (stderr, "Could not find identifier `%s' within process `%s'\n",
	     s, obj->getProc()->getName());
    delete id;
    return 0;
  }

  if (!tmp->validateDeref (si->bnl->cur)) {
    fprintf (stderr, "Array index is missing/out of bounds!\n");
    return 0;
  }

  c = tmp->Canonical (si->bnl->cur);
  Assert (c, "What?");

  res = glob_sp->getTypeOffset (si, c, &offset, &type, NULL);
  if (!res) {
    /* it is possible that it is an array reference */
    Array *ta = NULL;
    while (tmp->Rest()) {
      tmp = tmp->Rest();
    }
    ta = tmp->arrayInfo();
    if (ta) {
      tmp->setArray (NULL);
      c = tmp->Canonical (si->bnl->cur);
      Assert (c, "Hmm...");
      res = glob_sp->getTypeOffset (si, c, &offset, &type, NULL);
      if (res) {
	InstType *it = si->bnl->cur->FullLookup (tmp, NULL);
	Assert (it->arrayInfo(), "What?");
	offset += it->arrayInfo()->Offset (ta);
      }
      tmp->setArray (ta);
    }
    if (!res) {
      fprintf (stderr, "Could not find identifier `%s' within process `%s'\n",
	       s, obj->getProc()->getName());
      delete id;
      return 0;
    }
  }

  *ptype = type;
  *poffset = offset;
  if (pobj) {
    *pobj = obj;
  }
  delete id;
  return 1;
}


static int id_to_siminfo (char *s, int *ptype, int *poffset, ActSimObj **pobj)
{
  int v = id_to_siminfo_raw (s, ptype, poffset, pobj);
  if (*ptype == 3) {
    *ptype = 2;
  }
  return v;
}

static int id_to_siminfo_glob (char *s,
			       int *ptype, int *poffset, ActSimObj **pobj)
{
  ActSimObj *myobj;
  myobj = NULL;
  if (!id_to_siminfo (s, ptype, poffset, &myobj)) {
    return 0;
  }
  if (!myobj) {
    return 0;
  }
  if (pobj) {
    *pobj = myobj;
  }
  
  *poffset = myobj->getGlobalOffset (*poffset, *ptype);
  return 1;
}

static int id_to_siminfo_glob_raw (char *s,
				   int *ptype, int *poffset, ActSimObj **pobj)
{
  ActSimObj *myobj;
  myobj = NULL;
  if (!id_to_siminfo_raw (s, ptype, poffset, &myobj)) {
    return 0;
  }
  if (!myobj) {
    return 0;
  }
  if (pobj) {
    *pobj = myobj;
  }
  
  *poffset = myobj->getGlobalOffset (*poffset, (*ptype == 3 ? 2 : *ptype));
  return 1;
}



int process_set (int argc, char **argv)
{
  if (argc != 3) {
    fprintf (stderr, "Usage: %s <name> <val>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;

  if (!id_to_siminfo_glob (argv[1], &type, &offset, NULL)) {
    return LISP_RET_ERROR;
  }

  if (type == 2 || type == 3) {
    printf ("'%s' is a channel; not currently supported!\n", argv[1]);
    return LISP_RET_ERROR;
  }

  int val;
  
  if (type == 0) {
    if (strcmp (argv[2], "0") == 0 || strcmp (argv[2], "#f") == 0) {
      val = 0;
    }
    else if (strcmp (argv[2], "1") == 0 || strcmp (argv[2], "#t") == 0) {
      val = 1;
    }
    else if (strcmp (argv[2], "X") == 0) {
      val = 2;
    }
    else {
      fprintf (stderr, "Boolean must be set to either 0, 1, or X\n");
      return LISP_RET_ERROR;
    }
    const ActSim::watchpt_bucket *nm;
    if ((nm = glob_sim->chkWatchPt (0, offset))) {
      int oval = glob_sim->getBool (offset);
      if (oval != val) {
	BigInt tm = SimDES::CurTime();
	printf ("[");
	tm.decPrint (stdout, 20);
	printf ("] <[env]> ");
	printf ("%s := %c\n", nm->s, (val == 2 ? 'X' : ((char)val + '0')));

	BigInt tmpv;
	tmpv = val;

	glob_sim->recordTrace (nm, type, ACT_CHAN_IDLE, tmpv);
      }
    }
    glob_sim->setBool (offset, val);
  }
  else if (type == 1) {
    BigInt *otmp = glob_sim->getInt (offset);
    val = atoi (argv[2]);
    if (val < 0) {
      fprintf (stderr, "Integers are unsigned.\n");
      return LISP_RET_ERROR;
    }

    BigInt x(64, 0, 0);
    x = val;
    x.setWidth (otmp->getWidth());

    const ActSim::watchpt_bucket *nm;
    if ((nm = glob_sim->chkWatchPt (1, offset))) {
      if (*otmp != x) {
	BigInt tm = SimDES::CurTime();
	printf ("[");
	tm.decPrint (stdout, 20);
	printf ("] <[env]> ");
	printf ("%s := ", nm->s);
	x.decPrint (stdout);
	printf (" (0x");
	x.hexPrint (stdout);
	printf (")\n");

	glob_sim->recordTrace (nm, type, ACT_CHAN_IDLE, x);
      }
    }
    glob_sim->setInt (offset, x);
  }
  else {
    fatal_error ("Should not be here");
  }

  SimDES **arr;
  arr = glob_sim->getFO (offset, type);
  for (int i=0; i < glob_sim->numFanout (offset, type); i++) {
    ActSimDES *p = dynamic_cast <ActSimDES *> (arr[i]);
    Assert (p, "Hmm?");
    p->propagate ();
  }
  return LISP_RET_TRUE;
}

int process_wakeup (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <name>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  ActId *id = my_parse_id (argv[1]);
  ActId *orig = id;
  if (!id) {
    fprintf (stderr, "%s: could not parse `%s' into an identifier",
	     argv[0], argv[1]);
    return LISP_RET_ERROR;
  }

  ActSimObj *obj = find_object (&id, glob_sim->getInstTable());
  if (!obj) {
    fprintf (stderr, "%s: could not find instance `%s'\n", argv[0], argv[1]);
    delete orig;
    return LISP_RET_ERROR;
  }

  if (id) {
    fprintf (stderr, "%s: please specify process name\n", argv[0]);
    delete orig;
    return LISP_RET_ERROR;
  }
  delete orig;

  ChpSim *chp = dynamic_cast<ChpSim *>(obj);
  if (!chp) {
    fprintf (stderr, "%s: only supported for CHP/HSE components\n", argv[0]);
    return LISP_RET_ERROR;
  }
  chp->awakenDeadlockedGC ();
  return LISP_RET_TRUE;
}

int process_skipcomm (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <name>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;

  if (!id_to_siminfo_glob_raw (argv[1], &type, &offset, NULL)) {
    return LISP_RET_ERROR;
  }

  if (!(type == 2 || type == 3)) {
    printf ("%s: '%s' is a not a channel!\n", argv[0], argv[1]);
    return LISP_RET_ERROR;
  }

  ActId *id = my_parse_id (argv[1]);
  ActId *orig = id;
  if (!id) {
    fprintf (stderr, "%s: could not parse `%s' into an identifier",
	     argv[0], argv[1]);
    return LISP_RET_ERROR;
  }

  ActSimObj *obj = find_object (&id, glob_sim->getInstTable());
  if (!obj) {
    fprintf (stderr, "%s: could not find instance `%s'\n", argv[0], argv[1]);
    delete orig;
    return LISP_RET_ERROR;
  }
  delete orig;

  ChpSim *chp = dynamic_cast<ChpSim *>(obj);
  if (!chp) {
    fprintf (stderr, "%s: only supported for CHP/HSE components\n", argv[0]);
    return LISP_RET_ERROR;
  }
  
  act_channel_state *c = glob_sim->getChan (offset);
  if (WAITING_SENDER (c)) {
    if (type != 3) {
      printf ("%s: state is waiting-send; use this command for the sending process.\n", argv[1]);
      return LISP_RET_ERROR;
    }
    chp->skipChannelAction (1, offset);
  }
  else if (WAITING_RECEIVER (c)) {
    if (type != 2) {
      printf ("%s: state is waiting-recv; use this command for the receiving process.\n", argv[1]);
      return LISP_RET_ERROR;
    }
    chp->skipChannelAction (0, offset);
  }
  else {
    printf ("%s: channel `%s' is not in a state where it can be skipped.\n",
	    argv[0], argv[1]);
    return LISP_RET_ERROR;
  }
  
  return LISP_RET_TRUE;
}

int process_get (int argc, char **argv)
{
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <name> [#f]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;

  if (!id_to_siminfo_glob (argv[1], &type, &offset, NULL)) {
    return LISP_RET_ERROR;
  }

  unsigned long val;
  if (type == 0) {
    val = glob_sim->getBool (offset);
    LispSetReturnInt (val);
    if (argc == 2) {
      if (val == 0) {
	printf ("%s: 0\n", argv[1]);
      }
      else if (val == 1) {
	printf ("%s: 1\n", argv[1]);
      }
      else {
	printf ("%s: X\n", argv[1]);
      }
    }
  }
  else if (type == 1) {
    BigInt *ival = glob_sim->getInt (offset);
    val = ival->getVal (0);
    LispSetReturnInt (val);
    if (argc == 2) {
      printf ("%s: %lu  (0x%lx)\n", argv[1], val, val);
    }
  }
  else {
    act_channel_state *c = glob_sim->getChan (offset);
    if (WAITING_SENDER (c)) {
      printf ("%s: waiting sender\n", argv[1]);
      LispSetReturnInt(1);
    }
    else if (WAITING_SEND_PROBE (c)) {
      printf ("%s: waiting sender probe\n", argv[1]);
      LispSetReturnInt(2);
    }
    else if (WAITING_RECEIVER(c)) {
      printf ("%s: waiting receiver\n", argv[1]);
      LispSetReturnInt(3);
    }
    else if (WAITING_RECV_PROBE(c)) {
      printf ("%s: waiting receiver probe\n", argv[1]);
      LispSetReturnInt(4);
    }
    else {
      printf ("%s: idle\n", argv[1]);
      LispSetReturnInt(0);
    }
  }
  return LISP_RET_INT;
}

int process_mget (int argc, char **argv)
{
  if (argc < 2) {
    fprintf (stderr, "Usage: %s <name1> <name2> ...\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;

  for (int i=1; i < argc; i++) {
    if (!id_to_siminfo_glob (argv[i], &type, &offset, NULL)) {
      return LISP_RET_ERROR;
    }

    if (type == 2 || type == 3) {
      printf ("'%s' is a channel; not currently supported!\n", argv[i]);
      return LISP_RET_ERROR;
    }

    unsigned long val;
    if (type == 0) {
      val = glob_sim->getBool (offset);
      if (val == 0) {
	printf ("%s: 0\n", argv[i]);
      }
      else if (val == 1) {
	printf ("%s: 1\n", argv[i]);
      }
      else {
	printf ("%s: X\n", argv[i]);
      }
    }
    else if (type == 1) {
      BigInt *ival;
      ival = glob_sim->getInt (offset);
      printf ("%s: %lu  (0x%lx)\n", argv[i], ival->getVal(0), ival->getVal (0));
    }
  }
  return LISP_RET_TRUE;
}

int process_watch (int argc, char **argv)
{
  if (argc < 2) {
    fprintf (stderr, "Usage: %s <n1> <n2> ...\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;
  ActSimObj *obj;

  for (int i=1; i < argc; i++) {
    if (!id_to_siminfo (argv[i], &type, &offset, &obj)) {
      return LISP_RET_ERROR;
    } 
    obj->addWatchPoint (type, offset, argv[i]);
  }

  return LISP_RET_TRUE;
}

int process_breakpt (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <name>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;
  ActSimObj *obj;

  if (!id_to_siminfo (argv[1], &type, &offset, &obj)) {
    return LISP_RET_ERROR;
  }

  obj->toggleBreakPt (type, offset, argv[1]);

  return LISP_RET_TRUE;
}

int process_unwatch (int argc, char **argv)
{
  if (argc < 2) {
    fprintf (stderr, "Usage: %s <n1> <n2> ...\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;
  ActSimObj *obj;

  for (int i=1; i < argc; i++) {
    if (!id_to_siminfo (argv[i], &type, &offset, &obj)) {
      return LISP_RET_ERROR;
    }
    obj->delWatchPoint (type, offset);
  }

  return LISP_RET_TRUE;
}

int process_chcount (int argc, char **argv)
{
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <ch> [#f]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;
  ActSimObj *obj;

  if (!id_to_siminfo (argv[1], &type, &offset, &obj)) {
    return LISP_RET_ERROR;
  }
  if (type != 2 && type != 3) {
    fprintf (stderr, "%s: is not of channel type\n", argv[1]);
    return LISP_RET_ERROR;
  }
  int goff = obj->getGlobalOffset (offset, type);
  act_channel_state *ch = glob_sim->getChan (goff);
  if (argc != 3) {
    printf ("Channel %s: completed actions %lu\n", argv[1], ch->count);
  }
  LispSetReturnInt (ch->count);
  return LISP_RET_INT;
}

int process_logfile (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <file>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  actsim_close_log ();
  
  FILE *fp = fopen (argv[1], "w");
  if (!fp) {
    fprintf (stderr, "%s: could not open file `%s'\n", argv[0], argv[1]);
    return LISP_RET_ERROR;
  }
  actsim_set_log (fp);
  return LISP_RET_TRUE;
}

int process_filter (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <regexp>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  glob_sim->logFilter (argv[1]);

  return LISP_RET_TRUE;
}

int process_error (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <str>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  fprintf (stderr, "ERROR: %s\n", argv[1]);

  return LISP_RET_ERROR;
}

int process_echo (int argc, char **argv)
{
  int nl = 1;
  if (argc > 1 && strcmp (argv[1], "-n") == 0) {
      nl = 0;
  }
  for (int i=2-nl; i < argc; i++) {
    printf ("%s", argv[i]);
    if (i != argc-1) {
      printf (" ");
    }
  }
  if (nl) {
    printf ("\n");
  }
  return LISP_RET_TRUE;
}

int process_mode (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s reset|run\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (strcmp (argv[1], "reset") == 0) {
    glob_sim->setMode (1);
  }
  else if (strcmp (argv[1], "run") == 0) {
    glob_sim->setMode (0);
  }
  else {
    fprintf (stderr, "%s: unknown mode\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_random (int argc, char **argv)
{
  if (argc == 1) {
    glob_sim->setRandom();
  }
  else if (argc == 3) {
    glob_sim->setRandom (atoi (argv[1]), atoi(argv[2]));
  }
  else {
    fprintf (stderr, "Usage: %s [min max]\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_norandom (int argc, char **argv)
{
  if (argc == 1) {
    glob_sim->setNoRandom();
  }
  else {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_random_seed (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <val>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setRandomSeed (atoi (argv[1]));
  return LISP_RET_TRUE;
}

int process_random_choice (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s on|off\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (strcmp (argv[1], "on") == 0) {
    glob_sim->setRandomChoice (1);
  }
  else if (strcmp (argv[1], "off") == 0) {
    glob_sim->setRandomChoice (0);
  }
  else {
    fprintf (stderr, "Usage: %s on|off\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_break_on_warn (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setWarning (1);
  return LISP_RET_TRUE;
}

int process_exit_on_warn (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setWarning (2);
  return LISP_RET_TRUE;
}

int process_resume_on_warn (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setWarning (0);
  return LISP_RET_TRUE;
}

static void _compute_status (ActInstTable *tab, int val)
{
  if (tab->obj) {
    tab->obj->printStatus (val);
  }
  if (tab->H) {
    hash_iter_t it;
    hash_bucket_t *b;
    hash_iter_init (tab->H, &it);
    while ((b = hash_iter_next (tab->H, &it))) {
      _compute_status ((ActInstTable *)b->v, val);
    }
  }
}

int process_status (int argc, char **argv)
{
  int val;
  
  if (argc != 2) {
    fprintf (stderr, "Usage: %s 0|1|X\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (strcmp (argv[1], "0") == 0) {
    val = 0;
  }
  else if (strcmp (argv[1], "1") == 0) {
    val = 1;
  }
  else if (strcmp (argv[1], "X") == 0 || strcmp (argv[1], "U") == 0) {
    val = 2;
  }
  else {
    fprintf (stderr, "Usage: %s 0|1|X\n", argv[0]);
    return LISP_RET_ERROR;
  }
  _compute_status (glob_sim->getInstTable(), val);

  /* now dump status for all the primary I/O pins and globals */
  if (glob_sim->getInstTable()->obj) {
    glob_sim->getInstTable()->obj->printStatus (val, true);
  }
  
  return LISP_RET_TRUE;
}

int process_create_generic_trace (const char *cmd,
				  const char *file,
				  const char *trname,
				  const char *msg)
{
  int idx;
  
  idx = glob_sim->useOrAllocTrIndex (trname);
  if (idx == -1) {
    fprintf (stderr, "%s: could not load %s trace file library\n", cmd, msg);
    return LISP_RET_ERROR;
  }

  if (glob_sim->getTrace (idx)) {
    fprintf (stderr, "%s: closing current %s file\n", cmd, msg);
    glob_sim->initTrace (idx, NULL);
  }

  if (!glob_sim->initTrace (idx, file)) {
    return LISP_RET_ERROR;
  }
  
  return LISP_RET_TRUE;
}

int process_stop_generic_trace (const char *cmd,
				const char *trname,
				const char *msg)
{
  int idx;

  idx = glob_sim->trIndex (trname);
  if (idx == -1) {
    fprintf (stderr, "%s: could not find %s trace file library\n", cmd, msg);
    return LISP_RET_ERROR;
  }
  
  if (glob_sim->getTrace (idx)) {
    glob_sim->initTrace (idx, NULL);
  }
  else {
    fprintf (stderr, "%s: no current %s file.\n", cmd, msg);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}


int process_createvcd (int argc, char **argv)
{
  int idx;
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <file>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return process_create_generic_trace (argv[0], argv[1], "vcd", "VCD");
}

int process_stopvcd (int argc, char **argv)
{
  int idx;
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return process_stop_generic_trace (argv[0], "vcd", "VCD");
}

static char *tf_name (const char *s)
{
  char *ret;
  if (strcmp (s, "atr") == 0) {
    ret = Strdup ("ATRACE");
  }
  else {
    ret = Strdup (s);
    for (int i=0; ret[i]; i++) {
      ret[i] = toupper(ret[i]);
    }
  }
  return ret;
}

int process_createalint (int argc, char **argv)
{
  int idx;
  const char *fmt;
  char *tmpbuf;
  char *file;
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s [-fmt] <file>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (argc == 3) {
    if (argv[1][0] != '-') {
      fprintf (stderr, "Usage: %s [-fmt] <file>\n", argv[0]);
      return LISP_RET_ERROR;
    }
    fmt = argv[1]+1;
    file = argv[2];
  }
  else {
    fmt = "atr";
    file = argv[1];
  }
  tmpbuf = tf_name (fmt);  
  int ret = process_create_generic_trace (argv[0], file, fmt, tmpbuf);
  FREE (tmpbuf);
  return ret;
}

int process_stopalint (int argc, char **argv)
{
  char *tmpbuf;
  const char *fmt;
  
  if (argc != 1 && argc != 2) {
    fprintf (stderr, "Usage: [-fmt] %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (argc == 2) {
    if (argv[1][0] != '-') {
      fprintf (stderr, "Usage: %s [-fmt]\n", argv[0]);
      return LISP_RET_ERROR;
    }
    fmt = argv[1]+1;
  }
  else {
    fmt = "atr";
  }
  tmpbuf = tf_name (fmt);

  if (glob_sim->trIndex (fmt) == -1) {
    fprintf (stderr, "%s: no format `%s' exists", argv[0], fmt);
    FREE (tmpbuf);
    return LISP_RET_ERROR;
  }

  int ret = process_stop_generic_trace (argv[0], fmt, tmpbuf);
  FREE (tmpbuf);
  return ret;
}

int process_createlxt2 (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <file>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return process_create_generic_trace (argv[0], argv[1], "lxt2", "LXT2");
}

int process_stoplxt2 (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return process_stop_generic_trace (argv[0], "lxt2", "LXT2");
}

int process_timescale (int argc, char **argv)
{
  double tm;
  
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <t>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  tm = atof (argv[1]);
  if (tm <= 0) {
    fprintf (stderr, "Timescale value has to be positive!");
    return LISP_RET_ERROR;
  }
  glob_sim->setTimescale (tm);
  return LISP_RET_TRUE;
}

int process_get_sim_time (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s <t>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (!glob_sim) { 
    fprintf (stderr, "%s: No simulation?\n", argv[0]);
    return LISP_RET_ERROR;
  }
  double cur_time = glob_sim->curTimeMetricUnits ();
  LispSetReturnFloat (cur_time*1e12); 
  return LISP_RET_FLOAT;
}

int process_get_sim_itime (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s <t>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (!glob_sim) { 
    fprintf (stderr, "%s: No simulation?\n", argv[0]);
    return LISP_RET_ERROR;
  }
  BigInt tm = SimDES::CurTime();
  LispSetReturnInt (tm.getVal (0));
  return LISP_RET_INT;
}


struct LispCliCommand Cmds[] = {
  { NULL, "Initialization and setup", NULL },

  { "echo", "[-n] args - display to screen", process_echo },
  { "error", "<str> - report error and abort execution", process_error },
  
  { "initialize", "<proc> - initialize simulation for <proc>",
    process_initialize },

  { "mode", "reset|run - set running mode", process_mode },
  
  { "random", "[min max] - randomize timings", process_random },
  { "random_seed", "<val> - set random number seed", process_random_seed },
  { "norandom", "- deterministic timing", process_norandom },
  { "random_choice", "on|off - randomize non-deterministic choices", process_random_choice },

#if 0
  { "dumptc", "<file> - dump transition counts to a file", process_dumptc },
#endif

  { NULL, "Running simulation", NULL },

  { "step", "[n] - run the next [n] events", process_step },
  { "advance", "<delay> - run for <delay> time", process_advance },
  { "cycle", "- run until simulation stops", process_cycle },

  { "set", "<name> <val> - set a variable to a value", process_set },
  { "gc-retry", "<name> - re-try guards in a deadlocked process", process_wakeup },
  { "skip-comm", "<name> - skip the communication action", process_skipcomm },
  
  { "get", "<name> [#f] - get value of a variable; optional arg turns off display", process_get },
  { "mget", "<name1> <name2> ... - multi-get value of a variable", process_mget },
  { "chcount", "<name> [#f] - return the number of completed actions on named channel", process_chcount },

  { "watch", "<n1> <n2> ... - add watchpoint for <n1> etc.", process_watch },
  { "unwatch", "<n1> <n2> ... - delete watchpoint for <n1> etc.", process_unwatch },
  { "breakpt", "<n> - toggle breakpoint for <n>", process_breakpt },
  { "break", "<n> - toggle breakpoint for <n>", process_breakpt },

  { "break-on-warn", "- stop simulation on warning", process_break_on_warn },
  { "exit-on-warn", "- like break-on-warn, but exit", process_exit_on_warn },
  { "resume-on-warn", "- continue simulation on warning", process_resume_on_warn },

  { "status", "0|1|X - list all nodes with specified value", process_status },

  { "timescale", "<t> - set time scale to <t> picoseconds for tracing", process_timescale },
  { "get_sim_time", "- returns current simulation time in picoseconds", process_get_sim_time },
  { "get_sim_itime", "- returns current simulation time (integer)", process_get_sim_itime },
  { "vcd_start", "<file> - Create Verilog change dump for all watched values", process_createvcd },
  { "vcd_stop", "- Stop VCD generation", process_stopvcd },
  { "trace_start", "[-fmt] <file> - Create trace file in specified format for all watched values", process_createalint },
  { "trace_stop", "[-fmt] - Stop trace file generation for specified format", process_stopalint },
  { "lxt2_start", "<file> - Create LXT2 format trace file for all watched values", process_createlxt2 },
  { "lxt2_stop", "- Stop LXT2 trace file generation", process_stoplxt2 },

#if 0  
  { "pending", "- dump pending events", process_pending },

  { NULL, "Production rule tracing", NULL },

  { "fanin", "<n> - list fanin for <n>", process_fanin },
  { "fanin-get", "<n> - list fanin with values for <n>", process_fanin_get },
  { "fanout", "<n> - list fanout for <n>", process_fanout },
  
#endif  

  { NULL, "Process and CHP commands", NULL },

#if 0
  { "send", "<chan> <val> - send a value on a channel", process_send },
  { "recv", "<chan> [#f] - receive a value from a channel; optional arg turns off display", process_recv },
#endif

  { "filter", "<regexp> - only show log messages that match regexp", process_filter },
  { "logfile", "<file> - dump actsim log output to a log file <file>", process_logfile },
  
  { "procinfo", "<filename> [<inst-name>] - save the program counter for a process to file (- for stdout)", process_procinfo },
  { "energy", "<filename> [<inst-name>] - save energy usage to file (- for stdout)", process_getenergy },
  { "coverage", "<filename> [<inst-name>] - report coverage for guards", process_coverage },
  { "goto", "[<inst-name>] <label> - for a single-threaded state, jump to label", process_goto }
};

/* -- access top-level Act  -- */
Act *actsim_Act()
{
  return glob_act;
}

Process *actsim_top()
{
  return glob_top;
}

int debug_metrics;

int main (int argc, char **argv)
{
  const char *metrics_tech_name;
  
  config_set_default_int ("sim.chp.default_delay", 10);
  config_set_default_int ("sim.chp.default_energy", 0);
  config_set_default_real ("sim.chp.default_leakage", 0);
  config_set_default_int ("sim.chp.default_area", 0);
  config_set_default_int ("sim.chp.debug_metrics", 0);
  config_set_int ("net.emit_parasitics", 1);

  /* initialize ACT library */
  list_t *l = list_new ();
  list_append (l, "actsim.conf");
  list_append (l, "lint.conf");

  Act::Init (&argc, &argv, l);
  list_free (l);

  debug_metrics = config_get_int ("sim.chp.debug_metrics");

  /* some usage check */
  if (argc != 3) {
    usage (argv[0]);
  }

  if (config_exists ("sim.chp.metrics_tech_name")) {
    metrics_tech_name = config_get_string ("sim.chp.metrics_tech_name");
    if (strcmp (metrics_tech_name, getenv ("ACT_TECH")) != 0) {
      fprintf (stderr, "Simulator tech: `%s'; metrics conf file for: `%s'\n",
	       getenv ("ACT_TECH"), metrics_tech_name);
      fatal_error ("Simulator technology specified does not match config-specified metrics");
    }
  }

  /* read in the ACT file */
  glob_act = new Act (argv[1]);

  /* expand it */
  glob_act->Expand ();

  /* map to cells: these get characterized */
  //ActCellPass *cp = new ActCellPass (a);
  //cp->run();
 
  /* find the process specified on the command line */
  Process *p = glob_act->findProcess (argv[2], true);

  if (!p) {
    fatal_error ("Could not find process `%s' in file `%s'", argv[2], argv[1]);
  }

  if (!p->isExpanded()) {
    p = p->Expand (ActNamespace::Global(), p->CurScope(), 0, NULL);
  }

  if (!p->isExpanded()) {
    fatal_error ("Process `%s' is not expanded.", argv[2]);
  }

  glob_top = p;

  /* do stuff here */
  glob_sp = new ActStatePass (glob_act);
  glob_sp->run (p);
  
  glob_sim = new ActSim (p);
  glob_sim->runInit ();
  ActExclConstraint::_sc = glob_sim;

  signal (SIGINT, signal_handler);

  LispInit ();
  LispCliInit (NULL, ".actsim_history", "actsim> ", Cmds,
	       sizeof (Cmds)/sizeof (Cmds[0]));

  while (!LispCliRun (stdin)) {
    if (LispInterruptExecution) {
      fprintf (stderr, " *** interrupted\n");
    }
    clr_interrupt ();
  }

  LispCliEnd ();
  
  delete glob_sim;

  return 0;
}
