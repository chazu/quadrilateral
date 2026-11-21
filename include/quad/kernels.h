#ifndef QUAD_KERNELS_H
#define QUAD_KERNELS_H

/* Native kernels that come with Quadrilateral. */

#include "quad/kernel.h"

/* native.conduction: steady-state conduction through a wall between two
 * thermal ports, "hot" and "cold" (design section 9.2).
 *
 * Slots: hot.temperature, cold.temperature (IN, K), hot.capacity,
 * cold.capacity (IN, J/K), hot.energy, cold.energy (LINK, J, scale -3),
 * conductance (IN, W/K) and tolerance (IN, dimensionless, scale -9, in
 * (0, 1]). One conserved transfer, "heat", from hot.energy to cold.energy.
 *
 * Each step transfers conductance × (T_hot − T_cold) × step, from the
 * temperatures at the start of the step (explicit coupling, ADR 0009
 * section 9). The horizon makes the step at most `tolerance` times the time
 * constant of the two lumped capacities: C_h C_c / (G (C_h + C_c)). The
 * arithmetic is integer only, so the kernel is portable. */
const QkKernel *qk_conduction(void);

#endif
