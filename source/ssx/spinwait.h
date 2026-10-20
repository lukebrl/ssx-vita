/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  spinwait.h
 * @brief Makes the game's busy-wait loops sleep between checks.
 */

#ifndef SSX_SPINWAIT_H
#define SSX_SPINWAIT_H

void ssx_spinwait_patch(void);
void ssx_spinwait_report(void);

#endif // SSX_SPINWAIT_H
