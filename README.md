# actsim

### Build and Installation

This program is for use with [the ACT toolkit](https://github.com/asyncvlsi/act).

   * Please install the ACT toolkit first; installation instructions are [here](https://github.com/asyncvlsi/act/blob/master/README.md).
   * Also install the ACT [standard library](https://github.com/asyncvlsi/stdlib).
   * Build this program using the standard ACT tool install instructions [here](https://github.com/asyncvlsi/act/blob/master/README_tool.md).

To build, run `./configure` and then `make` and `make install`.


### Building with Xyce

There are a number of things to keep in mind when building `actsim` with `Xyce`.

Building `Xyce`:
   * Replace the files `N_CIR_Xyce.{C,h}` (in `src/CircuitPKG/`) and `N_CIR_XyceCInterface.{C,h}` (in `utils/XyceCInterface`) with the modified files from `xyce-bits/` (note: we've sent these to the Xyce authors and this should not be needed in the near future)
   * Build and install `Xyce` itself, and also build and install the Xyce C interface library (in the `utils/XyceCInterface` directory)
   * Preserve your cmake build directory for the next step. (We need one file from it as described below.)

Building `actsim`:
   * When you run `./configure`, it should detect that the Xyce C interface library exists.
   * To use static linking for `actsim`, we need to extract the correct link-time options used by your Xyce build. To do so:
      *  Run `./grab_xyce.sh <path>` where `<path>` is the path to the cmake build directory for your Xyce build
      *  Alternatively, you can copy the file `src/CMakeFiles/Xyce.dir/link.txt` from your cmake build directory for Xyce, place it it any directory, and run `grab_xyce.sh` with the path set to the directory containing `link.txt`
   * After this, you can build actsim.
 
