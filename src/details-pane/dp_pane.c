/* Details pane widget (PLAN.md §13.1.2, §13.1.3). See dp_pane.h.
 *
 * Lifetime: every async read gets a ReadCtx with a weak ref to the pane and the pane's generation at the time
 * it started; dp_pane_show() bumps the generation and cancels the previous reads, so a late result is
 * dropped. The video player is owned by the pane and stopped on show, unmap and dispose. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime (CLOCK_THREAD_CPUTIME_ID) */
#include "dp_pane.h"

#include <time.h>

#include "config.h"
#include "dp_format.h"
#include "dp_meta.h"
#include "dp_stack.h"
#include "dp_thumb.h"

#define PREVIEW_ICON_SIZE 128 /* px, logical: file icons in the preview */
#define FOLDER_RENDER_SIZE 192 /* px, logical: folder previews in the preview area */
#define ICON_CARD_PX      256 /* px: an icon card of the multi-selection stack (PLAN.md §13.10 P11) */
#define BOTTOM_PREVIEW_W  220 /* px, logical: the preview box at the bottom position */
#define BOTTOM_GROUP_W    250 /* px, logical: one group column at the bottom position */
#define HEADER_ICON_SIZE  32

struct _DpPane
{
  GtkWidget       parent_instance;

  GtkWidget      *scroller;      /* Our only child (unparented in dispose). Everything below is borrowed
                                  * from the widget tree under it. */
  GtkWidget      *preview;       /* GtkOverlay */
  GtkWidget      *picture;       /* Single item */
  GtkWidget      *content;       /* Box: [preview][info] — vertical at the right, horizontal at the bottom */
  GtkWidget      *info_box;      /* Box: [header][groups] */
  GtkWidget      *play;          /* Play button over a video's thumbnail */
  GtkWidget      *controls;      /* Pause / sound buttons while playing */
  GtkWidget      *pause_button;
  GtkWidget      *mute_button;
  GtkWidget      *icon;          /* Header icon */
  GtkWidget      *title;
  GtkWidget      *subtitle;
  GtkWidget      *groups;        /* Box of groups: [details][extra…][permissions], or the selection summary */
  GtkWidget      *details_w;     /* Borrowed (in @groups) or NULL: the "Details" group */
  GtkWidget      *perms_w;       /* Borrowed (in @groups) or NULL: the "Permissions" group */

  int             preferred_width;
  int             preferred_height;
  GtkOrientation  layout;        /* VERTICAL: right of the view; HORIZONTAL: Windows 7 strip at the bottom */
  GCancellable   *cancel;        /* Owned: the reads for what is shown now */
  guint           generation;    /* Bumped by every show */
  guint           pending;       /* Reads of this generation still running */
  GFile          *shown;         /* Owned or NULL: the single item shown (for re-show checks) */
  DpInfo         *info;          /* Owned ref or NULL: single item */
  GPtrArray      *extra;         /* Owned or NULL: DpGroup* for @info */
  DpFolderCount   count;
  gboolean        has_count;
  gboolean        count_failed;
  DpSummary      *summary;       /* Owned or NULL: multiple selection */
  GdkPaintable   *still;         /* Owned or NULL: the preview when no video plays */
  const char     *preview_kind;  /* Static: "none", "icon", "thumbnail", "folder-preview", "stack", "video" */
  GtkMediaStream *media;         /* Owned or NULL: the playing video */
};

G_DEFINE_FINAL_TYPE (DpPane, dp_pane, GTK_TYPE_WIDGET)

static DpFolderRenderer folder_renderer;  /* Process-wide (main thread). */
static DpPane          *playing;          /* Weak pointer: the one pane playing a video (PLAN.md §13.3 rule 6). */
static GtkCssProvider  *css;              /* Owned: added to the default display once. */
static GHashTable      *count_cache;      /* Owned: "uri\nmtime" (owned) → DpFolderCount* (owned). */
static gint64           max_main_us;      /* Longest main-thread step (dp_pane_get_max_main_us). */
static DpPaneStyle      pane_style = {    /* Process-wide look (dp_pane_set_style); = the schema defaults */
  .border = TRUE, .margin = 0, .padding = 18, .radius = 0, .preview_radius = 12, .preview_margin = 0,
  .stack_depth = 1 /* blur */, .stack_shadow = TRUE, .stack_rotation = TRUE, .stack_keep_aspect = TRUE,
  .stack_fade = 0, .stack_card = TRUE,
};
static GPtrArray       *panes;            /* Borrowed DpPane* (each removes itself in dispose): restyled live. */
static GHashTable      *retiring;         /* Owned: GtkMediaStream* (owned ref) → its release timeout id. */

/* Main-thread CPU time in µs (wall time also counts waiting for the CPU while workers/thumbnailers run). */
static gint64
thread_cpu_us (void)
{
  struct timespec ts;
  return clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? (gint64) ts.tv_sec * 1000000 + ts.tv_nsec / 1000 : 0;
}

#define TIME_BEGIN() gint64 dp_t0_ = g_get_monotonic_time (), dp_c0_ = thread_cpu_us ()
#define TIME_END(what)                                                                                    \
  G_STMT_START                                                                                            \
  {                                                                                                       \
    gint64 dp_dt_ = g_get_monotonic_time () - dp_t0_;                                                     \
    max_main_us = MAX (max_main_us, dp_dt_);                                                              \
    if (dp_dt_ > 16000)                                                                                   \
      g_debug ("slow main-thread step: %s took %" G_GINT64_FORMAT " µs (cpu %" G_GINT64_FORMAT " µs)",     \
               (what), dp_dt_, thread_cpu_us () - dp_c0_);                                                \
  }                                                                                                       \
  G_STMT_END

gint64
dp_pane_get_max_main_us (void)
{
  return max_main_us;
}

