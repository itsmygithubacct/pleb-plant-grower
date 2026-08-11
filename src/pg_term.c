/*
 * Terminal session ownership for the standalone frontend: framebuffer sizing,
 * the snap logic that owns the resolution floor, and input. Standalone only —
 * never part of the archive kilix-land links.
 *
 * kittyfb_options does have min_width/min_height and we deliberately leave them
 * unset, so a small terminal degrades to the 800x450 fallback rather than being
 * refused. The floor is policy and lives here.
 */
#include "pg_term.h"

int pg_term_placeholder(void)
{
    return 0;
}
