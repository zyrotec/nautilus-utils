/* Details pane widget (PLAN.md §13.1.2, §13.1.3). See dp_pane.h.
 *
 * Lifetime: every async read gets a ReadCtx with a weak ref to the pane and the pane's generation at the time
 * it started; dp_pane_show() bumps the generation and cancels the previous reads, so a late result is
 * dropped. The video player is owned by the pane and stopped on show, unmap and dispose. */
#define _GNU_SOURCE 1 /* RUSAGE_THREAD (dp_timing.h) */
#include "dp_pane.h"

#include <math.h>
#include <time.h>

#include "config.h"
#include "dp_format.h"
#include "dp_box.h"
#include "dp_meta.h"
#include "dp_backdrop.h"
#include "dp_stack_view.h"
#include "dp_thumb.h"
#include "dp_timing.h"

#define ICON_BOX_SPACE    48  /* px: the icon box is the icon size plus this (right layout) */
#define MEDIA_MAX_HEIGHT  300 /* px: the tallest a media box gets at the right */
#define FIXED_BOX_HEIGHT  220 /* px: the box when media sit flush inside a fixed box (P20) */
#define HEADER_ICON_SIZE  32

struct _DpPane
{
  GtkWidget       parent_instance;

  GtkWidget      *scroller;      /* Our only child (unparented in dispose). Everything below is borrowed
                                  * from the widget tree under it. */
  GtkWidget      *box;           /* DpBox around @preview: media in its own shape, icons in a fixed box (P17) */
  GtkWidget      *preview;       /* GtkOverlay (the visible box: background, radius) */
  GtkWidget      *names;         /* Box: title + subtitle */
  GtkWidget      *pairs;         /* GtkGrid: the Windows 7 summary at the bottom (P19) */
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
  GtkWidget      *backdrop;      /* DpBackdrop: the box's blurred background (P28) */
  GtkWidget      *stack;         /* DpStackView: several selected (P24–P27) */
  guint           stack_art;     /* Folders drawn with their previews in the stack (P23) */
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
  .stack_fade = 0, .stack_card = TRUE, .icon_size = 128, .stack_border_color = { 1, 1, 1, 1 },
  .stack_border_width = 2, .stack_corner_radius = 4, .stack_spacing = 12,
};
static GPtrArray       *panes;            /* Borrowed DpPane* (each removes itself in dispose): restyled live. */
static GHashTable      *retiring;         /* Owned: GtkMediaStream* (owned ref) → its release timeout id. */

#define TIME_BEGIN() DpStepSample dp_s0_; dp_step_sample (&dp_s0_)
#define TIME_END(what) G_STMT_START { gint64 dp_dt_ = dp_step_end (&dp_s0_, (what)); max_main_us = MAX (max_main_us, dp_dt_); } G_STMT_END

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

static void apply_style (DpPane *self);
static void render_pairs (DpPane *self);

static gboolean
is_bottom (DpPane *self)
{
  return self->layout == GTK_ORIENTATION_HORIZONTAL;
}

/* Second half of render_info, one main-loop turn later (keeps each step under a frame, PLAN.md §4.4). */
static gboolean
render_perms_idle (gpointer data)
{
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL || self->info == NULL || is_bottom (self))
    return G_SOURCE_REMOVE;
  TIME_BEGIN ();
  self->perms_w = build_permissions (self->info);
  if (self->perms_w != NULL)
    gtk_box_append (GTK_BOX (self->groups), self->perms_w); /* always last: extra groups go before it */
  TIME_END ("render (permissions)");
  return G_SOURCE_REMOVE;
}

/* Single item read: details now, permissions in the next turn. Each later read only touches its own group.
 * Bottom: the Windows 7 summary instead (P19). */
static void
render_info (DpPane *self)
{
  if (is_bottom (self))
    {
      render_pairs (self);
      return;
    }
  TIME_BEGIN ();
  clear_box (self->groups);
  self->details_w = build_details (self);
  gtk_box_append (GTK_BOX (self->groups), self->details_w);
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, render_perms_idle, read_ctx (self), NULL);
  TIME_END ("render (info)");
}

