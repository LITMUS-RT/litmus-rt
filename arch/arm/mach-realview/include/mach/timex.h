/*
 *  arch/arm/mach-realview/include/mach/timex.h
 *
 *  RealView architecture timex specifications
 *
 *  Copyright (C) 2003 ARM Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define CLOCK_TICK_RATE		(50000000 / 16)

#if defined(CONFIG_MACH_REALVIEW_PB11MP) || defined(CONFIG_MACH_REALVIEW_PB1176)

static inline unsigned long realview_get_arm11_cp15_ccnt(void)
{
	unsigned long cycles;
	/* Read CP15 CCNT register. */
	asm volatile ("mrc p15, 0, %0, c15, c12, 1" : "=r" (cycles));
	return cycles;
}

#define get_cycles realview_get_arm11_cp15_ccnt

#elif defined(CONFIG_MACH_REALVIEW_PBA8)


static inline unsigned long realview_get_a8_cp15_ccnt(void)
{
	unsigned long cycles;
	/* Read CP15 CCNT register. */
	asm volatile ("mrc p15, 0, %0, c9, c13, 0" : "=r" (cycles));
	return cycles;
}

#define get_cycles realview_get_a8_cp15_ccnt

#endif
