#!/bin/bash

make -C /usr/src/linux SUBDIRS=`pwd`/ clean
rm LOG .tmp* 2> /dev/null