/* Folder counted: replaces only the "Details" group. */
static void
render_count (DpPane *self)
{
  if (is_bottom (self) || self->details_w == NULL)
    {
      render_pairs (self);
      return;
    }
  TIME_BEGIN ();
  GtkWidget *fresh = build_details (self);
  gtk_box_insert_child_after (GTK_BOX (self->groups), fresh, self->details_w);
  gtk_box_remove (GTK_BOX (self->groups), self->details_w);
  self->details_w = fresh;
  TIME_END ("render (count)");
}

/* Image / media / PDF groups: inserted between "Details" and "Permissions". */
static void
render_extra (DpPane *self)
{
  if (is_bottom (self) || self->details_w == NULL)
    {
      render_pairs (self);
      return;
    }
  TIME_BEGIN ();
  GtkWidget *after = self->details_w;
  for (guint i = 0; self->extra != NULL && i < self->extra->len; i++)
    {
      GtkWidget *g = build_group (g_ptr_array_index (self->extra, i));
      gtk_box_insert_child_after (GTK_BOX (self->groups), g, after);
      after = g;
    }
  TIME_END ("render (extra)");
}

static void
render_summary (DpPane *self)
{
  if (is_bottom (self))
    {
      render_pairs (self);
      return;
    }
  TIME_BEGIN ();
  clear_box (self->groups);
  gtk_box_append (GTK_BOX (self->groups), build_group (self->summary->group));
  TIME_END ("render (summary)");
}

/* After a layout change: everything that has been read, in the new layout. */
static void
render_all (DpPane *self)
{
  self->details_w = self->perms_w = NULL;
  clear_box (self->groups);
  if (self->summary != NULL)
    render_summary (self);
  else if (self->info != NULL)
    {
      render_info (self);
      if (!is_bottom (self) && self->extra != NULL)
        render_extra (self);
    }
  else
    render_pairs (self);
}

/* ---- Windows 7 summary (bottom, P19) ---- */

/* @label's value in @g (borrowed) or NULL. */
static const char *
row_value (const DpGroup *g, const char *label)
{
  const DpRow *r = g != NULL ? dp_group_find (g, label) : NULL;
  return r != NULL ? r->value : NULL;
}

static void
pair_take (GPtrArray *pairs, const char *label, const char *value)
{
  if (value == NULL || *value == '\0')
    return;
  g_ptr_array_add (pairs, g_strdup (label));
  g_ptr_array_add (pairs, g_strdup (value));
}

/* "64 × 48 px" from "64 px" / "48 px". New string or NULL. */
static char *
join_size (const char *w, const char *h)
{
  if (w == NULL || h == NULL)
    return NULL;
  return g_strdup_printf ("%d × %d px", atoi (w), atoi (h));
}

