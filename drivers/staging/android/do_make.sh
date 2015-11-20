#!/bin/bash

make CC=gcc-4.1.2 -C /usr/src/linux-2.6.32-fan32d-r10202 SUBDIRS=`pwd` modules 2>&1 | tee LOG
