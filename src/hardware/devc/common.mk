#  
# Copyright 2007, 2008, QNX Software Systems. 
#  
# Licensed under the Apache License, Version 2.0 (the "License"). You 
# may not reproduce, modify or distribute this software except in 
# compliance with the License. You may obtain a copy of the License 
# at: http://www.apache.org/licenses/LICENSE-2.0 
#  
# Unless required by applicable law or agreed to in writing, software 
# distributed under the License is distributed on an "AS IS" basis, 
# WITHOUT WARRANTIES OF ANY KIND, either express or implied.
# 
# This file may contain contributions from others, either as 
# contributors under the License or as licensors under other terms.  
# Please review this entire file for other proprietary rights or license 
# notices, as well as the QNX Development Suite License Guide at 
# http://licensing.qnx.com/license-guide/ for other information.
# 
#
# General purpose makefile for building a Neutrino character device driver
#
ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

NAME=devc-$(SECTION)
EXTRA_SILENT_VARIANTS+=$(subst -, ,$(SECTION))

EXTRA_INCVPATH+=$(INSTALL_ROOT_nto)/usr/include/xilinx
EXTRA_LIBVPATH+=$(INSTALL_ROOT_nto)/usr/lib/xilinx


LIBS += io-char drvr
LIBS_hscif += dma-sysdmac
LIBS += $(LIBS_$(SECTION))



EXCLUDE_OBJS+=tedit.o
USEFILE=$(SECTION_ROOT)/options.c

INSTALLDIR=sbin

define PINFO
PINFO DESCRIPTION=
endef

include $(MKFILES_ROOT)/qmacros.mk
include $(SECTION_ROOT)/pinfo.mk

TINY_NAME=$(subst devc-,devc-t,$(BUILDNAME))

ifneq (,$(filter tedit.c, $(notdir $(SRCS))))

POST_TARGET=$(TINY_NAME)
EXTRA_ICLEAN=$(TINY_NAME)*

define POST_INSTALL
	-$(CP_HOST) $(TINY_NAME) $(INSTALL_DIRECTORY)/
endef

endif


#####AUTO-GENERATED by packaging script... do not checkin#####
   INSTALL_ROOT_nto = $(PROJECT_ROOT)/../../../install
   USE_INSTALL_ROOT=1
##############################################################

include $(MKFILES_ROOT)/qtargets.mk


-include $(PROJECT_ROOT)/roots.mk
ifndef LIBIOCHAR_ROOT
LIBIOCHAR_ROOT=$(PRODUCT_ROOT)
endif

#
# Some makefile mopery-popery to get devc-t*.pinfo generated properly
#
$(TINY_NAME): INSTALLNAME=$(INSTALL_DIRECTORY)/$(TINY_NAME)

$(TINY_NAME): tedit.o $(OBJS) $(LIBNAMES)
	$(TARGET_BUILD)
