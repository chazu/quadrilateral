#ifndef QUAD_RANDOM_H
#define QUAD_RANDOM_H

/* Counter-based random numbers (docs/adr/0002).
 *
 * A draw is a pure function of five values:
 *
 *   run seed      one per run
 *   stream name   for example "ignition"; hashed with qr_name_hash
 *   phenomenon    the identity of the phenomenon that draws
 *   event index   the phenomenon's own count of its events
 *   draw index    the position of the draw inside that event
 *
 * No generator state is shared, so a draw does not depend on the order in
 * which the scheduler runs phenomena. Sequential and parallel runs give the
 * same draws. The generator is Philox4x64-10 (Salmon et al., 2011): the key is
 * (run seed, stream name hash) and the counter is (event index, block index,
 * phenomenon, 0). The fourth counter word is reserved and is 0.
 *
 * Changing the generator changes every draw, so it changes the result of
 * every saved seed. QR_VERSION names the current mapping from inputs to draws.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QR_VERSION 1

/* The raw generator: 4 words out for a 4-word counter and a 2-word key. */
void qr_philox4x64(const uint64_t ctr[4], const uint64_t key[2], uint64_t out[4]);

/* FNV-1a, 64 bits. Stream names are hashed, not numbered, so that adding a
 * stream does not change the draws of the other streams. */
uint64_t qr_name_hash(const char *name, size_t len);

typedef struct {
    uint64_t key[2];       /* run seed, stream name hash */
    uint64_t phenomenon;
} QRStream;

QRStream qr_stream(uint64_t seed, uint64_t phenomenon, const char *name);

/* Draw `draw` of event `event`: a pure function. */
uint64_t qr_u64(const QRStream *s, uint64_t event, uint64_t draw);

/* A cursor reads the draws of one event in order: draw 0, 1, 2, ... */
typedef struct {
    QRStream s;
    uint64_t event;
    uint64_t next;          /* the index of the next draw */
    uint64_t block;         /* the block in buf; UINT64_MAX if none */
    uint64_t buf[4];
} QRCursor;

QRCursor qr_cursor(const QRStream *s, uint64_t event);
uint64_t qr_next(QRCursor *c);

/* A uniform integer in [0, n), with no bias (Lemire's method). n == 0 means
 * the full 64-bit range. A rejection uses more draws, so the number of draws
 * that one call uses can vary. */
uint64_t qr_below(QRCursor *c, uint64_t n);

/* A uniform integer in [lo, hi], lo <= hi. */
int64_t qr_range(QRCursor *c, int64_t lo, int64_t hi);

/* True with probability p × 10^scale, exactly, for scale in [-18, 0].
 * Example: p = 250000000 with scale -9 is a probability of 0.25. A p at or
 * below 0 is never true, and a p at or above 10^-scale is always true. Each
 * call uses a draw, also for those two cases, so a change in p does not shift
 * the later draws of the event. */
bool qr_chance(QRCursor *c, int64_t p, int scale);

/* A double in [0, 1), with 53 random bits. Kernels can use this for private
 * floating-point state. Authoritative state uses the integer functions. */
double qr_double(QRCursor *c);

#endif
