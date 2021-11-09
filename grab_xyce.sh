#!/bin/sh

if [ $# -ne 1 ]
then
	echo "Usage: $0 <path-to-xyce-build-dir>"
	exit 1
fi

xyce_build=$1

echo "Xyce build dir: $xyce_build"

if [ ! -d $xyce_build ]
then
	echo "Could not find directory $xyce_build"
	exit 1
fi

if [ ! -d $xyce_build/src/CMakeFiles/Xyce.dir ]
then
	echo "Could not find Xyce sub-directory (src/CMakeFiles/Xyce.dir) in $xyce_build"
	exit 1
fi

file=$xyce_build/src/CMakeFiles/Xyce.dir/link.txt

if [ ! -f $file ]
then
	echo "Could not find Xyce link.txt file in build directory"
	exit 1
fi

linker_stuff=
compiler_stuff=

found=0

for i in `cat $file`
do
	case $found in
	0) compiler_stuff=$i; found=1;;
	1) if [ $i = "Xyce" ]
	   then
		found=2
	   fi;;
	2)  if [ $i = "libxyce.a" ]
	    then
		val="-lxyce"
	    else
		val=$i
	    fi;
	    linker_stuff="${linker_stuff} $val";;
	esac
done

echo 
echo "Creating xyce.in..."
echo 
echo "Xyce was linked with the following compiler:"
echo "  $compiler_stuff"
echo 
echo "If this is different from the standard compiler, use it for linking actsim"
echo "(e.g. by using make CXX=newcompiler)"
echo 
echo "LIBXYCE=-lxycecinterface $linker_stuff" > xyce.in
