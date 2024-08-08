# actsim: The ACT simulator
[![CircleCI](https://dl.circleci.com/status-badge/img/gh/rmanohar/actsim/tree/master.svg?style=svg)](https://dl.circleci.com/status-badge/redirect/gh/rmanohar/actsim/tree/master)

`actsim` is a mixed-signal simulator capable of simulating [ACT](https://avlsi.csl.yale.edu/act) files.
`actsim` can simulate designs containing a combination of CHP, HSE, PRS, and analog circuits in a unified framework.
For analog simulation, we use the [`Xyce`](https://github.com/Xyce/Xyce) simulator.
Some of the extensions we needed in `Xyce` for mixed-signal simulation have been accepted by the Xyce team and are now part of the latest `Xyce` release.

### Build and Installation

This program is for use with [the ACT toolkit](https://github.com/asyncvlsi/act).

   * Please install the ACT toolkit first; installation instructions are [here](https://github.com/asyncvlsi/act/blob/master/README.md).
   * Also install the ACT [standard library](https://github.com/asyncvlsi/stdlib).
   * Also install the ACT [annotate library](https://github.com/asyncvlsi/annotate).
   * Build this program using the standard ACT tool install instructions [here](https://github.com/asyncvlsi/act/blob/master/README_tool.md).

To build and install, run `./configure` and then `./build.sh`.


### Building with Xyce

There are a number of things to keep in mind when building `actsim` with `Xyce`.

Building `Xyce`:
   
   * For Xyce 7.6 or newer
      * Build `Xyce` itself, using `cmake` and `$ACT_HOME` as the install directory
      * to compile the required interface run `make xycecinterface` in your cmake build directory
      * install the interface required by actsim with `make install` to `$ACT_HOME`
   * For Xyce 7.5 and older
      * Build and install `Xyce` itself, using `cmake` and `$ACT_HOME` as the install directory
      * To build and install the Xyce C interface library (in the `xyce-bits` directory), use the following commands:
         * Go to the `xyce-bits/` directory
         * Build an object file using `g++ -std=c++17 -I. -I$ACT_HOME/include -c N_CIR_XyceCInterface.C`
         * Create the library using `ar ruv libxycecinterface.a N_CIR_XyceCInterface.o`
         * If you need to, use `ranlib libxycecinterface.a`
         * Copy `libxycecinterface.a` to `$ACT_HOME/lib`
         * Copy `N_CIR_XyceCInterface.h` to `$ACT_HOME/include`
   * Preserve your cmake build directory for the next step. (We need one file from it as described below.)

Note: the `xyce-bits/` directory is only needed if you are using a version of Xyce prior to 7.6. The changes 
have been incorporated by the Xyce team into their core distribution.

Building `actsim`:
   * When you run `./configure`, it should detect that the Xyce C interface library exists.
   * To use static linking for `actsim`, we need to extract the correct link-time options used by your Xyce build. To do so:
      *  Run `./grab_xyce.sh <path>` where `<path>` is the path to the cmake build directory for your Xyce build
      *  Alternatively, you can copy the file `src/CMakeFiles/Xyce.dir/link.txt` from your cmake build directory for Xyce, place it it any directory, and run `grab_xyce.sh` with the path set to the directory containing `link.txt`
   * After this, you can build actsim.

### Running the simulator

To start a simulation, use `actsim <file.act> <top-level-process>`. 
More information on running a simulation is [available](https://avlsi.csl.yale.edu/act/doku.php?id=tools:actsim).
