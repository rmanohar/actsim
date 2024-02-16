
/*************************************************************************
 *
 *  This file is part of ACT standard library
 *
 *  Copyright (c) 2024 Fabian Posch
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 **************************************************************************
 */

#include <cstdint>
#include <iostream>
#include <sstream>

#include "../../actsim_ext.h"
#include "../simlib_file.h"

/**
 * @brief Write a logger line to a file
 *
 * The log writer supports verbosity level, these are:
 * 0: 'channel':'value'
 * 1: 'ID' ('channel'): 'value'
 * 2: Logger 'ID' (Channel 'channel'): Received value 'value'%x (0x'value')
 *
 * This method can be called from a CHP function block.
 * It requires one argument to be passed in args:
 * - args[0]: The ID of the writer calling
 * - args[1]: The verbosity level
 * - args[2]: The logger ID
 * - args[3]: The logger channel
 * - args[4]: The value to write
 *
 * @param argc number of arguments in args
 * @param args argument vector
 * @return expr_res 0 on success, 1 otherwise
 */
extern "C" expr_res actsim_file_write_log(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 1;  // on error we return 1

    // make sure we have the appropriate amount of arguments
    if (argc != 5) {
        std::cerr
            << "actim_file_write_log: Must be invoked with 5 arguments only"
            << std::endl;
        return ret;
    }

    uint32_t writer_id = args[0].v;
    uint8_t verbosity = args[1].v;
    uint32_t logger_id = args[2].v;
    uint32_t channel = args[3].v;
    uint64_t value = args[4].v;

    // build the log line
    std::ostringstream builder;

    switch (verbosity) {
        case 0:
            builder << channel << ":" << std::hex << value << std::endl;
            break;

        case 1:
            builder << logger_id << " (" << channel << "): " << std::hex
                    << value << std::endl;
            break;

        case 2:
            builder << "Logger " << logger_id << " (Channel " << channel
                    << "): Received value " << value << "%x (0x" << std::hex
                    << value << ")" << std::endl;

        default:
            break;
    }

    ret.v = actsim_file_write_core(writer_id, builder.str());
    return ret;
}