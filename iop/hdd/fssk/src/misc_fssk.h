/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

#ifndef __MISC_FSSK_H__
#define __MISC_FSSK_H__

extern int fsskCreateEventFlag(void);
extern int fsskCreateThread(void (*function)(void *arg), int StackSize);

#endif