/* The few properties the strip shows, as label, value, label, value… (owned strings). */
static GPtrArray *
bottom_pairs (DpPane *self)
{
  GPtrArray *pairs = g_ptr_array_new_with_free_func (g_free);
  if (self->summary != NULL)
    {
      const DpGroup *g = self->summary->group;
      pair_take (pairs, "Size", row_value (g, "Size (files)"));
      pair_take (pairs, "Contains", row_value (g, "Contains"));
      pair_take (pairs, "Location", row_value (g, "Location"));
      return pairs;
    }
  if (self->info == NULL)
    return pairs;
  const DpGroup *x = self->extra != NULL && self->extra->len > 0 ? g_ptr_array_index (self->extra, 0) : NULL;
  switch (self->info->kind)
    {
    case DP_KIND_IMAGE:
      pair_take (pairs, "Dimensions", row_value (x, "Dimensions"));
      pair_take (pairs, "Date taken", row_value (x, "Date taken"));
      pair_take (pairs, "Camera", row_value (x, "Camera"));
      break;
    case DP_KIND_VIDEO:
    case DP_KIND_AUDIO:
      pair_take (pairs, "Length", row_value (x, "Duration"));
      if (x != NULL && g_strcmp0 (x->title, "Video") == 0)
        {
          g_autofree char *dims = join_size (row_value (x, "Width"), row_value (x, "Height"));
          pair_take (pairs, "Dimensions", dims);
          pair_take (pairs, "Codec", row_value (x, "Video codec"));
        }
      else
        {
          pair_take (pairs, "Artist", row_value (x, "Artist"));
          pair_take (pairs, "Album", row_value (x, "Album"));
        }
      break;
    case DP_KIND_PDF:
      pair_take (pairs, "Pages", row_value (x, "Pages"));
      pair_take (pairs, "Title", row_value (x, "Title"));
      pair_take (pairs, "Author", row_value (x, "Author"));
      break;
    default:
      break;
    }
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (DpGroup) d = dp_details_group (self->info, self->has_count ? &self->count : NULL, now);
  if (self->info->is_dir)
    {
      pair_take (pairs, "Contents", self->count_failed ? NULL : row_value (d, "Contents"));
      pair_take (pairs, "Size", self->count_failed ? NULL : row_value (d, "Total size"));
    }
  else
    pair_take (pairs, "Size", row_value (d, "Size"));
  pair_take (pairs, "Modified", row_value (d, "Modified"));
  const DpPerms *p = &self->info->perms;
  if (p->has_access)
    {
      g_auto (GStrv) chips = dp_access_chips (p->can_read, p->can_write, p->can_execute, self->info->is_dir);
      g_autofree char *joined = g_strjoinv (", ", chips);
      pair_take (pairs, "You can", joined);
    }
  return pairs;
}

/* Rows that fit the strip: its height minus the padding, one row per ~24 px. */
static int
pair_rows (DpPane *self)
{
  int h = self->preferred_height - 2 * pane_style.padding;
  return CLAMP ((h + 6) / 24, 1, 6);
}

/* Bottom: the pairs in columns, filled top to bottom then left to right (Windows 7). */
static void
render_pairs (DpPane *self)
{
  TIME_BEGIN ();
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->pairs)) != NULL)
    gtk_grid_remove (GTK_GRID (self->pairs), child);
  g_autoptr (GPtrArray) pairs = bottom_pairs (self);
  int rows = pair_rows (self);
  for (guint i = 0; i + 1 < pairs->len; i += 2)
    {
      int n = (int) i / 2, col = (n / rows) * 2, row = n % rows;
      GtkWidget *l = dim_label (g_ptr_array_index (pairs, i));
      gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
      if (col > 0)
        gtk_widget_set_margin_start (l, 24);
      GtkWidget *v = gtk_label_new (g_ptr_array_index (pairs, i + 1));
      gtk_label_set_xalign (GTK_LABEL (v), 0);
      gtk_label_set_ellipsize (GTK_LABEL (v), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (v), 32);
      gtk_widget_set_tooltip_text (v, g_ptr_array_index (pairs, i + 1));
      gtk_accessible_update_relation (GTK_ACCESSIBLE (v), GTK_ACCESSIBLE_RELATION_LABELLED_BY, l, NULL, -1);
      gtk_grid_attach (GTK_GRID (self->pairs), l, col, row, 1, 1);
      gtk_grid_attach (GTK_GRID (self->pairs), v, col + 1, row, 1, 1);
    }
  TIME_END ("render (pairs)");
}

/* ------------------------------------------------------------------------------------------------ */
/* Preview                                                                                          */

/* Media kinds fill a box of their own shape (P17); icons sit in a fixed box at the icon size. */
static gboolean
is_media_kind (const char *kind)
{
  return g_strcmp0 (kind, "thumbnail") == 0;
}

static gboolean
is_stack (DpPane *self)
{
  return g_strcmp0 (self->preview_kind, "stack") == 0;
}

/* P28: the box's background from the picture shown (pictures and video posters; a fixed box shows it best). */
static void
sync_backdrop (DpPane *self)
{
  gboolean on = pane_style.backdrop && is_media_kind (self->preview_kind) && self->still != NULL;
  dp_backdrop_set_paintable (DP_BACKDROP (self->backdrop), on ? self->still : NULL);
}