static const char style_sheet[] =
  ".dp-preview { background-color: alpha(currentColor, 0.05); }\n"
  "dp-pane.dp-floating { background-color: var(--card-bg-color); }\n"
  "dp-pane.dp-edge-start { border-left: 1px solid var(--border-color); }\n"
  "dp-pane.dp-edge-top { border-top: 1px solid var(--border-color); }\n"
  "dp-pane.dp-edge-all { border: 1px solid var(--border-color); }\n"
  /* Our paned's own line is replaced by the pane's border (on/off, P13); the handle stays draggable. */
  "paned.dp-paned > separator { background: none; background-image: none; box-shadow: none; }\n"
  /* P15: a line under Files' content header bar while the pane is shown (class set by the hook). */
  "headerbar.dp-header-line { box-shadow: inset 0 -1px var(--border-color); }\n"
  ".dp-play { min-width: 56px; min-height: 56px; -gtk-icon-size: 24px; }\n"
  ".dp-chip { border-radius: 9999px; padding: 3px 12px; background-color: alpha(currentColor, 0.08);"
  " font-size: 0.9em; }\n"
  ".dp-chip.dp-none { background-color: alpha(var(--error-color), 0.12); color: var(--error-color); }\n"
  ".dp-stack-card { border-radius: 8px; background-color: var(--card-bg-color);"
  " box-shadow: 0 1px 4px alpha(black, 0.25); }\n"
  ".dp-group-title { margin-bottom: 2px; }\n";

/* The fixed sheet plus the sizes from the options. */
static void
load_css (void)
{
  g_autofree char *sizes = g_strdup_printf (
    "dp-pane { margin: %dpx; border-radius: %dpx; }\n"
    ".dp-preview, .dp-preview picture.dp-video { border-radius: %dpx; }\n"
    ".dp-preview { margin: %dpx; }\n",
    pane_style.margin, pane_style.radius, pane_style.preview_radius, pane_style.preview_margin);
  g_autofree char *all = g_strconcat (style_sheet, sizes, NULL);
  gtk_css_provider_load_from_string (css, all);
}

static void
ensure_style (void)
{
  if (css != NULL)
    return;
  GdkDisplay *display = gdk_display_get_default ();
  if (display == NULL)
    return;
  css = gtk_css_provider_new ();
  load_css ();
  gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

void
dp_pane_unload_style (void)
{
  /* Feature shutdown: release retired players now. */
  if (retiring != NULL)
    {
      GHashTableIter it;
      gpointer media, id;
      g_hash_table_iter_init (&it, retiring);
      while (g_hash_table_iter_next (&it, &media, &id))
        {
          g_source_remove (GPOINTER_TO_UINT (id));
          g_object_unref (media);
        }
      g_clear_pointer (&retiring, g_hash_table_unref);
    }
  if (css != NULL)
    {
      GdkDisplay *display = gdk_display_get_default ();
      if (display != NULL)
        gtk_style_context_remove_provider_for_display (display, GTK_STYLE_PROVIDER (css));
      g_clear_object (&css);
    }
  g_clear_pointer (&count_cache, g_hash_table_unref);
}

void
dp_pane_set_folder_renderer (DpFolderRenderer renderer)
{
  folder_renderer = renderer;
}

/* ------------------------------------------------------------------------------------------------ */
/* Async plumbing                                                                                   */

typedef struct
{
  GWeakRef pane;
  guint    generation;
} ReadCtx;

/* New context for a read started now; consumed by take_pane(). */
static ReadCtx *
read_ctx (DpPane *self)
{
  ReadCtx *ctx = g_new0 (ReadCtx, 1);
  g_weak_ref_init (&ctx->pane, self);
  ctx->generation = self->generation;
  self->pending++;
  return ctx;
}

/* Frees @ctx; returns a new ref to its pane if the read is still current, else NULL. */
static DpPane *
take_pane (ReadCtx *ctx)
{
  DpPane *self = g_weak_ref_get (&ctx->pane);
  guint generation = ctx->generation;
  g_weak_ref_clear (&ctx->pane);
  g_free (ctx);
  if (self == NULL)
    return NULL;
  if (generation != self->generation)
    {
      g_object_unref (self);
      return NULL;
    }
  self->pending--;
  return self;
}

/* ------------------------------------------------------------------------------------------------ */
/* Building blocks                                                                                  */

static GtkWidget *
dim_label (const char *text)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_widget_add_css_class (l, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_set_valign (l, GTK_ALIGN_START);
  return l;
}

static GtkWidget *
group_box (const char *title)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *h = g_object_new (GTK_TYPE_LABEL, "label", title, "accessible-role", GTK_ACCESSIBLE_ROLE_HEADING, NULL);
  gtk_widget_add_css_class (h, "heading");
  gtk_widget_add_css_class (h, "dp-group-title");
  gtk_label_set_xalign (GTK_LABEL (h), 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (h), GTK_ACCESSIBLE_PROPERTY_LEVEL, 3, -1);
  gtk_box_append (GTK_BOX (box), h);
  return box;
}

/* A group of "label  value" rows, Explorer style: dim label left, value right. */
static GtkWidget *
build_group (const DpGroup *g)
{
  GtkWidget *box = group_box (g->title);
  gtk_widget_set_valign (box, GTK_ALIGN_START);
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  for (guint i = 0; i < g->rows->len; i++)
    {
      const DpRow *row = g_ptr_array_index (g->rows, i);
      GtkWidget *label = dim_label (row->label);
      GtkWidget *value = gtk_label_new (row->value);
      gtk_label_set_xalign (GTK_LABEL (value), 1);
      gtk_label_set_justify (GTK_LABEL (value), GTK_JUSTIFY_RIGHT);
      gtk_label_set_wrap (GTK_LABEL (value), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (value), PANGO_WRAP_WORD_CHAR);
      gtk_widget_set_hexpand (value, TRUE);
      gtk_widget_set_halign (value, GTK_ALIGN_END);
      if (row->tooltip != NULL)
        gtk_widget_set_tooltip_text (value, row->tooltip);
      gtk_accessible_update_relation (GTK_ACCESSIBLE (value), GTK_ACCESSIBLE_RELATION_LABELLED_BY, label, NULL, -1);
      gtk_grid_attach (GTK_GRID (grid), label, 0, (int) i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), value, 1, (int) i, 1, 1);
    }
  gtk_box_append (GTK_BOX (box), grid);
  return box;
}

