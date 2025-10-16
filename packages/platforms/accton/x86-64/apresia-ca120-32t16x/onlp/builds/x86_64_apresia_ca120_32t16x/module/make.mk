###############################################################################
#
# 
#
###############################################################################
THIS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
x86_64_apresia_ca120_32t16x_INCLUDES := -I $(THIS_DIR)inc
x86_64_apresia_ca120_32t16x_INTERNAL_INCLUDES := -I $(THIS_DIR)src
x86_64_apresia_ca120_32t16x_DEPENDMODULE_ENTRIES := init:x86_64_apresia_ca120_32t16x ucli:x86_64_apresia_ca120_32t16x