static void
sync_box (DpPane *self)
{
  double ratio = 0;
  int icon_h = pane_style.icon_size + ICON_BOX_SPACE;
  gboolean stack = is_stack (self);
  gtk_widget_set_visible (self->stack, stack);
  gtk_widget_set_visible (self->picture, !stack);
  sync_backdrop (self);
  if (stack)
    {
      /* The stack keeps one shape for 2 or 3 items, so adding one never resizes the box. */
      int h = 0;
      ratio = dp_stack_view_get_ratio (DP_STACK_VIEW (self->stack), &h);
      if (ratio <= 0)
        icon_h = h;
    }
  gboolean media = (is_media_kind (self->preview_kind) && self->still != NULL) || (stack && ratio > 0);
  if (pane_style.media_box)
    {
      /* P20: one fixed box for everything; a picture touches its edges on one side, centred on the other. */
      dp_box_set_icon_height (DP_BOX (self->box), MAX (FIXED_BOX_HEIGHT, MAX (pane_style.icon_size + ICON_BOX_SPACE,
                                                                               stack ? icon_h : 0)));
      dp_box_set_ratio (DP_BOX (self->box), 0);
      gtk_picture_set_content_fit (GTK_PICTURE (self->picture),
                                   media ? GTK_CONTENT_FIT_CONTAIN : GTK_CONTENT_FIT_SCALE_DOWN);
      return;
    }
  dp_box_set_icon_height (DP_BOX (self->box), icon_h);
  if (media && !stack)
    ratio = gdk_paintable_get_intrinsic_aspect_ratio (self->still); /* the video keeps its poster's shape */
  dp_box_set_ratio (DP_BOX (self->box), ratio);
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture),
                               ratio > 0 && !stack ? GTK_CONTENT_FIT_FILL : GTK_CONTENT_FIT_SCALE_DOWN);
}

static void
set_still (DpPane *self, GdkPaintable *paintable, const char *kind)
{
  g_set_object (&self->still, paintable);
  self->preview_kind = kind;
  if (self->media == NULL)
    gtk_picture_set_paintable (GTK_PICTURE (self->picture), paintable);
  sync_box (self);
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
start_preview (DpPane *self, gboolean load_thumbnail)
{
  DpInfo *info = self->info;
  g_autoptr (GdkPaintable) icon = icon_paintable (self, info->icon, pane_style.icon_size);
  set_still (self, icon, icon != NULL ? "icon" : "none");
  if (info->is_dir)
    {
      if (folder_renderer != NULL && !info->custom_icon)
        {
          g_autoptr (GdkPaintable) folder_icon = icon_paintable (self, info->icon, pane_style.icon_size);
          g_autoptr (GdkPaintable) rendered = folder_icon != NULL
            ? folder_renderer (info->file, folder_icon, pane_style.icon_size,
                               gtk_widget_get_scale_factor (GTK_WIDGET (self)))
            : NULL;
          if (rendered != NULL)
            set_still (self, rendered, "folder-preview");
        }
      return;
    }
  /* Thumbnails: native files only; made if missing (PLAN.md §13.3 rule 3). */
  if (info->is_native && load_thumbnail)
    dp_thumb_load_async (info->uri, info->content_type, info->mtime_s, TRUE, self->cancel, on_thumb, read_ctx (self));
}

/* A stack item's thumbnail: existing ones only (a selection of many files shouldn't start thumbnailers, D5). */
typedef struct
{
  ReadCtx *ctx;
  char    *key;
} StackThumb;

static void
on_stack_thumb (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  StackThumb *st = data;
  g_autofree char *key = st->key;
  ReadCtx *ctx = st->ctx;
  g_free (st);
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) texture = dp_thumb_load_finish (res, &error);
  g_autoptr (DpPane) self = take_pane (ctx);
  if (self == NULL || texture == NULL)
    return;
  dp_stack_view_set_thumbnail (DP_STACK_VIEW (self->stack), key, texture);
  sync_box (self);
}