/* "Owner · louis" + its chips below (screenshot 3 style). Takes @chips. */
static void
add_chip_row (GtkWidget *box, const char *who, const char *name, GStrv chips)
{
  g_autofree char *heading = name != NULL ? g_strdup_printf ("%s · %s", who, name) : g_strdup (who);
  gtk_box_append (GTK_BOX (box), dim_label (heading));
  GtkWidget *wrap = adw_wrap_box_new ();
  adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (wrap), 6);
  adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (wrap), 6);
  gtk_widget_set_margin_bottom (wrap, 6);
  for (guint i = 0; chips[i] != NULL; i++)
    {
      GtkWidget *chip = gtk_label_new (chips[i]);
      gtk_widget_add_css_class (chip, "dp-chip");
      if (g_str_equal (chips[i], "No access"))
        gtk_widget_add_css_class (chip, "dp-none");
      g_autofree char *a11y = dp_chip_accessible_label (who, chips[i]);
      gtk_accessible_update_property (GTK_ACCESSIBLE (chip), GTK_ACCESSIBLE_PROPERTY_LABEL, a11y, -1);
      adw_wrap_box_append (ADW_WRAP_BOX (wrap), chip);
    }
  gtk_box_append (GTK_BOX (box), wrap);
  g_strfreev (chips);
}

static GtkWidget *
build_permissions (const DpInfo *info)
{
  const DpPerms *p = &info->perms;
  if (!p->has_access && !p->has_mode)
    return NULL;
  GtkWidget *box = group_box ("Permissions");
  if (p->has_access)
    add_chip_row (box, "You", NULL, dp_access_chips (p->can_read, p->can_write, p->can_execute, info->is_dir));
  if (p->has_mode)
    {
      add_chip_row (box, "Owner", p->owner, dp_permission_chips (p->mode, DP_WHO_OWNER, info->is_dir));
      add_chip_row (box, "Group", p->group, dp_permission_chips (p->mode, DP_WHO_GROUP, info->is_dir));
      add_chip_row (box, "Others", NULL, dp_permission_chips (p->mode, DP_WHO_OTHERS, info->is_dir));
      GStrv special = dp_special_chips (p->mode);
      if (special[0] != NULL)
        add_chip_row (box, "Special", NULL, special);
      else
        g_strfreev (special);
    }
  return box;
}

static void
clear_box (GtkWidget *box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (box)) != NULL)
    gtk_box_remove (GTK_BOX (box), child);
}

/* The "Details" group from what has been read so far (folder rows wait for the count). */
static GtkWidget *
build_details (DpPane *self)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  const DpFolderCount *count = self->has_count ? &self->count : NULL;
  g_autoptr (DpGroup) details = dp_details_group (self->info, count, now);
  if (self->info->is_dir && self->count_failed)
    {
      /* Can't list it (no permission): drop the "Counting…" rows. */
      for (guint i = details->rows->len; i > 0; i--)
        {
          const DpRow *row = g_ptr_array_index (details->rows, i - 1);
          if (g_str_equal (row->label, "Contents") || g_str_equal (row->label, "Total size"))
            g_ptr_array_remove_index (details->rows, i - 1);
        }
    }
  return build_group (details);
}

/* Bottom layout: every group is a fixed-width column. */
static void
size_groups (DpPane *self)
{
  gboolean bottom = self->layout == GTK_ORIENTATION_HORIZONTAL;
  for (GtkWidget *g = gtk_widget_get_first_child (self->groups); g != NULL; g = gtk_widget_get_next_sibling (g))
    gtk_widget_set_size_request (g, bottom ? BOTTOM_GROUP_W : -1, -1);
}

static void apply_style (DpPane *self);

/* Second half of render_info, one main-loop turn later (keeps each step under a frame, PLAN.md §4.4). */
static gboolean
render_perms_idle (gpointer data)
{
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL || self->info == NULL)
    return G_SOURCE_REMOVE;
  TIME_BEGIN ();
  self->perms_w = build_permissions (self->info);
  if (self->perms_w != NULL)
    gtk_box_append (GTK_BOX (self->groups), self->perms_w); /* always last: extra groups go before it */
  size_groups (self);
  TIME_END ("render (permissions)");
  return G_SOURCE_REMOVE;
}

/* Single item read: details now, permissions in the next turn. Each later read only touches its own group. */
static void
render_info (DpPane *self)
{
  TIME_BEGIN ();
  clear_box (self->groups);
  self->details_w = build_details (self);
  gtk_box_append (GTK_BOX (self->groups), self->details_w);
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, render_perms_idle, read_ctx (self), NULL);
  size_groups (self);
  TIME_END ("render (info)");
}

/* Folder counted: replaces only the "Details" group. */
static void
render_count (DpPane *self)
{
  TIME_BEGIN ();
  GtkWidget *fresh = build_details (self);
  gtk_box_insert_child_after (GTK_BOX (self->groups), fresh, self->details_w);
  gtk_box_remove (GTK_BOX (self->groups), self->details_w);
  self->details_w = fresh;
  size_groups (self);
  TIME_END ("render (count)");
}

/* Image / media / PDF groups: inserted between "Details" and "Permissions". */
static void
render_extra (DpPane *self)
{
  TIME_BEGIN ();
  GtkWidget *after = self->details_w;
  for (guint i = 0; self->extra != NULL && i < self->extra->len; i++)
    {
      GtkWidget *g = build_group (g_ptr_array_index (self->extra, i));
      gtk_box_insert_child_after (GTK_BOX (self->groups), g, after);
      after = g;
    }
  size_groups (self);
  TIME_END ("render (extra)");
}

