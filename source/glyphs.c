/* glyphs.c -- the Switch's button icons (L, R, +, -) written next to the NRO from its own copy (glyphs_data.c)
 *
 * The game shows Xbox icons (LB, RB, the Menu and View buttons) with Project Dream's controller bridge; the port's
 * versions of them (tools/switch_glyphs.py: the game's own icons, remade) are bundles pd_shim.c's bundle_path
 * override loads from <NRO folder>/glyphs. Built into the NRO since the first release (no tool for a player to
 * run): each written at start when it is missing or not the same size.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

#include "log.h"
#include "paths.h"

typedef struct {
  const char *name;
  const uint8_t *data;
  size_t size;
} GlyphFile;

extern const GlyphFile g_glyph_files[];
extern const size_t g_glyph_count;

void glyphs_install(void) {
  char dir[512], path[700];
  snprintf(dir, sizeof dir, "%s/glyphs", path_base());
  mkdir(dir, 0777);
  unsigned wrote = 0;
  for (size_t i = 0; i < g_glyph_count; i++) {
    const GlyphFile *g = &g_glyph_files[i];
    snprintf(path, sizeof path, "%s/%s", dir, g->name);
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == g->size)
      continue;
    FILE *f = fopen(path, "wb");
    if (!f)
      continue;
    const size_t n = fwrite(g->data, 1, g->size, f);
    fclose(f);
    wrote += n == g->size;
  }
  if (wrote)
    log_printf("[glyphs] the Switch's button icons written (%u of %zu)", wrote, g_glyph_count);
}
