
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

#include <iostream>
#include <memory>
#include <queue>
#include <vector>

#include "../actsim_ext.h"

std::vector<std::unique_ptr<std::queue<expr_res>>> buffers;

/**
 * @brief Create a new buffer
 *
 * @param argc Number of arguments given (must be 0)
 * @param args Argument vector
 * @return expr_res Buffer ID
 */
extern expr_res actsim_buffer_create(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 32;
    ret.v = 0;

    // make sure we have the appropriate amount of arguments
    if (argc != 0) {
        std::cerr
            << "actsim_buffer_exists: Must be invoked with 0 arguments only"
            << std::endl;
        return ret;
    }

    // the size is the new last index
    ret.v = buffers.size();

    // create the new buffer
    buffers.emplace_back(std::make_unique<std::queue<expr_res>>());

    return ret;
}

/**
 * @brief Push to infinite sized buffer
 *
 * @param argc Number of arguments given (must be 2; buffer ID, value)
 * @param args Argument vector
 * @return expr_res 1 on success, 0 otherwise
 */
extern expr_res actsim_buffer_push(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 0;

    // make sure we have the appropriate amount of arguments
    if (argc != 2) {
        std::cerr << "actsim_buffer_push: Must be invoked with 2 arguments "
                     "only (buffer ID, value)"
                  << std::endl;
        return ret;
    }

    size_t index = args[0].v;
    auto& value = args[1];

    // make sure the buffer exists
    if (index >= buffers.size()) {
        std::cerr << "actsim_buffer_push: Invalid buffer ID (out of range)"
                  << std::endl;
        return ret;
    }

    // push to the buffer
    buffers[index]->push(value);
    ret.v = 1;

    return ret;
}

/**
 * @brief Probe the infinite sized buffer
 *
 * @param argc Number of arguments given (must be 1; buffer ID)
 * @param args Argument vector
 * @return expr_res Is buffer empty?
 */
extern expr_res actsim_buffer_empty(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 1;
    ret.v = 1;  // on fault buffer is shown as empty

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actsim_buffer_empty: Must be invoked with 1 argument "
                     "only (buffer ID)"
                  << std::endl;
        ret.v = 1;
        return ret;
    }

    size_t index = args[0].v;

    // make sure the buffer exists
    if (index >= buffers.size()) {
        std::cerr << "actsim_buffer_empty: Invalid buffer ID (out of range)"
                  << std::endl;
        return ret;
    }

    // actually check the buffer state
    ret.v = buffers[index]->empty();

    return ret;
}

/**
 * @brief Pop from infinite sized buffer
 *
 * @param argc Number of arguments given (must be 1)
 * @param args Argument vector
 * @return expr_res Value removed from buffer
 */
extern expr_res actsim_buffer_pop(int argc, struct expr_res* args) {
    expr_res ret;
    ret.width = 64;
    ret.v = 0;

    // make sure we have the appropriate amount of arguments
    if (argc != 1) {
        std::cerr << "actsim_buffer_pop: Must be invoked with 1 argument only "
                     "(buffer ID)"
                  << std::endl;
        return ret;
    }

    size_t index = args[0].v;

    // make sure the buffer exists
    if (index >= buffers.size()) {
        std::cerr << "actsim_buffer_pop: Invalid buffer ID (out of range)"
                  << std::endl;
        return ret;
    }

    // make sure the buffer isn't empty
    if (!buffers[index]->empty()) {
        std::cerr << "actsim_buffer_pop: Buffer pop called but buffer is empty!"
                  << std::endl;
        return ret;
    }

    // get the value from the buffer
    ret = buffers[index]->front();
    buffers[index]->pop();

    return ret;
}