static void
render_summary (DpPane *self)
{
  TIME_BEGIN ();
  clear_box (self->groups);
  gtk_box_append (GTK_BOX (self->groups), build_group (self->summary->group));
  size_groups (self);
  TIME_END ("render (summary)");
}

/* ------------------------------------------------------------------------------------------------ */
/* Preview                                                                                          */

static void
set_still (DpPane *self, GdkPaintable *paintable, const char *kind)
{
  g_set_object (&self->still, paintable);
  self->preview_kind = kind;
  if (self->media == NULL)
    gtk_picture_set_paintable (GTK_PICTURE (self->picture), paintable);
}

/* New ref: @gicon from the theme at @size (NULL if it can't be found). */
static GdkPaintable *
icon_paintable (DpPane *self, GIcon *gicon, int size)
{
  if (gicon == NULL)
    return NULL;
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (self)));
  GtkIconPaintable *p = gtk_icon_theme_lookup_by_gicon (theme, gicon, size, gtk_widget_get_scale_factor (GTK_WIDGET (self)),
                                                        gtk_widget_get_direction (GTK_WIDGET (self)), 0);
  return GDK_PAINTABLE (p);
}

static void
update_play_button (DpPane *self)
{
  gboolean video = self->info != NULL && self->info->kind == DP_KIND_VIDEO && self->info->is_native &&
                   self->summary == NULL;
  gtk_widget_set_visible (self->play, video && self->media == NULL);
  gtk_widget_set_visible (self->controls, self->media != NULL);
}

static void
on_thumb (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) texture = dp_thumb_load_finish (res, &error);
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL || texture == NULL)
    return;
  set_still (self, GDK_PAINTABLE (texture), "thumbnail");
}

/* Shows the single item's preview: icon now, then the thumbnail or the folder preview. */
static void
start_preview (DpPane *self)
{
  DpInfo *info = self->info;
  g_autoptr (GdkPaintable) icon = icon_paintable (self, info->icon, PREVIEW_ICON_SIZE);
  set_still (self, icon, icon != NULL ? "icon" : "none");
  if (info->is_dir)
    {
      if (folder_renderer != NULL && !info->custom_icon)
        {
          g_autoptr (GdkPaintable) folder_icon = icon_paintable (self, info->icon, FOLDER_RENDER_SIZE);
          g_autoptr (GdkPaintable) rendered = folder_icon != NULL
            ? folder_renderer (info->file, folder_icon, FOLDER_RENDER_SIZE,
                               gtk_widget_get_scale_factor (GTK_WIDGET (self)))
            : NULL;
          if (rendered != NULL)
            set_still (self, rendered, "folder-preview");
        }
      return;
    }
  /* Thumbnails: native files only; made if missing (PLAN.md §13.3 rule 3). */
  if (info->is_native)
    dp_thumb_load_async (info->uri, info->content_type, info->mtime_s, TRUE, self->cancel, on_thumb, read_ctx (self));
}

/* New texture: @info's icon on a translucent card (P11: optional) for a stack card without a thumbnail. Main
 * thread (icon theme). Drawn with cairo: icon paintables only produce texture/mask nodes, which it renders. */
static GdkTexture *
icon_card (DpPane *self, const DpInfo *info)
{
  const int px = ICON_CARD_PX;
  int icon_px = pane_style.stack_card ? px * 3 / 5 : px * 4 / 5;
  g_autoptr (GdkPaintable) icon = icon_paintable (self, info->icon, icon_px);
  GtkSnapshot *snap = gtk_snapshot_new (); /* consumed below */
  if (pane_style.stack_card)
    {
      GdkRGBA fg;
      gtk_widget_get_color (GTK_WIDGET (self), &fg);
      fg.alpha = 0.10f;
      GskRoundedRect r;
      gsk_rounded_rect_init_from_rect (&r, &GRAPHENE_RECT_INIT (0, 0, px, px), px / 10.0f);
      gtk_snapshot_push_rounded_clip (snap, &r);
      gtk_snapshot_append_color (snap, &fg, &r.bounds);
      gtk_snapshot_pop (snap);
    }
  if (icon != NULL)
    {
      gtk_snapshot_save (snap);
      gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT ((px - icon_px) / 2.0f, (px - icon_px) / 2.0f));
      gdk_paintable_snapshot (icon, snap, icon_px, icon_px);
      gtk_snapshot_restore (snap);
    }
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snap);
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
  if (node != NULL)
    {
      cairo_t *cr = cairo_create (surface);
      gsk_render_node_draw (node, cr);
      cairo_destroy (cr);
    }
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  g_autoptr (GBytes) bytes = g_bytes_new (cairo_image_surface_get_data (surface), (gsize) stride * px);
  cairo_surface_destroy (surface);
  /* GDK_MEMORY_DEFAULT = cairo's ARGB32 (gdkmemorytexture.h). */
  return gdk_memory_texture_new (px, px, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
}

static void
on_stack (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) frame = dp_stack_render_finish (res, &error);
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL || frame == NULL)
    return;
  set_still (self, GDK_PAINTABLE (frame), "stack");
}

