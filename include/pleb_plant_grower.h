/*
 * pleb-plant-grower — the one public header.
 *
 * Everything kilix-land needs to embed the game, and everything the standalone
 * frontend needs to drive it, is declared here. Internal structure lives in
 * src/pg_*.h and is not installed.
 */
#ifndef PLEB_PLANT_GROWER_H
#define PLEB_PLANT_GROWER_H

#ifdef __cplusplus
extern "C" {
#endif

#define PG_VERSION "0.1.0"

/* Logical stage. The fleet's rooms are authored in this space and scaled to
 * the terminal framebuffer at present time. */
#define PG_STAGE_WIDTH  480
#define PG_STAGE_HEIGHT 270

/* Exactly four of each at first release. */
#define PG_SPECIES_COUNT 4
#define PG_POT_COUNT     4

/* Build identity. Returns PG_VERSION; the archive's proof of life. */
const char *pg_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PLEB_PLANT_GROWER_H */
