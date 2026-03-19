#!/bin/sh

# Assignment 1: finder.sh
#
# Implements a script that takes in a path to a directory on the filesystem (referred to as filesdir), the second
# argument is a text string which will be searcvhed within these files (referred to as searchstr).
#
# Exits with return value 1 error and print statements if any of the parameters were not specified
# Exits with return value 1 error and print statements if filesdir does not represent a directory on the system
# Prints a message "The number of files are X and the number of matching lines are Y" where X is the number of
# files in the directory and all subdirectories and Y is the number of matching lines found in respective files, 
# where a matching line refers to a line which contains searchstr (and may contain additional content).

filearg=$1

searcharg=$2

if [ $# != 2 ];
then
	echo "ERROR: Expected two arguments. Exiting now...."
	exit 1
fi

if [ -d "$filearg" ]; 
then
	filecount=`ls $filearg | wc -l`

	linecount=`grep -i "$searcharg" ${filearg}/*.* | wc -l`

	echo "The number of files are $filecount and the number of matching lines are $linecount"
	
	exit 0

else
	echo "ERROR: Directory does not exist! Exiting now..."
	exit 1
fi