/* P23: a folder with a folder preview is drawn with its pictures (new ref), else NULL (its icon). */
static GdkPaintable *
stack_folder_art (DpPane *self, const DpInfo *info, int size)
{
  if (!info->is_dir || info->custom_icon || folder_renderer == NULL)
    return NULL;
  int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
  g_autoptr (GdkPaintable) folder_icon = icon_paintable (self, info->icon, size);
  return folder_icon != NULL ? folder_renderer (info->file, folder_icon, size, scale) : NULL;
}

static gboolean
is_stack_media (const DpInfo *info)
{
  return info->is_native && !info->is_dir &&
         (info->kind == DP_KIND_IMAGE || info->kind == DP_KIND_VIDEO || info->kind == DP_KIND_PDF);
}

/* Several selected: the first items as a stack of live layers (P24): their icons (folders with their previews)
 * at once, thumbnails cross-faded in as they load. Items already shown keep their place and thumbnail, so
 * adding one to the selection only moves the others. */
static void
start_stack (DpPane *self)
{
  guint n = MIN (self->summary->infos->len, DP_STACK_VIEW_MAX);
  DpStackViewItem items[DP_STACK_VIEW_MAX] = { { NULL, NULL, FALSE, 0 } };
  GdkPaintable *icons[DP_STACK_VIEW_MAX] = { NULL }; /* owned, released below (the view takes its own refs) */
  self->stack_art = 0;
  double size = pane_style.icon_size;
  for (guint i = 0; i < n; i++, size *= 0.85)
    {
      const DpInfo *info = g_ptr_array_index (self->summary->infos, i);
      icons[i] = stack_folder_art (self, info, (int) size);
      if (icons[i] != NULL)
        self->stack_art++;
      else
        icons[i] = icon_paintable (self, info->icon, MAX ((int) size, 16));
      items[i].key = info->uri;
      items[i].icon = icons[i];
      items[i].media = is_stack_media (info);
      items[i].seed = g_str_hash (info->uri);
    }
  DpStackView *view = DP_STACK_VIEW (self->stack);
  dp_stack_view_set_items (view, items, n);
  set_still (self, NULL, "stack");
  for (guint i = 0; i < n; i++)
    {
      g_clear_object (&icons[i]);
      const DpInfo *info = g_ptr_array_index (self->summary->infos, i);
      if (!items[i].media || dp_stack_view_has_thumbnail (view, info->uri))
        continue;
      StackThumb *st = g_new0 (StackThumb, 1); /* consumed by on_stack_thumb */
      st->ctx = read_ctx (self);
      st->key = g_strdup (info->uri);
      dp_thumb_load_async (info->uri, info->content_type, info->mtime_s, FALSE, self->cancel, on_stack_thumb, st);
    }
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

/* Second step of a single item: the reads that run in workers. */
static gboolean
start_reads_idle (gpointer data)
{
  g_autoptr (DpPane) self = take_pane (data);
  if (self == NULL || self->info == NULL)
    return G_SOURCE_REMOVE;
  DpInfo *info = self->info;
  if (info->is_dir)
    {
      if (!self->has_count)
        dp_folder_count_async (info->file, self->cancel, on_count, read_ctx (self));
    }
  else
    {
      if (info->is_native)
        dp_thumb_load_async (info->uri, info->content_type, info->mtime_s, TRUE, self->cancel, on_thumb, read_ctx (self));
      dp_extra_read_async (info, self->cancel, on_extra, read_ctx (self));
    }
  return G_SOURCE_REMOVE;
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
  start_preview (self, FALSE);
  if (info->is_dir)
    {
      g_autofree char *key = count_key (info);
      DpFolderCount *cached = count_cache != NULL ? g_hash_table_lookup (count_cache, key) : NULL;
      if (cached != NULL)
        {
          self->count = *cached;
          self->has_count = TRUE;
        }
    }
  update_play_button (self);
  render_info (self);
  /* The workers (thumbnail, media / PDF readers, counting) start after the rows are built: started together, their
   * libraries' allocations contend with ours for glibc's malloc locks and the step took 17–22 ms of wall time on
   * 5 ms of CPU (PLAN.md §13.14 S1). Queued after the permissions idle. */
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, start_reads_idle, read_ctx (self), NULL);
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

/* Forgets what was shown and cancels its reads. @keep_stack: several items stay selected, so the stack, the
 * subtitle and the summary stay on screen until the new summary replaces them (no flicker, P27). */
static void
reset (DpPane *self, gboolean keep_stack)
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
  keep_stack = keep_stack && is_stack (self);
  if (!keep_stack)
    {
      dp_stack_view_set_items (DP_STACK_VIEW (self->stack), NULL, 0);
      self->stack_art = 0;
    }
  set_still (self, NULL, keep_stack ? "stack" : "none");
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
  gboolean multi = files != NULL && files->len > 1;
  gboolean keep = multi && is_stack (self);
  reset (self, keep);
  self->cancel = g_cancellable_new ();
  if (!keep)
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
      if (!keep)
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
  if (gtk_widget_get_visible (self->pairs))
    {
      /* The bottom summary: label=value; in reading order (column by column). */
      g_string_append (s, " [Summary:");
      for (int col = 0;; col += 2)
        {
          gboolean any = FALSE;
          for (int row = 0; row < 8; row++)
            {
              GtkWidget *l = gtk_grid_get_child_at (GTK_GRID (self->pairs), col, row);
              GtkWidget *v = gtk_grid_get_child_at (GTK_GRID (self->pairs), col + 1, row);
              if (l == NULL || v == NULL)
                continue;
              any = TRUE;
              g_string_append_printf (s, " %s=%s;", gtk_label_get_text (GTK_LABEL (l)), gtk_label_get_text (GTK_LABEL (v)));
            }
          if (!any)
            break;
        }
      g_string_append_c (s, ']');
    }
  g_string_append_printf (s, " box=%s stack-art=%s backdrop=%s", dp_box_get_ratio_is_media (DP_BOX (self->box)) ? "media" : "icon",
                          self->stack_art > 0 ? "yes" : "no",
                          dp_backdrop_get_paintable (DP_BACKDROP (self->backdrop)) != NULL ? "yes" : "no");
  if (is_stack (self))
    {
      DpStackView *v = DP_STACK_VIEW (self->stack);
      g_string_append_printf (s, " stack=%s:%u thumbs=%u", dp_stack_view_get_shape (v), dp_stack_view_get_n_items (v),
                              dp_stack_view_get_n_thumbnails (v));
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

  /* Right: one column. Bottom: the Windows 7 summary — box | name, type | pairs in columns (P19). */
  GtkOrientation o = self->layout;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->content), o);
  gtk_box_set_spacing (GTK_BOX (self->content), bottom ? 18 : 18);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->info_box), o);
  gtk_box_set_spacing (GTK_BOX (self->info_box), bottom ? 32 : 18);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller), bottom ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
                                  bottom ? GTK_POLICY_NEVER : GTK_POLICY_AUTOMATIC);
  dp_box_set_layout (DP_BOX (self->box), o);
  DpStackStyle ss = {
    .icon_size = pane_style.icon_size,
    .border = pane_style.stack_border,
    .border_color = pane_style.stack_border_color,
    .border_width = pane_style.stack_border_width,
    .corner_radius = pane_style.stack_corner_radius,
    .shadow = pane_style.stack_shadow,
    .rotation = pane_style.stack_rotation,
    .keep_aspect = pane_style.stack_keep_aspect,
    .depth = pane_style.stack_depth,
    .fade = pane_style.stack_fade,
    .card = pane_style.stack_card,
    .media_square = pane_style.stack_media_square,
    .spacing = pane_style.stack_spacing,
  };
  dp_stack_view_set_style (DP_STACK_VIEW (self->stack), &ss);
  sync_box (self);
  int strip = MAX (self->preferred_height - 2 * pane_style.padding, 24);
  dp_box_set_max_height (DP_BOX (self->box), bottom ? strip : MEDIA_MAX_HEIGHT);
  gtk_widget_set_valign (self->box, bottom ? GTK_ALIGN_FILL : GTK_ALIGN_START);
  gtk_widget_set_visible (self->icon, !bottom && pane_style.show_icon); /* the box is the icon at the bottom */
  /* P21: centred name and type (right layout). */
  gboolean centre = pane_style.title_centered && !bottom;
  gtk_label_set_xalign (GTK_LABEL (self->title), centre ? 0.5f : 0.0f);
  gtk_label_set_xalign (GTK_LABEL (self->subtitle), centre ? 0.5f : 0.0f);
  gtk_label_set_justify (GTK_LABEL (self->title), centre ? GTK_JUSTIFY_CENTER : GTK_JUSTIFY_LEFT);
  gtk_label_set_justify (GTK_LABEL (self->subtitle), centre ? GTK_JUSTIFY_CENTER : GTK_JUSTIFY_LEFT);
  gtk_widget_set_visible (self->groups, !bottom);
  gtk_widget_set_visible (self->pairs, bottom);
  gtk_widget_set_valign (self->names, bottom ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
  gtk_widget_set_hexpand (self->names, !bottom);
  gtk_widget_set_hexpand (self->info_box, TRUE);
  gtk_widget_set_valign (self->pairs, GTK_ALIGN_CENTER);
  gtk_label_set_wrap (GTK_LABEL (self->title), !bottom);
  gtk_label_set_ellipsize (GTK_LABEL (self->title), bottom ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_NONE);
  gtk_label_set_max_width_chars (GTK_LABEL (self->title), bottom ? 28 : -1);
  if (bottom)
    {
      gtk_widget_remove_css_class (self->title, "title-3");
      gtk_widget_add_css_class (self->title, "heading");
    }
  else
    {
      gtk_widget_remove_css_class (self->title, "heading");
      gtk_widget_add_css_class (self->title, "title-3");
    }
}

