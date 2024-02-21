
# actsim Simulation Library

This folder contains the simulation library shipped with actsim. It contains multiple components which are helpful when simulating and/or verifying asynchronous logic designs.
Components in this folder are not synthesizable and should not be used in a final design. They are high level and partially C++ descriptions for verification.

## Contents

This library contains components for various purposes. A brief list can be found here.

### Sources

* `source_static`: Simple single-ended source that will repeatedly emit a static value token
* `source_static_multi`: Simple multi-ended source that will repeatedly emit a static value token
* `source_sequence`: Emits a static sequence of token values on a single output channel, can loop through the values
* `source_sequence_multi`: Emits a static sequence of token values on a configurable number of output channels, can loop through the values
* `source_file`: Emits a list of tokens given through an input file on a single output channel. The input file format supports comments, and is base 10 by default. Base 2 (0b), 8 (0o), and 16 (0x) are also supported. The input file must not have any trailing newlines.
* `source_file_multi`: Same as `source_file`, but with a configurable number of output channels.

### Sinks

* `sink`: Simple token sink
* `sink_file`: Writes the observed tokens into an output file with configurable level of verbosity

### Scoreboards

* `lockstep`: Used when one or more inputs and outputs are expected to see the same number of tokens in the same order for model and DUT
* `deterministic`: Used when one or more output channels see the same number of tokens in the same order for model and DUT
* `input_logger`: Used to log the input tokens for one or more input channels (identical to sink; same message format and verbosity parameters as scoreboards)

### Utility

* `logger`: Zero-slack logger which reports tokens passing through the channel
* `logger_file`: Zero-slack logger which prints tokens passing through the channel to a file (with configurable level of verbosity)
* `buffer`: Infinite capacity buffer. Used to eliminate timing impact and channel blocking of simulation harness on the DUT

### File interaction

* `openr`: Open a file by ID for reading (multiple instances can read the same file without interference). Returns a reader ID (basically a file descriptor)
* `read`: Read a line from a file
* `eof`: Returns true if the reader has reached the end of the file
* `closer`: Close a file for reading
* `openw`: Open a file by ID for writing (only one instance can write to the same file). Returns a writer ID (basically a file descriptor)
* `write`: Write a value to a file
* `closew`: Close a file for writing

### Random generators

* `source_simple`: Returns a stream of random numbers
* `source_simple_multi`: Returns an identical stream of random numbers on multiple output channels
* `source_range`: Returns a stream of random numbers within a given range
* `source_range_multi`: Returns an identical stream of random numbers within a given range on multiple output channels

## Roadmap

* Add tests for random sources
* Add splitter to utility
* Support trailing newline in file sources
* Token counters; both to count to a parameter given number of tokens and raise a flag as well as count equal number of tokens on two different busses
* Nondeterministic scoreboard for designs where the token order of design and model might differ but the token number is identical
* Support for constrained random testing using new sources with C++ backend
* Drop in backend to support distributed constrained random testing in action
* More comprehensive source/sink/logger/scoreboard identification once string parameters are available
* Move non-essential parameters to default parameters once available
* Move enable connections to default connections once available