/* Several selected: the folder-preview stack (P10) of the first items, rendered in a worker. */
static void
start_stack (DpPane *self)
{
  guint n = MIN (self->summary->infos->len, DP_STACK_MAX);
  DpStackItem items[DP_STACK_MAX] = { { NULL, 0, FALSE, NULL, 0 } };
  GdkTexture *cards[DP_STACK_MAX] = { NULL }; /* owned, released below (the job takes its own refs) */
  for (guint i = 0; i < n; i++)
    {
      const DpInfo *info = g_ptr_array_index (self->summary->infos, i);
      cards[i] = icon_card (self, info);
      items[i].uri = info->uri;
      items[i].mtime = info->mtime_s;
      /* Existing thumbnails only: a selection of many files shouldn't start thumbnailers (D5). */
      items[i].thumbnail = info->is_native && !info->is_dir;
      items[i].fallback = cards[i];
      items[i].seed = g_str_hash (info->uri);
    }
  FpStyle style;
  dp_stack_style_init (&style, pane_style.stack_depth, pane_style.stack_shadow, pane_style.stack_rotation,
                       pane_style.stack_keep_aspect, pane_style.stack_fade);
  int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
  int logical = self->layout == GTK_ORIENTATION_HORIZONTAL
                  ? BOTTOM_PREVIEW_W
                  : CLAMP (self->preferred_width - 2 * (pane_style.padding + pane_style.preview_margin) - 20, 120, 300);
  g_autoptr (GdkPaintable) placeholder = icon_paintable (self, ((DpInfo *) g_ptr_array_index (self->summary->infos, 0))->icon,
                                                          PREVIEW_ICON_SIZE);
  set_still (self, placeholder, "icon");
  if (n > 0)
    dp_stack_render_async (&style, items, n, (guint) (logical * scale), self->cancel, on_stack, read_ctx (self));
  for (guint i = 0; i < n; i++)
    g_clear_object (&cards[i]);
}

/* ------------------------------------------------------------------------------------------------ */
/* Video                                                                                            */

static void
sync_media_buttons (DpPane *self)
{
  if (self->media == NULL)
    return;
  gboolean on = gtk_media_stream_get_playing (self->media);
  gtk_button_set_icon_name (GTK_BUTTON (self->pause_button),
                            on ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text (self->pause_button, on ? "Pause" : "Play");
  gboolean muted = gtk_media_stream_get_muted (self->media);
  gtk_button_set_icon_name (GTK_BUTTON (self->mute_button),
                            muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
  gtk_widget_set_tooltip_text (self->mute_button, muted ? "Unmute" : "Mute");
}

static void
on_media_notify (GObject *media, GParamSpec *pspec, gpointer data)
{
  (void) media;
  (void) pspec;
  sync_media_buttons (data);
}

/* Loops the preview by hand: gtk_media_stream_set_loop() leaks a GstStructure in GTK 4.24's GStreamer backend
 * (gtk_gst_media_file_update_loop → gst_play_get_config) on every call. */
static void
on_media_ended (GObject *media, GParamSpec *pspec, gpointer data)
{
  (void) pspec;
  (void) data;
  GtkMediaStream *stream = GTK_MEDIA_STREAM (media);
  if (gtk_media_stream_get_ended (stream) && gtk_media_stream_is_seekable (stream))
    {
      gtk_media_stream_seek (stream, 0);
      gtk_media_stream_play (stream);
    }
}

static gboolean
release_media (gpointer media)
{
  g_hash_table_steal (retiring, media);
  g_object_unref (media); /* the ref retire_media() took over */
  return G_SOURCE_REMOVE;
}

/* Drops a stopped player a moment later: releasing GTK's GStreamer player right while it is still busy
 * (e.g. seeking for our loop) crashed in GstPlay's own thread once in the smoke test (PLAN.md §13.11).
 * Takes @media's reference. */
static void
retire_media (GtkMediaStream *media)
{
  if (retiring == NULL)
    retiring = g_hash_table_new (NULL, NULL);
  guint id = g_timeout_add (1500, release_media, media);
  g_hash_table_insert (retiring, media, GUINT_TO_POINTER (id));
}

void
dp_pane_stop_media (DpPane *self)
{
  g_return_if_fail (DP_IS_PANE (self));
  if (self->media != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->media, self);
      gtk_media_stream_pause (self->media);
      retire_media (g_steal_pointer (&self->media));
      gtk_widget_remove_css_class (self->picture, "dp-video");
      gtk_picture_set_paintable (GTK_PICTURE (self->picture), self->still);
    }
  if (playing == self)
    g_clear_weak_pointer (&playing);
  update_play_button (self);
}

static void
on_play (GtkButton *button, gpointer data)
{
  (void) button;
  DpPane *self = data;
  if (self->info == NULL || self->media != NULL)
    return;
  if (playing != NULL && playing != self)
    dp_pane_stop_media (playing);
  self->media = gtk_media_file_new_for_file (self->info->file);
  gtk_media_stream_set_muted (self->media, TRUE);
  g_signal_connect (self->media, "notify::ended", G_CALLBACK (on_media_ended), self);
  g_signal_connect (self->media, "notify::playing", G_CALLBACK (on_media_notify), self);
  g_signal_connect (self->media, "notify::muted", G_CALLBACK (on_media_notify), self);
  gtk_widget_add_css_class (self->picture, "dp-video");
  gtk_picture_set_paintable (GTK_PICTURE (self->picture), GDK_PAINTABLE (self->media));
  gtk_media_stream_play (self->media);
  g_set_weak_pointer (&playing, self);
  update_play_button (self);
  sync_media_buttons (self);
}

static void
on_pause (GtkButton *button, gpointer data)
{
  (void) button;
  DpPane *self = data;
  if (self->media == NULL)
    return;
  if (gtk_media_stream_get_playing (self->media))
    gtk_media_stream_pause (self->media);
  else
    gtk_media_stream_play (self->media);
}

static void
on_mute (GtkButton *button, gpointer data)
{
  (void) button;
  DpPane *self = data;
  if (self->media != NULL)
    gtk_media_stream_set_muted (self->media, !gtk_media_stream_get_muted (self->media));
}

gboolean
dp_pane_play (DpPane *self)
{
  g_return_val_if_fail (DP_IS_PANE (self), FALSE);
  if (!gtk_widget_get_visible (self->play))
    return FALSE;
  on_play (GTK_BUTTON (self->play), self);
  return self->media != NULL;
}

gboolean
dp_pane_is_playing (DpPane *self)
{
  g_return_val_if_fail (DP_IS_PANE (self), FALSE);
  return self->media != NULL && gtk_media_stream_get_playing (self->media);
}

/* ------------------------------------------------------------------------------------------------ */
/* Reads                                                                                            */

static char *
count_key (const DpInfo *info)
{
  return g_strdup_printf ("%s\n%" G_GUINT64_FORMAT, info->uri, info->mtime_s);
}

static void
on_count (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  DpFolderCount count;
  gboolean ok = dp_folder_count_finish (res, &count, &error);
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL)
    return;
  if (!ok)
    {
      g_debug ("cannot count %s: %s", self->info->uri, error->message);
      self->count_failed = TRUE;
    }
  else
    {
      self->count = count;
      self->has_count = TRUE;
      if (count_cache == NULL)
        count_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      if (g_hash_table_size (count_cache) >= DP_COUNT_CACHE_MAX)
        g_hash_table_remove_all (count_cache);
      g_hash_table_replace (count_cache, count_key (self->info), g_memdup2 (&count, sizeof count));
    }
  render_count (self);
}

