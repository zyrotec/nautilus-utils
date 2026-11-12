/* Folder views core (PLAN.md §10): the saved-value format, which locations can save a view, icon size ↔
 * zoom level, and an in-memory LRU cache of folder views. GLib only, no Nautilus types (PLAN.md §4.2);
 * main thread only. */
#pragma once

#include <glib.h>

#include "nu_settings.h"

G_BEGIN_DECLS

/* --- Saved value: "<version>;<generation>;<view>;<grid-zoom>;<list-zoom>" (PLAN.md §10.1.2) --- */

/* Encodes @v for @generation. Caller owns the returned string (g_free). @v must be valid. */
char *fv_encode (const NuFolderView *v,
                 guint               generation);

/* Decodes @text. Returns TRUE and fills @out only for a well-formed value of the current format version,
 * of exactly @generation, with a valid view and in-range zoom levels; anything else (NULL, garbage, an older
 * generation after Reset All, a newer format) returns FALSE and leaves @out untouched. */
gboolean fv_decode (const char   *text,
                    guint         generation,
                    NuFolderView *out);

/* --- Locations --- */

typedef enum
{
  FV_LOCATION_SAVE,         /* remembers its own view */
  FV_LOCATION_DEFAULT_ONLY, /* can't store one (starred, other-locations, search): always our default */
  FV_LOCATION_IGNORE,       /* Files' own special view (network:///): never touched */
} FvLocationKind;

/* Classifies a location by its URI (PLAN.md §10.1, §10.7). NULL or empty = FV_LOCATION_IGNORE. */
FvLocationKind fv_location_kind (const char *uri);

/* --- Zoom levels --- */

/* The zoom level of @view shown with @icon_size pixels (Nautilus' "icon-size" property), or -1 if
 * @icon_size isn't one of that view's sizes. */
int fv_zoom_for_icon_size (NuViewType view,
                           guint      icon_size);

/* The icon size in pixels of @zoom in @view, or 0 if out of range. */
guint fv_icon_size_for_zoom (NuViewType view,
                             int        zoom);

/* The zoom level @v uses for its own view type. */
int fv_folder_view_zoom (const NuFolderView *v);

/* --- Cache: URI → saved view, or "known to have none" --- */

typedef enum
{
  FV_CACHE_MISS,  /* not read yet this session */
  FV_CACHE_NONE,  /* read: the folder has no (valid) saved view → our default */
  FV_CACHE_SAVED, /* read or saved: @out holds the folder's view */
} FvCacheResult;

typedef struct _FvCache FvCache;

/* Creates an empty cache holding at most @max_entries (> 0). Free with fv_cache_free(). */
FvCache *fv_cache_new (guint max_entries);

/* Frees the cache and every entry. NULL is ignored. */
void fv_cache_free (FvCache *cache);

/* Looks @uri up and marks it most recently used. Fills @out only for FV_CACHE_SAVED. */
FvCacheResult fv_cache_lookup (FvCache      *cache,
                               const char   *uri,
                               NuFolderView *out);

/* Records @uri's view (@v NULL = known to have none), as most recently used; drops the least recently
 * used entry beyond the limit. Copies @uri and @v. */
void fv_cache_store (FvCache            *cache,
                     const char         *uri,
                     const NuFolderView *v);

/* Forgets every entry (Reset All, feature off). */
void fv_cache_clear (FvCache *cache);

/* Number of entries (tests). */
guint fv_cache_size (FvCache *cache);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FvCache, fv_cache_free)

G_END_DECLS
