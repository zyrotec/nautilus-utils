/* Folder views core (PLAN.md §10). See fv_state.h. */
#include "fv_state.h"

#include <string.h>

#include "config.h"

/* --- Saved value ------------------------------------------------------------------------------ */

/* Returns a newly allocated string; caller frees with g_free. */
char *
fv_encode (const NuFolderView *v,
           guint               generation)
{
  g_return_val_if_fail (nu_folder_view_valid (v), NULL);
  return g_strdup_printf ("%d;%u;%d;%u;%u", FV_FORMAT_VERSION, generation, (int) v->view, v->grid_zoom,
                          v->list_zoom);
}

/* Strict unsigned decimal: digits only (no sign, no spaces), at most 10 of them, value <= @max. */
static gboolean
parse_field (const char *text,
             guint64     max,
             guint64    *out)
{
  gsize len = strlen (text);
  if (len == 0 || len > 10)
    return FALSE;
  for (gsize i = 0; i < len; i++)
    if (!g_ascii_isdigit (text[i]))
      return FALSE;
  return g_ascii_string_to_unsigned (text, 10, 0, max, out, NULL);
}

gboolean
fv_decode (const char   *text,
           guint         generation,
           NuFolderView *out)
{
  if (text == NULL || strlen (text) > 64) /* a valid value is at most 20 bytes */
    return FALSE;
  g_auto (GStrv) f = g_strsplit (text, ";", -1); /* owned, freed by g_auto */
  if (g_strv_length (f) != 5)
    return FALSE;
  guint64 version, gen, view, grid, list;
  if (!parse_field (f[0], G_MAXUINT, &version) || version != FV_FORMAT_VERSION ||
      !parse_field (f[1], G_MAXUINT, &gen) || gen != generation || !parse_field (f[2], G_MAXUINT, &view) ||
      !parse_field (f[3], NU_GRID_ZOOM_MAX, &grid) || !parse_field (f[4], NU_LIST_ZOOM_MAX, &list))
    return FALSE;
  NuFolderView v = { .view = (NuViewType) view, .grid_zoom = (guint8) grid, .list_zoom = (guint8) list };
  if (!nu_folder_view_valid (&v))
    return FALSE;
  *out = v;
  return TRUE;
}

/* --- Locations -------------------------------------------------------------------------------- */

FvLocationKind
fv_location_kind (const char *uri)
{
  if (uri == NULL || *uri == '\0')
    return FV_LOCATION_IGNORE;
  g_autofree char *scheme = g_uri_parse_scheme (uri); /* owned; NULL if @uri has no valid scheme */
  if (scheme == NULL)
    return FV_LOCATION_IGNORE;
  if (g_ascii_strcasecmp (scheme, "network") == 0)
    {
      /* Only the root is Files' Network view; anything below it is a normal folder. */
      const char *rest = uri + strlen (scheme) + 1;
      while (*rest == '/')
        rest++;
      return *rest == '\0' ? FV_LOCATION_IGNORE : FV_LOCATION_SAVE;
    }
  static const char *const default_only[] = { "starred", "other-locations", "x-nautilus-search" };
  for (gsize i = 0; i < G_N_ELEMENTS (default_only); i++)
    if (g_ascii_strcasecmp (scheme, default_only[i]) == 0)
      return FV_LOCATION_DEFAULT_ONLY;
  return FV_LOCATION_SAVE;
}

/* --- Zoom levels ------------------------------------------------------------------------------ */

/* Nautilus 51.1 icon sizes per zoom level (nautilus-enums.h, PLAN.md §10.7). */
static const guint grid_sizes[NU_GRID_ZOOM_MAX + 1] = { 48, 64, 96, 168, 256 };
static const guint list_sizes[NU_LIST_ZOOM_MAX + 1] = { 16, 32, 64 };

