#!/bin/sh

# writer.sh
#
# 

writefile=$1

writestr=$2

if [ $# != 2 ];
then
        echo "ERROR: Expected two arguments. Exiting now...."
        exit 1
fi

filename="${writefile##*/}"

directory=`dirname $writefile`

if [ ! -d "$directory" ];
then
	mkdir -p $directory
	
	if [ ! -d "$directory" ];
	then
		echo "ERROR: Could not create directory. Exiting now..."
		exit 1
	fi
fi

touch $directory/$filename
echo "$writestr" > $directory/$filename
