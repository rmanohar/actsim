
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

#include "../simlib_file.h"

#include <common/config.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../../actsim_ext.h"

std::unordered_map<size_t, std::pair<std::ifstream, size_t>> input_streams;
std::unordered_map<size_t, std::pair<std::ofstream, size_t>> output_streams;
std::map<size_t, size_t> input_files;
std::set<size_t> output_files;
size_t reader_cnt = 0;
size_t writer_cnt = 0;

/*
 * On some systems, even though we are
 * linking against the same shared library,
 * the state of the config file is replicated.
 * If this function is defined, ACT will automatically
 * call this function to synchronize the state.
 */
extern "C" void _builtin_update_config(void* v) {
    config_set_state((struct Hashtable*)v);
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
extern "C" expr_res actsim_file_openr(int argc, struct expr_res* args) {
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

    // get the file name to open
    std::string filename;

    // check if the config defines an alias for this file
    if (config_exists("sim.file.name_table")) {
        int len = config_get_table_size("sim.file.name_table");

        // make sure the configured alias is still in bounds
        if (file_id >= len) {
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

    // make sure the file isn't already open for writing
    if (output_files.find(file_id) != output_files.end()) {
        std::cerr << "actsim_file_openr: File '" << filename
                  << "' is already opened for writing!" << std::endl;
        return ret;
    }

    // register the file as open in reading mode
    input_files.emplace(file_id, 1);

    // get a new reader ID
    // the first ID generated this way is 1, so we can use 0 as an error value
    ++reader_cnt;

    // make sure the reader doesn't already exist
    while (input_streams.find(reader_cnt) != input_streams.end()) ++reader_cnt;

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
    input_streams.emplace(reader_cnt,
                          std::make_pair(std::move(input_file), file_id));

    return ret;
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
extern "C" expr_res actsim_file_read(int argc, struct expr_res* args) {
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

    // check if we have the have the requested file has already been opened
    if (input_streams.find(reader_id) == input_streams.end()) {
        std::cerr << "actim_file_read: Unknown reader ID, open a file first!"
                  << std::endl;
        return ret;
    }

    // make sure the file is still open
    if (!input_streams[reader_id].first.is_open()) {
        std::cerr << "actim_file_read: File index "
                  << input_streams[reader_id].second << " is closed."
                  << std::endl;
        return ret;
    }

    // read the value from the file
    unsigned long value;

    // read new line and skip if empty
    bool empty = false;
    bool end = false;
    std::string line;

    do {
        if (!std::getline(input_streams[reader_id].first, line)) {
            end = true;
            continue;
        }

        // remove comments from line
        line = line.substr(0, line.find("#"));

        // remove leading whitespaces
        line = line.substr(line.find_first_not_of("\t\n "), std::string::npos);

        // check if line empty
        if (line.empty()) {
            empty = true;
            continue;
        }

        size_t base = 10;

        // check if base 16
        if (line.rfind("0x", 0) == 0) {
            // remove the characters and set the base
            line = line.substr(2, std::string::npos);
            base = 16;
        }

        // check if base 2
        if (line.rfind("0b", 0) == 0) {
            // remove the characters and set the base
            line = line.substr(2, std::string::npos);
            base = 2;
        }

        // check if base 8
        if (line.rfind("0o", 0) == 0) {
            // remove the characters and set the base
            line = line.substr(2, std::string::npos);
            base = 8;
        }

        size_t end;

        // read into value
        try {
            value = std::stoul(line, &end, base);
        } catch (std::invalid_argument& e) {
            std::cerr << "actim_file_read: Could not convert line '" << line
                      << "' into unsigned long value of base " << base
                      << std::endl;
            value = 0;
        } catch (std::out_of_range& e) {
            std::cerr << "actim_file_read: Conversion of line '" << line
                      << "' into unsigned long value of base " << base
                      << " is out of range!" << std::endl;
            value = 0;
        }

        // make sure there wasn't some trailing garbage in the line
        auto remainder = line.substr(end, std::string::npos);
        if (!remainder.empty()) {
            std::cerr << "actim_file_read: There was more content left in the "
                         "line that was ignored. Remaining content: '"
                      << remainder << "'" << std::endl;
        }

    } while (empty && !end);

    if (end) {
        std::cerr << "actim_file_read: Reached end of input file while trying "
                     "to read line. Trailing non-value or newline?"
                  << std::endl;
    }

    ret.v = value;
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
extern "C" expr_res actsim_file_eof(int argc, struct expr_res* args) {
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
    if (input_streams.find(reader_id) == input_streams.end()) {
        std::cerr << "actim_file_eof: Unknown reader ID, open a file first!"
                  << std::endl;
        return ret;
    }

    // read the value from the file
    uint64_t value;

    if (input_streams[reader_id].first.eof()) {
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
 * @return expr_res 1 on success, 0 otherwise
 */
extern "C" expr_res actsim_file_closer(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;  // if there is any error, we return 0 (boolean like)

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actsim_file_closer: Must be invoked with 1 argument only"
                     "(reader ID)"
                  << std::endl;
        return ret;
    }

    size_t reader_id = args[0].v;

    // check if we have the have the requested file already open
    if (input_streams.find(reader_id) == input_streams.end()) {
        std::cerr << "actim_file_closer: Unknown reader ID, open a file first!"
                  << std::endl;
        return ret;
    }

    // make sure the file isn't already closed
    if (!input_streams[reader_id].first.is_open()) {
        std::cerr << "actim_file_closer: File was already closed!" << std::endl;
        input_streams.erase(reader_id);
        return ret;
    }

    auto file_id = input_streams[reader_id].second;

    // close file and erase the reader ID
    input_streams[reader_id].first.close();
    input_streams.erase(reader_id);
    ret.v = 1;

    // reduce the reference counter
    --input_files[file_id];

    // if the reference counter hit zero, remove it
    if (input_files[file_id] == 0) input_files.erase(file_id);

    return ret;
}

/**
 * @brief Open a file for writing
 *
 * This method can be called from a CHP function block.
 * It requires one argument to be passed in args:
 * - args[0]: The ID of the file to write to
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res writer ID if file open successful; 0 otherwise
 */
extern "C" expr_res actsim_file_openw(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 32;
    ret.v = 0;

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actim_file_openw: Must be invoked with 1 argument only"
                  << std::endl;
        return ret;
    }

    size_t file_id = args[0].v;
    std::string filename;

    // construct the file name
    if (config_exists("sim.file.outname_table")) {
        int len = config_get_table_size("sim.file.outname_table");

        if (file_id >= len) {
            std::cerr << "actim_file_openw: File name index " << file_id
                      << " is out of bounds given the outname table length of "
                      << len << std::endl;
            return ret;
        }

        filename = (config_get_table_string("sim.file.outname_table"))[file_id];

    } else {
        std::ostringstream builder;
        builder << config_get_string("sim.file.outprefix") << "." << file_id;
        filename = builder.str();
    }

    // make sure the file isn't already open for reading
    if (input_files.find(file_id) != input_files.end()) {
        std::cerr << "actsim_file_openw: File '" << filename
                  << "' is already opened for reading!" << std::endl;
        return ret;
    }

    // make sure the file isn't already open for writing
    if (output_files.find(file_id) != output_files.end()) {
        std::cerr << "actsim_file_openw: File '" << filename
                  << "' is already opened for writing!" << std::endl;
        return ret;
    }

    // register the file as open for writing
    output_files.emplace(file_id);

    // increase the writer id counter
    ++writer_cnt;

    // finally, open the file
    std::ofstream output_file(filename);

    // make sure the file is actually open
    if (!output_file.is_open()) {
        std::cerr << "actim_file_openw: Could not open file '" << filename
                  << "' for writing." << std::endl;
        return ret;
    }

    // store the output stream
    output_streams.emplace(writer_cnt,
                           std::make_pair(std::move(output_file), file_id));

    ret.v = writer_cnt;
    return ret;
}

/**
 * @brief Write a value to a file
 *
 * This method can be called from a CHP function block.
 * It requires two arguments to be passed in args:
 * - args[0]: The ID of the writer calling
 * - args[1]: The value to write
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res 1 on success, 0 otherwise
 */
extern "C" expr_res actsim_file_write(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 9;  // on error we return 0 (boolean like)

    // make sure we have the appropriate amount of arguments
    if (argc != 2) {
        std::cerr << "actim_file_write: Must be invoked with 2 arguments only"
                  << std::endl;
        return ret;
    }

    size_t writer_id = args[0].v;

    // build the log line
    std::ostringstream builder;
    builder << args[1].v << std::endl;

    ret.v = actsim_file_write_core(writer_id, builder.str());
    return ret;
}

/**
 * @brief Write a string to a file
 *
 * This function cannot be called from CHP directly. It is meant
 * as a library function, so other components can implement functions
 * which construct more intricate output than just a value + new line.
 *
 * This is necessary for now since the ACT does not feature string handling
 * at the moment. This is planned for the future, but not implemented at
 * the moment.
 *
 * @param writer_id ID of the writer accessing the file
 * @param str String to write into the file
 * @return true The write succeeded
 * @return false The write failed
 */
bool actsim_file_write_core(size_t writer_id, std::string str) {
    // make sure the file has been opened for writing
    if (output_streams.find(writer_id) == output_streams.end()) {
        std::cerr
            << "actim_file_write_core: Unknown writer ID, open a file first!"
            << std::endl;
        return false;
    }

    // make sure the file is still open
    if (!output_streams[writer_id].first.is_open()) {
        std::cerr << "actim_file_write_core: File index "
                  << output_streams[writer_id].second << " is closed."
                  << std::endl;
        return false;
    }

    // write to the file
    output_streams[writer_id].first << str;
    output_streams[writer_id].first.flush();

    return true;
}

/**
 * @brief Close a file that was open for writing
 *
 * This method can be called from a CHP function block.
 * It requires one argument to be passed in args:
 * - args[0]: The ID of writer calling
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res 1 on success, 0 otherwise
 */
extern "C" expr_res actsim_file_closew(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;  // on error we return 0 (boolean like)

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actim_file_closew: Must be invoked with 1 argument only"
                  << std::endl;
        return ret;
    }

    size_t writer_id = args[0].v;

    // check if we have the have the requested file already open
    if (output_streams.find(writer_id) == output_streams.end()) {
        std::cerr << "actim_file_closew: Unknown writer ID, open a file first!"
                  << std::endl;
        return ret;
    }

    // make sure the file isn't already closed
    if (!output_streams[writer_id].first.is_open()) {
        std::cerr << "actim_file_closew: File was already closed!" << std::endl;
        output_streams.erase(writer_id);
        return ret;
    }

    auto file_id = output_streams[writer_id].second;

    // close file and erase the writer ID
    output_streams[writer_id].first.close();
    output_streams.erase(writer_id);
    ret.v = 1;

    // remove the file from the list of open files
    output_files.erase(file_id);

    return ret;
}