void
dp_pane_set_style (const DpPaneStyle *style)
{
  g_return_if_fail (style != NULL);
  gboolean icon_changed = style->icon_size != pane_style.icon_size;
  pane_style = *style;
  pane_style.icon_size = CLAMP (pane_style.icon_size, 16, 256);
  pane_style.padding = CLAMP (pane_style.padding, 0, 64);
  if (css != NULL)
    load_css ();
  for (guint i = 0; panes != NULL && i < panes->len; i++)
    {
      DpPane *pane = g_ptr_array_index (panes, i);
      apply_style (pane);
      if (icon_changed && pane->summary != NULL)
        start_stack (pane); /* icons looked up at the new size */
      else if (icon_changed && pane->info != NULL)
        start_preview (pane, TRUE);
      sync_box (pane);
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
  render_all (self);
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
  apply_style (self); /* the box and the summary rows follow the strip's height */
  if (is_bottom (self))
    render_pairs (self);
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
      reset (self, FALSE);
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
  self->box = dp_box_new ();
  self->preview = gtk_overlay_new ();
  gtk_widget_add_css_class (self->preview, "dp-preview");
  gtk_widget_set_overflow (self->preview, GTK_OVERFLOW_HIDDEN);
  dp_box_set_child (DP_BOX (self->box), self->preview);
  /* No padding inside: media fill the box (P17); icons are centred at their own size. */
  self->picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_SCALE_DOWN);
  gtk_picture_set_can_shrink (GTK_PICTURE (self->picture), TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->picture), GTK_ACCESSIBLE_PROPERTY_LABEL, "Preview", -1);
  self->backdrop = dp_backdrop_new ();
  gtk_overlay_set_child (GTK_OVERLAY (self->preview), self->backdrop);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->preview), self->picture);
  self->stack = dp_stack_view_new ();
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->stack), GTK_ACCESSIBLE_PROPERTY_LABEL, "Selected items", -1);
  gtk_widget_set_visible (self->stack, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->preview), self->stack);

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
  gtk_box_append (GTK_BOX (content), self->box);

  /* Header: icon, name, type. */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  self->icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (self->icon), HEADER_ICON_SIZE);
  gtk_widget_set_valign (self->icon, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (header), self->icon);
  GtkWidget *names = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  self->names = names;
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
  self->pairs = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (self->pairs), 12);
  gtk_grid_set_row_spacing (GTK_GRID (self->pairs), 4);
  gtk_box_append (GTK_BOX (self->info_box), self->pairs);

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
