/*************************************************************************
 *
 *  Copyright (c) 2022 Rajit Manohar
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

#include <common/array.h>
#include <stdio.h>

#include "../../actsim_ext.h"

/* use local copy from glibc for platform-independent generation */
extern "C" int local_rand_r(unsigned int* seed);

struct random_state {
    unsigned seed;
    int bitwidth;
    unsigned int min, max;
};

L_A_DECL(struct random_state, _rstate);

#define CHECK_NUM_ARGS(s, n)                                                  \
    do {                                                                      \
        if (argc != (n)) {                                                    \
            fprintf(stderr, s ": needs exactly %d argument%s, got %d\n", (n), \
                    ((n) == 1 ? "" : "s"), argc);                             \
            return ret;                                                       \
        }                                                                     \
    } while (0)

#define CHECK_RAND_IDX(s, n)                                               \
    do {                                                                   \
        if ((n) >= A_LEN(_rstate)) {                                       \
            fprintf(stderr, s ": random index %lu out of range; max=%d\n", \
                    (n), A_LEN(_rstate) - 1);                              \
            return ret;                                                    \
        }                                                                  \
    } while (0)

extern "C" expr_res actsim_rand_init(int argc, struct expr_res* args) {
    struct random_state* r;
    expr_res ret;
    ret.width = 32;
    ret.v = 0;

    CHECK_NUM_ARGS("actsim_rand_init", 1);

    if (args[0].v > 64) {
        fprintf(stderr, "act_rand_init: max bitwidth is 64, got %lu\n",
                args[0].v);
        return ret;
    }

    A_NEW(_rstate, struct random_state);
    r = &A_NEXT(_rstate);

    r->seed = A_LEN(_rstate);
    r->bitwidth = args[0].v;
    r->min = 0;
    r->max = 0;

    A_INC(_rstate);

    ret.v = A_LEN(_rstate) - 1;
    return ret;
}

extern "C" expr_res actsim_rand_get(int argc, struct expr_res* args) {
    struct random_state* r;
    expr_res ret;
    ret.width = 64;
    ret.v = 0;

    CHECK_NUM_ARGS("actsim_rand_get", 1);
    CHECK_RAND_IDX("actsim_rand_get", args[0].v);

    r = &_rstate[args[0].v];

    ret.width = r->bitwidth;
    ret.v = local_rand_r(&r->seed);
    if (r->min != 0 && r->max != 0) {
        if (r->min == r->max) {
            ret.v = r->min;
        } else {
            ret.v = r->min + (ret.v % (r->max - r->min + 1));
        }
    }
    if (ret.width < 64) {
        ret.v = ret.v & ((1 << ret.width) - 1);
    }
    ret.width = 64;
    return ret;
}

extern "C" expr_res actsim_rand_seed(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;

    CHECK_NUM_ARGS("actsim_rand_seed", 2);
    CHECK_RAND_IDX("actsim_rand_seed", args[0].v);

    _rstate[args[0].v].seed = args[1].v;

    ret.v = 1;
    return ret;
}

extern "C" expr_res actsim_rand_init_range(int argc, struct expr_res* args) {
    struct random_state* r;
    expr_res ret;
    ret.width = 32;
    ret.v = 0;

    CHECK_NUM_ARGS("actsim_rand_init_range", 3);

    if (args[0].v > 64) {
        fprintf(stderr, "actsim_rand_init_range: max bitwidth is 64, got %lu\n",
                args[0].v);
        return ret;
    }

    if (args[1].v > args[2].v) {
        fprintf(stderr, "actsim_rand_init_range: min (%lu) > max (%lu)\n",
                args[1].v, args[2].v);
        return ret;
    }

    A_NEW(_rstate, struct random_state);
    r = &A_NEXT(_rstate);

    r->seed = A_LEN(_rstate);
    r->bitwidth = args[0].v;
    r->min = args[1].v;
    r->max = args[2].v;

    A_INC(_rstate);

    ret.v = A_LEN(_rstate) - 1;
    return ret;
}