int
fv_zoom_for_icon_size (NuViewType view,
                       guint      icon_size)
{
  const guint *sizes = view == NU_VIEW_GRID ? grid_sizes : view == NU_VIEW_LIST ? list_sizes : NULL;
  int n = view == NU_VIEW_GRID ? NU_GRID_ZOOM_MAX + 1 : NU_LIST_ZOOM_MAX + 1;
  for (int i = 0; sizes != NULL && i < n; i++)
    if (sizes[i] == icon_size)
      return i;
  return -1;
}

guint
fv_icon_size_for_zoom (NuViewType view,
                       int        zoom)
{
  if (view == NU_VIEW_GRID && zoom >= 0 && zoom <= NU_GRID_ZOOM_MAX)
    return grid_sizes[zoom];
  if (view == NU_VIEW_LIST && zoom >= 0 && zoom <= NU_LIST_ZOOM_MAX)
    return list_sizes[zoom];
  return 0;
}

int
fv_folder_view_zoom (const NuFolderView *v)
{
  return v->view == NU_VIEW_GRID ? v->grid_zoom : v->list_zoom;
}

/* --- Cache ------------------------------------------------------------------------------------ */

typedef struct
{
  char        *uri; /* Owned; also the hash table key (not duplicated there), freed with the entry */
  gboolean     has_view;
  NuFolderView view;
} Entry;

struct _FvCache
{
  guint       max;
  GQueue      order; /* Of Entry*, most recently used first; entries owned through this queue */
  GHashTable *index; /* uri (borrowed from the entry) → GList* link in @order; owned */
};

/* Frees one entry and its URI. */
static void
entry_free (Entry *e)
{
  g_free (e->uri);
  g_free (e);
}

/* Returns a new cache; caller frees with fv_cache_free. */
FvCache *
fv_cache_new (guint max_entries)
{
  g_return_val_if_fail (max_entries > 0, NULL);
  FvCache *c = g_new0 (FvCache, 1);
  c->max = max_entries;
  g_queue_init (&c->order);
  c->index = g_hash_table_new (g_str_hash, g_str_equal);
  return c;
}

void
fv_cache_clear (FvCache *cache)
{
  g_hash_table_remove_all (cache->index);
  g_queue_clear_full (&cache->order, (GDestroyNotify) entry_free);
}

void
fv_cache_free (FvCache *cache)
{
  if (cache == NULL)
    return;
  fv_cache_clear (cache);
  g_hash_table_unref (cache->index);
  g_free (cache);
}

FvCacheResult
fv_cache_lookup (FvCache      *cache,
                 const char   *uri,
                 NuFolderView *out)
{
  GList *link = uri != NULL ? g_hash_table_lookup (cache->index, uri) : NULL;
  if (link == NULL)
    return FV_CACHE_MISS;
  g_queue_unlink (&cache->order, link);
  g_queue_push_head_link (&cache->order, link);
  Entry *e = link->data;
  if (!e->has_view)
    return FV_CACHE_NONE;
  *out = e->view;
  return FV_CACHE_SAVED;
}

void
fv_cache_store (FvCache            *cache,
                const char         *uri,
                const NuFolderView *v)
{
  g_return_if_fail (uri != NULL);
  g_return_if_fail (v == NULL || nu_folder_view_valid (v));
  GList *link = g_hash_table_lookup (cache->index, uri);
  if (link != NULL)
    {
      g_queue_unlink (&cache->order, link);
      g_queue_push_head_link (&cache->order, link);
    }
  else
    {
      Entry *e = g_new0 (Entry, 1); /* owned by @order from here on */
      e->uri = g_strdup (uri);
      g_queue_push_head (&cache->order, e);
      g_hash_table_insert (cache->index, e->uri, cache->order.head);
      link = cache->order.head;
    }
  Entry *e = link->data;
  e->has_view = v != NULL;
  if (v != NULL)
    e->view = *v;

  while (cache->order.length > cache->max)
    {
      Entry *old = g_queue_pop_tail (&cache->order);
      g_hash_table_remove (cache->index, old->uri);
      entry_free (old);
    }
}

guint
fv_cache_size (FvCache *cache)
{
  return cache->order.length;
}
