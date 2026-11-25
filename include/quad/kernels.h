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

/* native.rate: moves a stock at a rate that a slot gives (docs/adr/0020).
 * The front end generates one for each phenomenon that has interactions
 * with a rate, so source needs no kernel in C for a flow.
 *
 * The caller gives the slots. For each transfer it names the slot of the
 * rate (IN, or a STOCK of the phenomenon: a speed is the rate of a position),
 * the slot of the tolerance (IN), and the slots of the source
 * and of the sink (STOCK or LINK, or QK_NONE for one of them). The rate has
 * the dimension of the stock for each second. The tolerance has no dimension
 * and the scale -9, in (0, 1]. A transfer with a source and a sink is
 * conserved.
 *
 * Each step moves rate × step, from the rate at the start of the step
 * (explicit coupling, ADR 0009 section 9). The part of the amount that is
 * smaller than one quantum of the stock stays for the next step, so a small
 * rate loses nothing to rounding. The horizon keeps the change of the rate in
 * one step at about `tolerance` of the rate: the kernel measures how much the
 * rate changed since its last change, and assumes the same slope. The first
 * step is one tick, and a step is at most two times the time since the last
 * change. The kernel keeps that history for each transfer, so it is not
 * stateless: it gives save and restore. The arithmetic is integer only.
 *
 * A reaction (docs/adr/0021 section 7) is a set of transfers with one rate
 * slot, one tolerance slot, and a ratio slot for each transfer (IN). The
 * amount of the reaction in a step is rate × step, and each transfer moves
 * that amount times its ratio. The ratio has the dimension of its stock for
 * each unit of the reaction. Each transfer keeps the part of its amount that
 * is smaller than one quantum, so the transfers keep their ratios over a
 * run: the sum of a transfer differs from the sum of the reaction times the
 * ratio by less than one quantum of the stock, while the ratio is constant.
 *
 * qk_rate_new copies its arguments. NULL if a transfer names a slot that
 * does not fit. Free the kernel with qk_rate_free after every world that
 * uses it is freed, or give it to the world with qw_adopt_kernel. */
typedef struct {
    const char *name;
    uint32_t    rate, tolerance;   /* slot indices */
    uint32_t    from, to;          /* slot indices, or QK_NONE */
    uint32_t    ratio;             /* slot index of the ratio of a flow of a reaction, or QK_NONE */
} QkRateTransfer;

QkKernel *qk_rate_new(const QkSlot *slots, uint32_t nslots, const QkRateTransfer *transfers, uint32_t ntransfers);
void      qk_rate_free(QkKernel *k);

#endif
