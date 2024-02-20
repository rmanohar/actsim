#!/bin/sh

echo "Generate truth files from current project state"
echo "(use only if output is correct for a certain test)"
echo "test directory name: "
read test_dir_name

if [ -d $test_dir_name ]
then

    cd "$test_dir_name"
    i=test.act
    j=test
    k=test.actsim
    new_reg=new_regression.truth

    # simulate 
    actsim $i $j < $k > $new_reg 2>/dev/null

    if [ $? -eq 0 ] 
    then
        sed -E 's/(\[.*\]\s*)(<.*>.*)/\2/g' $new_reg > $new_reg.processed
        mv $new_reg.processed test.truth
        rm -f $new_reg
        echo "test.truth updated successfully"
    else
        echo "actsim unnatural exit, truth not updated!"
        exit 1
    fi

else
    echo "test does not exist!"
    exit 1
fi