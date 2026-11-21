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

/* The kernels of a room with n cells in a row, c0 .. c{n-1}. Each function
 * generates a contract for n and returns NULL for n outside 1..100000. Free
 * a generated kernel with qk_free after every world that uses it is freed.
 *
 * native.diffusion: an evolution kernel for the room's model. Slots:
 * c{i}.temperature (IN, K), c{i}.capacity (IN, J/K), c{i}.energy (STOCK, J,
 * scale -3), conductance (IN, W/K, between neighbours) and tolerance (IN, in
 * (0, 1]). Conserved transfers flow{i} from c{i} to c{i+1}. The step is
 * tolerance × C_min / (2G), inside the stability limit of explicit
 * diffusion.
 *
 * native.room.refine: a map kernel from an aggregate model to the n-cell
 * model. Slots: from.energy (STOCK), to.c{i}.energy (STOCK), to.c{i}.capacity
 * (IN) and spread (IN, K). Each cell gets the mean temperature, moved by a
 * seeded amount in [-spread, spread]. The last cell takes the remainder, so
 * the source ends at exactly zero.
 *
 * native.room.abstract: a map kernel from the n-cell model to the aggregate
 * model. Slots: from.c{i}.energy (STOCK) and to.energy (STOCK). It moves
 * every cell's energy into the one stock. */
QkKernel *qk_diffusion_new(unsigned n);
QkKernel *qk_refine_new(unsigned n);
QkKernel *qk_abstract_new(unsigned n);
void      qk_free(QkKernel *k);

#endif
