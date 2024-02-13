/*************************************************************************
 *
 *  Copyright (c) 2024 Fabian Posch
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
#include <common/config.h>
#include <common/misc.h>
#include <stdio.h>

#include "../actsim_ext.h"

L_A_DECL(FILE*, file_fp);
L_A_DECL(FILE*, file_outfp);

/*
 * On some systems, even though we are
 * linking against the same shared library,
 * the state of the config file is replicated.
 * If this function is defined, ACT will automatically
 * call this function to synchronize the state.
 */
void _builtin_update_config(void* v) { config_set_state((struct Hashtable*)v); }

expr_res actsim_file_read(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 64;
    ret.v = 0;
    if (argc != 1) {
        fprintf(stderr, "actim_file_read: should have 1 argument only\n");
        return ret;
    }

    if (args[0].v > 4000) {
        fprintf(stderr, "actsim_file_read: more than 4000 files?!\n");
        return ret;
    }

    while (args[0].v >= A_LEN(file_fp)) {
        A_NEW(file_fp, FILE*);
        A_NEXT(file_fp) = NULL;
        A_INC(file_fp);
    }

    if (!file_fp[args[0].v]) {
        char* buf;
        int release = 0;
        if (config_exists("sim.file.name_table")) {
            int len = config_get_table_size("sim.file.name_table");
            if (args[0].v >= len) {
                fprintf(stderr,
                        "File name index %d is out of bounds given the name "
                        "table length of %d\n",
                        (int)args[0].v, len);
                return ret;
            }
            buf = (config_get_table_string("sim.file.name_table"))[args[0].v];
        } else {
            char* prefix = config_get_string("sim.file.prefix");
            MALLOC(buf, char, strlen(prefix) + 10);
            snprintf(buf, strlen(prefix) + 10, "%s.%d", prefix, (int)args[0].v);
            release = 1;
        }

        file_fp[args[0].v] = fopen(buf, "r");

        if (!file_fp[args[0].v]) {
            fprintf(stderr, "Could not open file `%s' for reading.\n", buf);
            if (release) {
                FREE(buf);
            }
            return ret;
        }

        if (release) FREE(buf);
    }

    if (file_fp[args[0].v]) {
        if (fscanf(file_fp[args[0].v], "%lx ", &ret.v) != 1) {
            ret.v = 0;
        }
    }
    return ret;
}

expr_res actsim_file_create(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;

    if (argc != 1) {
        fprintf(stderr, "actim_file_create: should have 1 argument only\n");
        return ret;
    }

    if (args[0].v > 4000) {
        fprintf(stderr, "actsim_file_create: more than 4000 files?!\n");
        return ret;
    }

    while (args[0].v >= A_LEN(file_outfp)) {
        A_NEW(file_outfp, FILE*);
        A_NEXT(file_outfp) = NULL;
        A_INC(file_outfp);
    }

    if (!file_outfp[args[0].v]) {
        char* buf;
        int release = 0;
        if (config_exists("sim.file.outname_table")) {
            int len = config_get_table_size("sim.file.outname_table");

            if (args[0].v >= len) {
                fprintf(stderr,
                        "File name index %d is out of bounds given the outname "
                        "table length of %d\n",
                        (int)args[0].v, len);
                return ret;
            }

            buf =
                (config_get_table_string("sim.file.outname_table"))[args[0].v];

        } else {
            char* prefix = config_get_string("sim.file.outprefix");
            MALLOC(buf, char, strlen(prefix) + 10);
            snprintf(buf, strlen(prefix) + 10, "%s.%d", prefix, (int)args[0].v);
            release = 1;
        }

        file_outfp[args[0].v] = fopen(buf, "w");

        if (!file_outfp[args[0].v]) {
            fprintf(stderr, "Could not open file `%s' for writing.\n", buf);
            if (release) {
                FREE(buf);
            }
            return ret;
        }

        if (release) FREE(buf);

    } else {
        fprintf(stderr, "actsim_file_create: already created file!\n");
        return ret;
    }

    ret.v = 1;
    return ret;
}

expr_res actsim_file_write(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;

    if (argc != 2) {
        fprintf(stderr, "actim_file_write: should have 2 arguments\n");
        return ret;
    }

    if (args[0].v >= A_LEN(file_outfp)) {
        fprintf(stderr, "Illegal file index %d\n", (int)args[0].v);
        return ret;
    }

    if (!file_outfp[args[0].v]) {
        fprintf(stderr, "File index %d is closed.\n", (int)args[0].v);
        return ret;
    }
    fprintf(file_outfp[args[0].v], "%lx\n", args[1].v);
    fflush(file_outfp[args[0].v]);
    ret.v = 1;
    return ret;
}

expr_res actsim_file_closew(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;
    if (argc != 1) {
        fprintf(stderr, "actsim_file_closew: should have 1 argument only\n");
        return ret;
    }
    if (args[0].v >= A_LEN(file_outfp)) {
        fprintf(stderr, "actsim_file_closew: invalid ID %d!\n", (int)args[0].v);
        return ret;
    }
    if (file_outfp[args[0].v]) {
        fclose(file_outfp[args[0].v]);
        file_fp[args[0].v] = NULL;
        ret.v = 1;
    }
    return ret;
}

expr_res actsim_file_close(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;
    if (argc != 1) {
        fprintf(stderr, "actsim_file_close: should have 1 argument only\n");
        return ret;
    }
    if (args[0].v >= A_LEN(file_fp)) {
        fprintf(stderr, "actsim_file_close: invalid ID %d!\n", (int)args[0].v);
        return ret;
    }
    if (file_fp[args[0].v]) {
        fclose(file_fp[args[0].v]);
        file_fp[args[0].v] = NULL;
        ret.v = 1;
    }
    return ret;
}

expr_res actsim_file_eof(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;
    if (argc != 1) {
        fprintf(stderr, "actim_file_eof: should have 1 argument only\n");
        return ret;
    }

    if (args[0].v > 4000) {
        fprintf(stderr, "actsim_file_eof: more than 4000 files?!\n");
        return ret;
    }

    while (args[0].v >= A_LEN(file_fp)) {
        A_NEW(file_fp, FILE*);
        A_NEXT(file_fp) = NULL;
        A_INC(file_fp);
    }
    if (!file_fp[args[0].v]) {
        ret.v = 1;
    } else if (feof(file_fp[args[0].v])) {
        ret.v = 1;
    } else {
        ret.v = 0;
    }
    return ret;
}