static void
on_extra (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  GPtrArray *extra = dp_extra_read_finish (res, &error);
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL)
    {
      g_clear_pointer (&extra, g_ptr_array_unref);
      return;
    }
  g_clear_pointer (&self->extra, g_ptr_array_unref);
  self->extra = extra; /* taken (may be NULL on error) */
  render_extra (self);
}

static void
on_info (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  DpInfo *info = dp_info_read_finish (res, &error);
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL)
    {
      g_clear_pointer (&info, dp_info_unref);
      return;
    }
  if (info == NULL)
    {
      gtk_label_set_text (GTK_LABEL (self->subtitle), error->message);
      return;
    }
  self->info = info; /* taken */
  gtk_label_set_text (GTK_LABEL (self->title), info->display_name);
  gtk_label_set_text (GTK_LABEL (self->subtitle), info->description != NULL ? info->description : "");
  gtk_image_set_from_gicon (GTK_IMAGE (self->icon), info->icon);
  start_preview (self);
  if (info->is_dir)
    {
      g_autofree char *key = count_key (info);
      DpFolderCount *cached = count_cache != NULL ? g_hash_table_lookup (count_cache, key) : NULL;
      if (cached != NULL)
        {
          self->count = *cached;
          self->has_count = TRUE;
        }
      else
        dp_folder_count_async (info->file, self->cancel, on_count, read_ctx (self));
    }
  else
    dp_extra_read_async (info, self->cancel, on_extra, read_ctx (self));
  update_play_button (self);
  render_info (self);
}

static void
on_summary (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  DpSummary *summary = dp_summary_read_finish (res, &error);
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL)
    {
      g_clear_pointer (&summary, dp_summary_free);
      return;
    }
  if (summary == NULL)
    return;
  self->summary = summary; /* taken */
  const DpRow *contains = dp_group_find (summary->group, "Contains");
  gtk_label_set_text (GTK_LABEL (self->subtitle), contains != NULL ? contains->value : "");
  start_stack (self);
  render_summary (self);
}

/* Forgets what was shown and cancels its reads. */
static void
reset (DpPane *self)
{
  dp_pane_stop_media (self);
  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  self->generation++;
  self->pending = 0;
  g_clear_object (&self->shown);
  g_clear_pointer (&self->info, dp_info_unref);
  g_clear_pointer (&self->extra, g_ptr_array_unref);
  g_clear_pointer (&self->summary, dp_summary_free);
  self->has_count = FALSE;
  self->count_failed = FALSE;
  self->details_w = self->perms_w = NULL;
  set_still (self, NULL, "none");
}

void
dp_pane_show (DpPane    *self,
              GFile     *folder,
              GPtrArray *files,
              guint      n_selected)
{
  g_return_if_fail (DP_IS_PANE (self));
  TIME_BEGIN ();
  GFile *single = files != NULL && files->len == 1 ? g_ptr_array_index (files, 0)
                  : (files == NULL || files->len == 0) ? folder : NULL;
  reset (self);
  self->cancel = g_cancellable_new ();
  clear_box (self->groups);
  gtk_widget_set_visible (self->play, FALSE);
  gtk_widget_set_visible (self->controls, FALSE);
  if (single != NULL)
    {
      self->shown = g_object_ref (single);
      g_autofree char *base = g_file_get_basename (single);
      gtk_label_set_text (GTK_LABEL (self->title), base != NULL ? base : "");
      gtk_label_set_text (GTK_LABEL (self->subtitle), "");
      gtk_image_clear (GTK_IMAGE (self->icon));
      dp_info_read_async (single, self->cancel, on_info, read_ctx (self));
    }
  else if (files != NULL && files->len > 1)
    {
      g_autofree char *title = g_strdup_printf ("%u items selected", MAX (n_selected, files->len));
      gtk_label_set_text (GTK_LABEL (self->title), title);
      gtk_label_set_text (GTK_LABEL (self->subtitle), "");
      gtk_image_set_from_icon_name (GTK_IMAGE (self->icon), "edit-select-all-symbolic");
      dp_summary_read_async (files, MAX (n_selected, files->len), self->cancel, on_summary, read_ctx (self));
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->title), "");
      gtk_label_set_text (GTK_LABEL (self->subtitle), "");
      gtk_image_clear (GTK_IMAGE (self->icon));
    }
  TIME_END ("show");
}

guint
dp_pane_get_n_pending (DpPane *self)
{
  g_return_val_if_fail (DP_IS_PANE (self), 0);
  return self->pending;
}

