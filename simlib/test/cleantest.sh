#!/bin/sh

process_name="test"

for subdir in ./*/
do
    if [ -d $subdir ]
    then
        cd "$subdir"
        rm -f $process_name.processed $process_name.stdout $process_name.stderr .actsim_history
        cd ..
    fi
done

echo
echo "All tests cleaned."
echo