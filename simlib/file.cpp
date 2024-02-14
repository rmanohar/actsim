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

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../actsim_ext.h"

std::unordered_map<size_t, std::ifstream> input_files;
std::unordered_map<size_t, std::ofstream> output_files;
size_t reader_cnt = 0;

L_A_DECL(FILE*, file_outfp);

/*
 * On some systems, even though we are
 * linking against the same shared library,
 * the state of the config file is replicated.
 * If this function is defined, ACT will automatically
 * call this function to synchronize the state.
 */
extern void _builtin_update_config(void* v) {
    config_set_state((struct Hashtable*)v);
}

/**
 * @brief Read a line form an input file
 *
 * This method can be called from a CHP function block.
 * It requires one argument to be passed in args:
 * - args[0]: The ID of the reader calling
 *
 * This is not the file ID! This is am important difference,
 * since using file ID instead of reader ID make it ambiguous
 * who is calling - which means only one process can read from
 * a given file
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res Value read from the input file
 */
extern expr_res actsim_file_read(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 64;
    ret.v = 0;

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actim_file_read: Must be invoked with 1 argument only "
                     "(reader ID)"
                  << std::endl;
        return ret;
    }

    size_t reader_id = args[0].v;

    // check if we have the have the requested file already open
    if (input_files.find(reader_id) == input_files.end()) {
        std::cerr << "actim_file_read: Unknown reader ID, open a file first!"
                  << std::endl;
        return ret;
    }

    // read the value from the file
    unsigned long value;

    if (!(input_files[reader_id] >> value)) {
        std::cerr << "actim_file_read: Failed to read from file!" << std::endl;
        return ret;
    }

    ret.v = value;
    return ret;
}

/**
 * @brief Open a file for reading
 *
 * This method can be called from a CHP function block.
 * It requires one argument to be passed in args:
 * - args[0]: The ID of the file to open
 *
 * A CHP process wishing to read from a given input
 * file shall call this first to receive a reader ID
 * for a given file ID. This enables multiple access
 * for input files.
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res reader ID if file open successful; 0 otherwise
 */
extern expr_res actsim_file_openr(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 32;
    ret.v = 0;

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actim_file_openr: Must be invoked with 1 argument only "
                     "(file ID)"
                  << std::endl;
        return ret;
    }

    size_t file_id = args[0].v;

    // get a new reader ID
    // the first ID generated this way is 1, so we can use 0 as an error value
    ++reader_cnt;

    // make sure the reader doesn't already exist
    while (input_files.find(reader_cnt) != input_files.end()) ++reader_cnt;

    // get the file name to open
    std::string filename;

    // check if the config defines an alias for this file
    if (config_exists("sim.file.name_table")) {
        int len = config_get_table_size("sim.file.name_table");

        // make sure the configured alias is still in bounds
        if (args[0].v >= len) {
            std::cerr << "actim_file_openr: File name index " << file_id
                      << " is out of bounds given the name table length of "
                      << len << std::endl;
            return ret;
        }

        // and get the alias
        filename = (config_get_table_string("sim.file.name_table"))[file_id];

    } else {
        // seems like we don't, load the file normally using the file prefix
        std::ostringstream builder;
        builder << config_get_string("sim.file.prefix") << "." << file_id;
        filename = builder.str();
    }

    // now we finally open the file for reading
    std::ifstream input_file(filename);

    // make sure the file is actually open
    if (!input_file.is_open()) {
        std::cerr << "actim_file_openr: Could not open file '" << filename
                  << "' for reading." << std::endl;
        return ret;
    }

    // place the file stream in the input file list
    ret.v = reader_cnt;
    input_files.emplace(reader_cnt, std::move(input_file));

    return ret;
}

/**
 * @brief Check if we have reached EOF in an input file
 *
 * This method can be called from a CHP function block.
 * It requires one argument to be passed in args:
 * - args[0]: The ID of the reader calling
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res 0 if not at EOF of file, 1 otherwise (includes error)
 */
extern expr_res actsim_file_eof(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 1;  // if anything goes wrong, it's better the caller thinks we're
                // at EOF

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actim_file_eof: Must be invoked with 1 argument only "
                     "(reader ID)"
                  << std::endl;
        return ret;
    }

    size_t reader_id = args[0].v;

    // check if we have the have the requested file already open
    if (input_files.find(reader_id) == input_files.end()) {
        std::cerr << "actim_file_eof: Unknown reader ID, open a file first!"
                  << std::endl;
        return ret;
    }

    // read the value from the file
    unsigned long value;

    if (input_files[reader_id].eof()) {
        return ret;
    }

    // we're not at the end yet
    ret.v = 0;
    return ret;
}

/**
 * @brief Close a file reader
 *
 * This method can be called from a CHP function block.
 * It requires one argument to be passed in args:
 * - args[0]: The ID of the reader calling
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res 0 on success, 1 otherwise
 */
extern expr_res actsim_file_close(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 1;  // if there is any error, we return 1

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actsim_file_close: Must be invoked with 1 argument only "
                     "(reader ID)"
                  << std::endl;
        return ret;
    }

    size_t reader_id = args[0].v;

    // check if we have the have the requested file already open
    if (input_files.find(reader_id) == input_files.end()) {
        std::cerr << "actim_file_close: Unknown reader ID, open a file first!"
                  << std::endl;
        return ret;
    }

    // make sure the file isn't already closed
    if (!input_files[reader_id].is_open()) {
        std::cerr << "actim_file_close: File was already closed!" << std::endl;
        input_files.erase(reader_id);
        return ret;
    }

    // close file and erase the reader ID
    input_files[reader_id].close();
    input_files.erase(reader_id);
    ret.v = 0;

    return ret;
}

extern expr_res actsim_file_create(int argc, struct expr_res* args) {
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

extern expr_res actsim_file_write(int argc, struct expr_res* args) {
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

extern expr_res actsim_file_closew(int argc, struct expr_res* args) {
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
        // file_fp[args[0].v] = NULL;
        ret.v = 1;
    }
    return ret;
}
