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
#ifndef __ACTSIM_CHANNEL_H__
#define __ACTSIM_CHANNEL_H__

#include <act/act.h>

class ChanMethods;
class ChpSim;

struct act_channel_state {
  /* vinit : initializer for value */
  act_channel_state(expr_multires &vinit); // initialize all fields (constructor)
  ~act_channel_state();		// release storage (destructor)
  
  unsigned int send_here:16;	// if non-zero, this is the "pc" for
				// the sender to be used to wake-up
				// the sending process

  unsigned int sender_probe:1;	// 1 if the send_here wait is actually
				// due to a probe, and not a waiting
				// sender but a waiting probe
  
  unsigned int recv_here:16;	 // if non-zero, this is the "pc" for
				 // the receiver to be used to wake up
				 // the receiving process
  
  unsigned int receiver_probe:1; // receiver is probing and waiting as
				 // a result, but not blocked on a
				 // receive

  unsigned int fragmented:2;	// have to simulate this at a lower
				// level of abstraction since pieces
				// of the channel are accessed. bit0 =
				// input end is fragmented, bit1 =
				// output end is fragmented
  


  unsigned int sfrag_st:2;	// send/recv, send_up/recv_up, or
				// send_rest/recv_rest
  unsigned int rfrag_st:2;	// send/recv, send_up/recv_up, or
				// send_rest/recv_rest
  unsigned int frag_warn:1;	// warning for double frag
  unsigned int sufrag_st:8;	// micro-state within frag state
  unsigned int rufrag_st:8;	// micro-state within frag state

  unsigned int use_flavors:1;	// 1 if !+/!- are in use
  unsigned int send_flavor:1;	// state of send flavor
  unsigned int recv_flavor:1;	// state of recv flavor

  unsigned int skip_action:1;	// used for skip-comm
  

  struct iHashtable *fH;	// fragment hash table
  Channel *ct;			// channel type
  ActId *inst_id;		// instance
  ChanMethods *cm;		// fill in channel methods
  ChpSim *_dummy;

  int width;			// bitwidth
  int len;
  expr_multires data, data2;  	// data: used when the receiver is
				// waiting for sender; data2 used when
				// sender arrives before receive is posted.
  unsigned long count;          // number of completed channel actions
  WaitForOne *w;
  WaitForOne *probe;		// probe wake-up
};


/*
  Macros to inspect channel state
*/
#define WAITING_SENDER(c)  ((c)->send_here != 0 && (c)->sender_probe == 0)
#define WAITING_SEND_PROBE(c)  ((c)->send_here != 0 && (c)->sender_probe == 1)

#define WAITING_RECEIVER(c)  ((c)->recv_here != 0 && (c)->receiver_probe == 0)
#define WAITING_RECV_PROBE(c)  ((c)->recv_here != 0 && (c)->receiver_probe == 1)

/*
  1. var +/-
  2. self := e
  3. goto N
  4. sel (e) nextsel: x    [if guard is false goto x; if backward goto
                            then wait]
  5. skip

  - assignment statements translated as (1) or (2), skip as skip

  - s1 ; s2 or s1 , s2 -> map linear

  - [ g1 -> s1 ... [] gN -> sN ]  translated as

x1: sel (g1) next x2
    s1
    goto done
x2: sel (g2) next x3
    s2 
    goto done
...
xN: sel (gN) next x1    <--- backward
    sN
done: ...     

 - *[ ... ] loop

x1: sel (g1) next x2 
    ...

    goto x1
    sel (gN) next xN+1

    goto x1
xN+1: else [always true]
done: 

*/

enum channel_op_types {
      CHAN_OP_SKIP = 0,
      CHAN_OP_BOOL_T = 1,
      CHAN_OP_BOOL_F = 2,
      CHAN_OP_SELF = 3,
      CHAN_OP_GOTO = 4,
      CHAN_OP_SEL = 5,
      CHAN_OP_SELFACK = 6
};
      
struct one_chan_op {
  unsigned int type:3;
  union {
    ActId *var;			// used for bool
    struct {
      Expr *e;			// used for self := e and sel (e)
      int idx;			// used for sel (e) idx
    };
  };
};

struct chan_ops {
  A_DECL (one_chan_op, op);
};

class ChanMethods {
public:
  ChanMethods (Channel *ch);
  ~ChanMethods ();

  /* returns -1 when done, otherwise id for resuming */
  int runMethod (ActSimCore *sim, act_channel_state *ch, int idx, int from);
  int runProbe (ActSimCore *sim, act_channel_state *ch, int idx);
  
private:
  void _compile (int idx, act_chp_lang *hse);
  chan_ops _ops[ACT_NUM_STD_METHODS];
  Channel *_ch;
};


#endif /* __ACTSIM_CHANNEL_H__ */
