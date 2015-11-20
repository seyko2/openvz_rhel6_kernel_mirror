#!/bin/bash

make CC=x86_64-linux-gnu-gcc -C /usr/src/linux-2.6.32-fan32d-r10202.x86_64 SUBDIRS=`pwd` modules 2>&1 | tee LOG