char *
dp_pane_describe (DpPane *self)
{
  g_return_val_if_fail (DP_IS_PANE (self), NULL);
  GString *s = g_string_new (NULL);
  g_string_append_printf (s, "title=\"%s\" subtitle=\"%s\" preview=%s playing=%d pending=%u",
                          gtk_label_get_text (GTK_LABEL (self->title)), gtk_label_get_text (GTK_LABEL (self->subtitle)),
                          self->preview_kind, dp_pane_is_playing (self), self->pending);
  /* Walk the rendered widgets, so this describes what is on screen. */
  for (GtkWidget *g = gtk_widget_get_first_child (self->groups); g != NULL; g = gtk_widget_get_next_sibling (g))
    {
      GtkWidget *heading = gtk_widget_get_first_child (g);
      g_string_append_printf (s, " [%s:", gtk_label_get_text (GTK_LABEL (heading)));
      for (GtkWidget *c = gtk_widget_get_next_sibling (heading); c != NULL; c = gtk_widget_get_next_sibling (c))
        {
          if (GTK_IS_GRID (c))
            for (GtkWidget *cell = gtk_widget_get_first_child (c); cell != NULL; cell = gtk_widget_get_next_sibling (cell))
              {
                int col = 0;
                gtk_grid_query_child (GTK_GRID (c), cell, &col, NULL, NULL, NULL);
                g_string_append_printf (s, col == 0 ? " %s=" : "%s;", gtk_label_get_text (GTK_LABEL (cell)));
              }
          else if (GTK_IS_LABEL (c))
            g_string_append_printf (s, " %s=", gtk_label_get_text (GTK_LABEL (c)));
          else if (ADW_IS_WRAP_BOX (c))
            {
              for (GtkWidget *chip = gtk_widget_get_first_child (c); chip != NULL; chip = gtk_widget_get_next_sibling (chip))
                g_string_append_printf (s, "%s,", gtk_label_get_text (GTK_LABEL (chip)));
              g_string_append_c (s, ';');
            }
        }
      g_string_append_c (s, ']');
    }
  return g_string_free (s, FALSE);
}

/* ------------------------------------------------------------------------------------------------ */
/* Widget                                                                                           */

/* Classes, padding and layout of one pane from pane_style and its layout. */
static void
apply_style (DpPane *self)
{
  GtkWidget *w = GTK_WIDGET (self);
  gboolean floating = pane_style.margin > 0;
  gboolean bottom = self->layout == GTK_ORIENTATION_HORIZONTAL;
  gtk_widget_remove_css_class (w, "dp-edge-start");
  gtk_widget_remove_css_class (w, "dp-edge-top");
  gtk_widget_remove_css_class (w, "dp-edge-all");
  if (pane_style.border)
    gtk_widget_add_css_class (w, floating ? "dp-edge-all" : bottom ? "dp-edge-top" : "dp-edge-start");
  if (floating)
    gtk_widget_add_css_class (w, "dp-floating");
  else
    gtk_widget_remove_css_class (w, "dp-floating");
  gtk_widget_set_overflow (w, pane_style.radius > 0 ? GTK_OVERFLOW_HIDDEN : GTK_OVERFLOW_VISIBLE);
  int p = pane_style.padding;
  gtk_widget_set_margin_start (self->content, p);
  gtk_widget_set_margin_end (self->content, p);
  gtk_widget_set_margin_top (self->content, p);
  gtk_widget_set_margin_bottom (self->content, p);

  /* Right: one column. Bottom (Windows 7): preview | name + groups as columns (P14). */
  GtkOrientation o = self->layout;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->content), o);
  gtk_box_set_spacing (GTK_BOX (self->content), bottom ? 24 : 18);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->groups), o);
  gtk_box_set_spacing (GTK_BOX (self->groups), bottom ? 32 : 18);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller), bottom ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (self->preview, bottom ? BOTTOM_PREVIEW_W : -1, bottom ? -1 : DP_PREVIEW_MAX_HEIGHT - 40);
  gtk_widget_set_valign (self->preview, bottom ? GTK_ALIGN_START : GTK_ALIGN_FILL);
  gtk_widget_set_size_request (self->picture, -1, bottom ? 120 : DP_PREVIEW_MAX_HEIGHT - 60);
  size_groups (self);
}

void
dp_pane_set_style (const DpPaneStyle *style)
{
  g_return_if_fail (style != NULL);
  gboolean stack_changed = style->stack_depth != pane_style.stack_depth ||
                           style->stack_shadow != pane_style.stack_shadow ||
                           style->stack_rotation != pane_style.stack_rotation ||
                           style->stack_keep_aspect != pane_style.stack_keep_aspect ||
                           style->stack_fade != pane_style.stack_fade || style->stack_card != pane_style.stack_card;
  pane_style = *style;
  pane_style.padding = CLAMP (pane_style.padding, 0, 64);
  if (css != NULL)
    load_css ();
  for (guint i = 0; panes != NULL && i < panes->len; i++)
    {
      DpPane *pane = g_ptr_array_index (panes, i);
      apply_style (pane);
      if (stack_changed && pane->summary != NULL)
        start_stack (pane);
    }
}

