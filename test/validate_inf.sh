#!/bin/sh

ARCH=`$VLSI_TOOLS_SRC/scripts/getarch`
OS=`$VLSI_TOOLS_SRC/scripts/getos`
EXT=${ARCH}_${OS}
ACTTOOL=../actsim.$EXT 

if [ $# -eq 0 ]
then
	list=inf[0-9]*.act
else
	list="$@"
fi

if [ ! -d runs ]
then
	mkdir runs
fi

for i in $list
do
	$ACTTOOL $i 'test<>' > runs/$i.stdout 2> runs/$i.stderr <<EOF
step 10000
EOF
done