void
dp_pane_set_layout (DpPane         *self,
                    GtkOrientation  layout)
{
  g_return_if_fail (DP_IS_PANE (self));
  if (self->layout == layout)
    return;
  self->layout = layout;
  apply_style (self);
  if (self->summary != NULL)
    start_stack (self); /* its size depends on the layout */
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
dp_pane_set_preferred_height (DpPane *self,
                              int     height)
{
  g_return_if_fail (DP_IS_PANE (self));
  height = MAX (height, DP_PANE_MIN_HEIGHT);
  if (height == self->preferred_height)
    return;
  self->preferred_height = height;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
dp_pane_set_preferred_width (DpPane *self,
                             int     width)
{
  g_return_if_fail (DP_IS_PANE (self));
  width = MAX (width, DP_PANE_MIN_WIDTH);
  if (width == self->preferred_width)
    return;
  self->preferred_width = width;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
dp_pane_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                 int *min_baseline, int *nat_baseline)
{
  DpPane *self = DP_PANE (widget);
  gtk_widget_measure (self->scroller, orientation, for_size, min, nat, min_baseline, nat_baseline);
  if (self->layout == GTK_ORIENTATION_VERTICAL && orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *min = MAX (*min, DP_PANE_MIN_WIDTH);
      *nat = MAX (*min, self->preferred_width);
    }
  else if (self->layout == GTK_ORIENTATION_HORIZONTAL && orientation == GTK_ORIENTATION_VERTICAL)
    {
      *min = DP_PANE_MIN_HEIGHT; /* the content scrolls */
      *nat = MAX (*min, self->preferred_height);
    }
}

static void
dp_pane_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (DP_PANE (widget)->scroller, width, height, baseline, NULL);
}

static void
dp_pane_unmap (GtkWidget *widget)
{
  dp_pane_stop_media (DP_PANE (widget)); /* closing the pane, the tab or the window stops the video */
  GTK_WIDGET_CLASS (dp_pane_parent_class)->unmap (widget);
}

static void
dp_pane_dispose (GObject *object)
{
  DpPane *self = DP_PANE (object);
  if (panes != NULL)
    g_ptr_array_remove (panes, self);
  if (self->scroller != NULL)
    {
      reset (self);
      g_clear_pointer (&self->scroller, gtk_widget_unparent);
    }
  G_OBJECT_CLASS (dp_pane_parent_class)->dispose (object);
}

static void
dp_pane_class_init (DpPaneClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = dp_pane_dispose;
  widget_class->measure = dp_pane_measure;
  widget_class->size_allocate = dp_pane_size_allocate;
  widget_class->unmap = dp_pane_unmap;
  gtk_widget_class_set_css_name (widget_class, "dp-pane");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_REGION);
}

static GtkWidget *
osd_button (const char *icon, const char *tooltip)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_add_css_class (b, "osd");
  gtk_widget_add_css_class (b, "circular");
  gtk_widget_set_tooltip_text (b, tooltip);
  return b;
}

static void
dp_pane_init (DpPane *self)
{
  ensure_style ();
  self->preferred_width = DP_WIDTH_DEFAULT;
  self->preview_kind = "none";
  gtk_widget_add_css_class (GTK_WIDGET (self), "dp-pane");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, "Details", -1);

  self->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_parent (self->scroller, GTK_WIDGET (self));

  self->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  GtkWidget *content = self->content;
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), content);

  /* Preview area. */
  self->preview = gtk_overlay_new ();
  gtk_widget_add_css_class (self->preview, "dp-preview");
  gtk_widget_set_overflow (self->preview, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_size_request (self->preview, -1, DP_PREVIEW_MAX_HEIGHT - 40);
  GtkWidget *holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (holder, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (holder, GTK_ALIGN_FILL);
  self->picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_SCALE_DOWN);
  gtk_picture_set_can_shrink (GTK_PICTURE (self->picture), TRUE);
  gtk_widget_set_size_request (self->picture, -1, DP_PREVIEW_MAX_HEIGHT - 60);
  gtk_widget_set_margin_top (self->picture, 10);
  gtk_widget_set_margin_bottom (self->picture, 10);
  gtk_widget_set_margin_start (self->picture, 10);
  gtk_widget_set_margin_end (self->picture, 10);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->picture), GTK_ACCESSIBLE_PROPERTY_LABEL, "Preview", -1);
  gtk_box_append (GTK_BOX (holder), self->picture);
  gtk_overlay_set_child (GTK_OVERLAY (self->preview), holder);

  self->play = osd_button ("media-playback-start-symbolic", "Play Preview");
  gtk_widget_add_css_class (self->play, "dp-play");
  gtk_widget_set_halign (self->play, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->play, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->play, FALSE);
  g_signal_connect (self->play, "clicked", G_CALLBACK (on_play), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->preview), self->play);

  self->controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (self->controls, GTK_ALIGN_END);
  gtk_widget_set_valign (self->controls, GTK_ALIGN_END);
  gtk_widget_set_margin_end (self->controls, 8);
  gtk_widget_set_margin_bottom (self->controls, 8);
  self->pause_button = osd_button ("media-playback-pause-symbolic", "Pause");
  g_signal_connect (self->pause_button, "clicked", G_CALLBACK (on_pause), self);
  self->mute_button = osd_button ("audio-volume-muted-symbolic", "Unmute");
  g_signal_connect (self->mute_button, "clicked", G_CALLBACK (on_mute), self);
  gtk_box_append (GTK_BOX (self->controls), self->pause_button);
  gtk_box_append (GTK_BOX (self->controls), self->mute_button);
  gtk_widget_set_visible (self->controls, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->preview), self->controls);
  gtk_box_append (GTK_BOX (content), self->preview);

  /* Header: icon, name, type. */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  self->icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (self->icon), HEADER_ICON_SIZE);
  gtk_widget_set_valign (self->icon, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (header), self->icon);
  GtkWidget *names = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (names, TRUE);
  self->title = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->title, "title-3");
  gtk_label_set_xalign (GTK_LABEL (self->title), 0);
  gtk_label_set_wrap (GTK_LABEL (self->title), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (self->title), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable (GTK_LABEL (self->title), TRUE);
  self->subtitle = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->subtitle, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (self->subtitle), 0);
  gtk_label_set_wrap (GTK_LABEL (self->subtitle), TRUE);
  gtk_box_append (GTK_BOX (names), self->title);
  gtk_box_append (GTK_BOX (names), self->subtitle);
  gtk_box_append (GTK_BOX (header), names);
  self->info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_hexpand (self->info_box, TRUE);
  gtk_box_append (GTK_BOX (self->info_box), header);
  gtk_box_append (GTK_BOX (content), self->info_box);

  self->groups = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_box_append (GTK_BOX (self->info_box), self->groups);

  self->layout = GTK_ORIENTATION_VERTICAL;
  self->preferred_height = 220;
  if (panes == NULL)
    panes = g_ptr_array_new ();
  g_ptr_array_add (panes, self);
  apply_style (self);
}

GtkWidget *
dp_pane_new (void)
{
  return g_object_new (DP_TYPE_PANE, NULL);
}
