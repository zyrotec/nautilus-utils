/* "Nautilus Utils Settings" dialog (PLAN.md §9.1.2, decision N5).
 *
 * AdwDialog > AdwToolbarView: header bar with a view switcher (one tab per feature) and an Apply button;
 * the controls are bound to a *delayed* GSettings (nu_settings_new_delayed), so nothing changes until Apply
 * (g_settings_apply). Closing with unapplied changes reverts them. Narrow dialogs move the tabs to a bottom
 * switcher bar (AdwBreakpoint). Every control has an accessible label; colours come from the theme. */
#include "settings_dialog.h"

#include <adwaita.h>
#include <math.h>

#include "nu_settings.h"

#define DIALOG_KEY "st-settings-dialog" /* window qdata: GWeakRef to the open dialog */

static const char *const colour_mode_nicks[] = { "accent", "custom", "gradient" };
/* Folder preview enums (PLAN.md §11.3), NULL-terminated, in combo row order. */
static const char *const layout_nicks[] = { "grid", "stacked", NULL };
static const char *const stack_step_nicks[] = { "one", "three", NULL };
static const char *const depth_nicks[] = { "none", "blur", "transparency", "darken", NULL };
static const char *const position_nicks[] = { "top", "centre", "bottom", NULL };

/* ---- GSettings ↔ property mappings (all return TRUE when @value was set) ------------------------------ */

static gboolean
mode_to_selected (GValue *value, GVariant *variant, gpointer data)
{
  (void) data;
  const char *nick = g_variant_get_string (variant, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (colour_mode_nicks); i++)
    if (g_strcmp0 (nick, colour_mode_nicks[i]) == 0)
      {
        g_value_set_uint (value, i);
        return TRUE;
      }
  g_value_set_uint (value, 0);
  return TRUE;
}

/* Returns a new floating variant (consumed by GSettings), or NULL to reject the value. */
static GVariant *
selected_to_mode (const GValue *value, const GVariantType *type, gpointer data)
{
  (void) type;
  (void) data;
  guint i = g_value_get_uint (value);
  return i < G_N_ELEMENTS (colour_mode_nicks) ? g_variant_new_string (colour_mode_nicks[i]) : NULL;
}

/* Enum nick → combo row index; @data = NULL-terminated nick array (static). Unknown nick → 0. */
static gboolean
nick_to_selected (GValue *value, GVariant *variant, gpointer data)
{
  const char *const *nicks = data;
  const char *nick = g_variant_get_string (variant, NULL);
  g_value_set_uint (value, 0);
  for (guint i = 0; nicks[i] != NULL; i++)
    if (g_strcmp0 (nick, nicks[i]) == 0)
      g_value_set_uint (value, i);
  return TRUE;
}

/* Combo row index → enum nick. Returns a new floating variant (consumed by GSettings), or NULL to reject. */
static GVariant *
selected_to_nick (const GValue *value, const GVariantType *type, gpointer data)
{
  (void) type;
  const char *const *nicks = data;
  guint i = g_value_get_uint (value);
  for (guint k = 0; nicks[k] != NULL; k++)
    if (k == i)
      return g_variant_new_string (nicks[k]);
  return NULL;
}

/* Enum key == @data (a static nick) → boolean property (e.g. visible only in the stacked layout). */
static gboolean
nick_is (GValue *value, GVariant *variant, gpointer data)
{
  g_value_set_boolean (value, g_strcmp0 (g_variant_get_string (variant, NULL), data) == 0);
  return TRUE;
}

/* Integer key → double property (GtkAdjustment:value). */
static gboolean
int_to_double (GValue *value, GVariant *variant, gpointer data)
{
  (void) data;
  g_value_set_double (value, g_variant_get_int32 (variant));
  return TRUE;
}

/* Double property → integer key, rounded. Returns a new floating variant (consumed by GSettings). */
static GVariant *
double_to_int (const GValue *value, const GVariantType *type, gpointer data)
{
  (void) type;
  (void) data;
  return g_variant_new_int32 ((gint32) lround (g_value_get_double (value)));
}

/* Path key → row subtitle: the file name, or @data (static text) when empty. */
static gboolean
path_to_subtitle (GValue *value, GVariant *variant, gpointer data)
{
  const char *path = g_variant_get_string (variant, NULL);
  if (path == NULL || *path == '\0')
    g_value_set_string (value, data);
  else
    g_value_take_string (value, g_filename_display_basename (path));
  return TRUE;
}

/* Path key → TRUE when set. */
static gboolean
path_is_set (GValue *value, GVariant *variant, gpointer data)
{
  (void) data;
  const char *path = g_variant_get_string (variant, NULL);
  g_value_set_boolean (value, path != NULL && *path != '\0');
  return TRUE;
}

/* bar-colour-mode → visible, for the rows of one mode (@data = the mode, as a pointer-sized int). */
static gboolean
mode_is (GValue *value, GVariant *variant, gpointer data)
{
  const char *nick = g_variant_get_string (variant, NULL);
  g_value_set_boolean (value, g_strcmp0 (nick, colour_mode_nicks[GPOINTER_TO_UINT (data)]) == 0);
  return TRUE;
}

/* Settings value "#rrggbb" → GdkRGBA for GtkColorDialogButton:rgba (@data = fallback NuRgb*). */
static gboolean
string_to_rgba (GValue *value, GVariant *variant, gpointer data)
{
  NuRgb rgb = *(const NuRgb *) data;
  (void) nu_rgb_parse (g_variant_get_string (variant, NULL), &rgb); /* keeps the fallback if invalid */
  GdkRGBA rgba = { rgb.r / 255.0f, rgb.g / 255.0f, rgb.b / 255.0f, 1.0f };
  g_value_set_boxed (value, &rgba); /* copies */
  return TRUE;
}

static guint8
channel (float v)
{
  float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return (guint8) (c * 255.0f + 0.5f);
}

/* Returns a new floating variant (consumed by GSettings). */
static GVariant *
rgba_to_string (const GValue *value, const GVariantType *type, gpointer data)
{
  (void) type;
  (void) data;
  const GdkRGBA *rgba = g_value_get_boxed (value);
  if (rgba == NULL)
    return NULL;
  NuRgb rgb = { channel (rgba->red), channel (rgba->green), channel (rgba->blue) };
  g_autofree char *text = nu_rgb_to_string (rgb);
  return g_variant_new_string (text);
}

/* ---- Building blocks ------------------------------------------------------------------------------------ */

static GtkWidget *
switch_row (GSettings  *settings,
            const char *key,
            const char *title,
            const char *subtitle)
{
  GtkWidget *row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  g_settings_bind (settings, key, row, "active", G_SETTINGS_BIND_DEFAULT);
  return row;
}

/* An action row with a colour button for "#rrggbb" @key (@fallback: static default for invalid values). */
static GtkWidget *
colour_button_row (GSettings   *settings,
                   const char  *key,
                   const char  *title,
                   const NuRgb *fallback)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);

  g_autoptr (GtkColorDialog) dialog = gtk_color_dialog_new ();
  gtk_color_dialog_set_with_alpha (dialog, FALSE);
  gtk_color_dialog_set_title (dialog, title);
  GtkWidget *button = gtk_color_dialog_button_new (g_object_ref (dialog)); /* takes the ref */
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), button);

  /* @fallback points at static defaults (nu_settings_defaults), so no destroy notify is needed. */
  g_settings_bind_with_mapping (settings, key, button, "rgba", G_SETTINGS_BIND_DEFAULT,
                                string_to_rgba, rgba_to_string, (gpointer) fallback, NULL);
  return row;
}

/* A bar colour row for @key, visible only in colour @mode. */
static GtkWidget *
colour_row (GSettings      *settings,
            const char     *key,
            const char     *title,
            const NuRgb    *fallback,
            NuBarColourMode mode)
{
  GtkWidget *row = colour_button_row (settings, key, title, fallback);
  g_settings_bind_with_mapping (settings, "bar-colour-mode", row, "visible", G_SETTINGS_BIND_GET,
                                mode_is, NULL, GUINT_TO_POINTER ((guint) mode), NULL);
  return row;
}

/* A combo row bound to enum @key; @labels and @nicks are NULL-terminated and in the same order (static). */
static GtkWidget *
enum_row (GSettings         *settings,
          const char        *key,
          const char        *title,
          const char *const *labels,
          const char *const *nicks)
{
  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  g_autoptr (GtkStringList) model = gtk_string_list_new (labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (model));
  g_settings_bind_with_mapping (settings, key, row, "selected", G_SETTINGS_BIND_DEFAULT, nick_to_selected,
                                selected_to_nick, (gpointer) nicks, NULL);
  return row;
}

/* GtkScale:format-value: whole numbers with @data (static) as the unit, e.g. "4 px", "30 %". Returns a new
 * string (GTK frees it). */
static char *
format_value (GtkScale *scale, double value, gpointer data)
{
  (void) scale;
  return g_strdup_printf ("%d %s", (int) lround (value), (const char *) data);
}

/* A row with a slider for integer @key in [min, max] (schema range); @unit is static text. */
static GtkWidget *
slider_row (GSettings  *settings,
            const char *key,
            const char *title,
            const char *subtitle,
            int         min,
            int         max,
            const char *unit)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, min, max, 1);
  gtk_scale_set_digits (GTK_SCALE (scale), 0);
  gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
  gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func (GTK_SCALE (scale), format_value, (gpointer) unit, NULL);
  if (min < 0 && max > 0)
    gtk_scale_add_mark (GTK_SCALE (scale), 0, GTK_POS_BOTTOM, NULL);
  gtk_widget_set_size_request (scale, 180, -1);
  gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (scale), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_settings_bind_with_mapping (settings, key, gtk_range_get_adjustment (GTK_RANGE (scale)), "value",
                                G_SETTINGS_BIND_DEFAULT, int_to_double, double_to_int, NULL, NULL);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), scale);
  return row;
}

/* A spin row for integer @key in [min, max] pixels (at the 64 × 64 icon size). */
static GtkWidget *
px_row (GSettings  *settings,
        const char *key,
        const char *title,
        const char *subtitle,
        int         min,
        int         max)
{
  GtkWidget *row = adw_spin_row_new_with_range (min, max, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  g_settings_bind_with_mapping (settings, key, adw_spin_row_get_adjustment (ADW_SPIN_ROW (row)), "value",
                                G_SETTINGS_BIND_DEFAULT, int_to_double, double_to_int, NULL, NULL);
  return row;
}

/* ---- SVG file rows (PLAN.md §11.3) ---------------------------------------------------------------------- */

typedef struct
{
  GSettings  *settings; /* Owned ref: the dialog's delayed settings. */
  const char *key;      /* Static. */
} SvgPick;

static void
svg_pick_free (SvgPick *pick)
{
  g_clear_object (&pick->settings);
  g_free (pick);
}

/* The file chooser closed. A cancelled choice changes nothing. Consumes @data. */
static void
on_svg_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
  SvgPick *pick = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, &error);
  g_autofree char *path = file != NULL ? g_file_get_path (file) : NULL;
  if (path != NULL)
    g_settings_set_string (pick->settings, pick->key, path); /* pending until Apply, like every setting */
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    g_debug ("SVG chooser: %s", error->message);
  svg_pick_free (pick);
}

static void
on_svg_choose (GtkButton *button, gpointer data)
{
  GSettings *settings = data; /* kept alive by the dialog */
  const char *key = g_object_get_data (G_OBJECT (button), "st-key"); /* static string */
  const char *title = g_object_get_data (G_OBJECT (button), "st-title");

  g_autoptr (GtkFileDialog) chooser = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (chooser, title);
  gtk_file_dialog_set_modal (chooser, TRUE);
  g_autoptr (GtkFileFilter) svg = gtk_file_filter_new ();
  gtk_file_filter_set_name (svg, "SVG Images");
  gtk_file_filter_add_mime_type (svg, "image/svg+xml");
  gtk_file_filter_add_suffix (svg, "svg");
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, svg);
  gtk_file_dialog_set_filters (chooser, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (chooser, svg);
  g_autofree char *current = g_settings_get_string (settings, key);
  if (current != NULL && *current != '\0')
    {
      g_autoptr (GFile) file = g_file_new_for_path (current);
      gtk_file_dialog_set_initial_file (chooser, file);
    }

  SvgPick *pick = g_new0 (SvgPick, 1); /* freed by on_svg_chosen */
  pick->settings = g_object_ref (settings);
  pick->key = key;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
  gtk_file_dialog_open (chooser, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, on_svg_chosen, pick);
}

static void
on_svg_clear (GtkButton *button, gpointer data)
{
  g_settings_set_string (G_SETTINGS (data), g_object_get_data (G_OBJECT (button), "st-key"), "");
}

/* A row for SVG path @key (static): file name as subtitle (@unset when empty), Choose… and Clear buttons. */
static GtkWidget *
svg_row (GSettings  *settings,
         const char *key,
         const char *title,
         const char *unset)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  g_settings_bind_with_mapping (settings, key, row, "subtitle", G_SETTINGS_BIND_GET, path_to_subtitle, NULL,
                                (gpointer) unset, NULL);

  GtkWidget *clear = gtk_button_new_from_icon_name ("edit-clear-symbolic");
  gtk_widget_set_valign (clear, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (clear, "flat");
  g_autofree char *clear_label = g_strdup_printf ("Clear %s", title);
  gtk_widget_set_tooltip_text (clear, clear_label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (clear), GTK_ACCESSIBLE_PROPERTY_LABEL, clear_label, -1);
  g_object_set_data (G_OBJECT (clear), "st-key", (gpointer) key);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_svg_clear), settings);
  g_settings_bind_with_mapping (settings, key, clear, "sensitive", G_SETTINGS_BIND_GET, path_is_set, NULL, NULL,
                                NULL);

  GtkWidget *choose = gtk_button_new_from_icon_name ("document-open-symbolic");
  gtk_widget_set_valign (choose, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (choose, "flat");
  g_autofree char *choose_label = g_strdup_printf ("Choose %s…", title);
  gtk_widget_set_tooltip_text (choose, choose_label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (choose), GTK_ACCESSIBLE_PROPERTY_LABEL, choose_label, -1);
  g_object_set_data (G_OBJECT (choose), "st-key", (gpointer) key);
  g_object_set_data (G_OBJECT (choose), "st-title", (gpointer) title);
  g_signal_connect (choose, "clicked", G_CALLBACK (on_svg_choose), settings);

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), clear);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), choose);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), choose);
  return row;
}

static GtkWidget *
group (const char *title,
       const char *description)
{
  GtkWidget *g = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g), title);
  if (description != NULL)
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (g), description);
  return g;
}

/* ---- Pages ---------------------------------------------------------------------------------------------- */

static GtkWidget *
folder_preview_page (GSettings *settings)
{
  const NuConfig *d = nu_settings_defaults ();
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *g = group (NULL, NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g),
                             switch_row (settings, "folder-preview-enabled", "Show Previews on Folder Icons",
                                         "Draw the first pictures and videos of a folder on its icon in the "
                                         "grid view"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (g));

  /* Layout (PLAN.md §11.3). */
  GtkWidget *layout_group = group ("Layout", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (layout_group),
                             enum_row (settings, "folder-preview-layout", "Layout",
                                       (const char *[]) { "Grid", "Stacked", NULL }, layout_nicks));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (layout_group),
                             switch_row (settings, "folder-preview-cycle", "Cycle on Hover",
                                         "Show the next pictures while the pointer is over a folder"));
  GtkWidget *step = enum_row (settings, "folder-preview-stack-step", "Pictures per Step",
                              (const char *[]) { "One", "Three", NULL }, stack_step_nicks);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (step), "How many new pictures each hover step brings to the stack");
  g_settings_bind_with_mapping (settings, "folder-preview-layout", step, "visible", G_SETTINGS_BIND_GET, nick_is,
                                NULL, (gpointer) "stacked", NULL);
  g_settings_bind (settings, "folder-preview-cycle", step, "sensitive", G_SETTINGS_BIND_GET);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (layout_group), step);
  GtkWidget *depth = enum_row (settings, "folder-preview-depth-effect", "Depth Effect",
                               (const char *[]) { "None", "Blur", "Transparency", "Darken", NULL }, depth_nicks);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (depth), "How the pictures further back look");
  g_settings_bind_with_mapping (settings, "folder-preview-layout", depth, "visible", G_SETTINGS_BIND_GET, nick_is,
                                NULL, (gpointer) "stacked", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (layout_group), depth);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (layout_group));

  /* Thumbnail style: both layouts (V1). */
  GtkWidget *style_group = group ("Thumbnails", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (style_group),
                             switch_row (settings, "folder-preview-border", "Border", NULL));
  GtkWidget *border_colour = colour_button_row (settings, "folder-preview-border-colour", "Border Colour",
                                                &d->preview_border_colour);
  g_settings_bind (settings, "folder-preview-border", border_colour, "sensitive", G_SETTINGS_BIND_GET);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (style_group), border_colour);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (style_group),
                             switch_row (settings, "folder-preview-shadow", "Shadow", NULL));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (style_group),
                             switch_row (settings, "folder-preview-keep-aspect", "Keep Aspect Ratio",
                                         "Show whole pictures in their own shape instead of filling the space, "
                                         "cropped"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (style_group),
                             px_row (settings, "folder-preview-corner-radius", "Corner Radius",
                                     "Pixels at the smallest icon size, scaled with the icons", 0, 16));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (style_group),
                             switch_row (settings, "folder-preview-rotation", "Random Rotation",
                                         "Tilt each picture by a small angle that stays the same"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (style_group));

  /* Stack placement and the area all pictures are clipped to: stacked layout only (PLAN.md §11.5b). */
  GtkWidget *stack_group = group ("Stack", "Sizes are in pixels at the smallest (64 × 64) icon size and "
                                           "scale with the icons.");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             enum_row (settings, "folder-preview-stack-position", "Position",
                                       (const char *[]) { "Top", "Centre", "Bottom", NULL }, position_nicks));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             slider_row (settings, "folder-preview-stack-padding", "Side Padding",
                                         "Space between the pictures and the icon’s sides", 0, 20, "px"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             slider_row (settings, "folder-preview-stack-offset", "Vertical Offset",
                                         "Fine-tune the position", -16, 16, "px"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             slider_row (settings, "folder-preview-fade", "Bottom Fade",
                                         "Fade the pictures out towards the bottom", 0, 100, "%"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             px_row (settings, "folder-preview-view-top-radius", "Top Corner Radius",
                                     "Rounds the top of the area that clips all pictures", 0, 16));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             px_row (settings, "folder-preview-view-bottom-radius", "Bottom Corner Radius",
                                     "Rounds the bottom of that area (both 0: no clipping)", 0, 16));
  g_settings_bind_with_mapping (settings, "folder-preview-layout", stack_group, "visible", G_SETTINGS_BIND_GET,
                                nick_is, NULL, (gpointer) "stacked", NULL);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (stack_group));

  /* Custom folder SVGs: stacked layout only. */
  GtkWidget *icon_group = group ("Folder Icon",
                                 "Used on folders that show previews in the stacked layout. Both SVGs are "
                                 "drawn at the size of the icon: give them the same canvas, and place the "
                                 "overlay where it belongs inside its SVG. It is drawn on top of the "
                                 "pictures.");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (icon_group),
                             svg_row (settings, "folder-preview-folder-icon", "Folder Icon", "Theme folder icon"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (icon_group),
                             svg_row (settings, "folder-preview-overlay-icon", "Front Overlay", "None"));
  g_settings_bind_with_mapping (settings, "folder-preview-layout", icon_group, "visible", G_SETTINGS_BIND_GET,
                                nick_is, NULL, (gpointer) "stacked", NULL);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (icon_group));

  /* Options of a switched-off feature can't be edited (follows the pending switch). */
  g_settings_bind (settings, "folder-preview-enabled", layout_group, "sensitive", G_SETTINGS_BIND_GET);
  g_settings_bind (settings, "folder-preview-enabled", style_group, "sensitive", G_SETTINGS_BIND_GET);
  g_settings_bind (settings, "folder-preview-enabled", icon_group, "sensitive", G_SETTINGS_BIND_GET);
  g_settings_bind (settings, "folder-preview-enabled", stack_group, "sensitive", G_SETTINGS_BIND_GET);
  return page;
}

static GtkWidget *
sidebar_drives_page (GSettings *settings)
{
  const NuConfig *d = nu_settings_defaults ();
  GtkWidget *page = adw_preferences_page_new ();

  GtkWidget *main_group = group (NULL, NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (main_group),
                             switch_row (settings, "sidebar-drives-enabled", "Enhanced Drive Rows",
                                         "Usage bars and free space on drives, the system drive named after the "
                                         "OS, and network shares below a separator"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (main_group));

  GtkWidget *bar_group = group ("Usage Bar", NULL);
  GtkWidget *mode = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (mode), "Bar Colour");
  g_autoptr (GtkStringList) modes = gtk_string_list_new ((const char *[]) { "Accent Colour", "Custom Colour",
                                                                            "Gradient", NULL });
  adw_combo_row_set_model (ADW_COMBO_ROW (mode), G_LIST_MODEL (modes));
  g_settings_bind_with_mapping (settings, "bar-colour-mode", mode, "selected", G_SETTINGS_BIND_DEFAULT,
                                mode_to_selected, selected_to_mode, NULL, NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar_group), mode);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar_group),
                             colour_row (settings, "bar-colour", "Colour", &d->bar_colour, NU_BAR_COLOUR_CUSTOM));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar_group),
                             colour_row (settings, "bar-gradient-start", "Start Colour", &d->gradient_start,
                                         NU_BAR_COLOUR_GRADIENT));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar_group),
                             colour_row (settings, "bar-gradient-end", "End Colour", &d->gradient_end,
                                         NU_BAR_COLOUR_GRADIENT));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar_group),
                             switch_row (settings, "show-usage-bars", "Show Usage Bars", NULL));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar_group),
                             switch_row (settings, "show-size-labels", "Show Size Labels",
                                         "The “free of” line below each bar"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (bar_group));

  GtkWidget *eject_group = group ("Eject Button", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (eject_group),
                             switch_row (settings, "eject-slide", "Slide In on Hover",
                                         "Hidden until the pointer is over the drive or it has keyboard focus; "
                                         "the usage bar then uses the full width"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (eject_group));

  /* Options of a switched-off feature can't be edited (reads the pending value, so it follows the switch). */
  g_settings_bind (settings, "sidebar-drives-enabled", bar_group, "sensitive", G_SETTINGS_BIND_GET);
  g_settings_bind (settings, "sidebar-drives-enabled", eject_group, "sensitive", G_SETTINGS_BIND_GET);
  return page;
}

/* Confirmation answered (PLAN.md §10.1.3): only "reset" acts, and right away (not a pending setting). */
static void
on_reset_response (AdwAlertDialog *alert,
                   const char     *response,
                   gpointer        data)
{
  (void) alert;
  (void) data;
  if (g_strcmp0 (response, "reset") == 0 && nu_settings_reset_folder_views ())
    g_debug ("all folder views reset");
}

static void
on_reset_activated (AdwButtonRow *row,
                    gpointer      data)
{
  (void) data;
  AdwDialog *alert = adw_alert_dialog_new ("Reset All Folder Views?",
                                           "Every folder will show the default view and icon size again. "
                                           "This can’t be undone.");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert), "cancel", "_Cancel", "reset", "_Reset", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "reset", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  g_signal_connect (alert, "response", G_CALLBACK (on_reset_response), NULL);
  adw_dialog_present (alert, GTK_WIDGET (row));
}

static GtkWidget *
folder_views_page (GSettings *settings)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *main_group = group (NULL, NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (main_group),
                             switch_row (settings, "folder-views-enabled", "Remember View per Folder",
                                         "Each folder keeps its own view type and icon size; folders you "
                                         "haven’t changed use the default"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (main_group));

  GtkWidget *saved_group = group ("Saved Views", NULL);
  GtkWidget *reset = adw_button_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (reset), "Reset All Folder Views…");
  adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (reset), FALSE);
  adw_button_row_set_start_icon_name (ADW_BUTTON_ROW (reset), "edit-clear-all-symbolic");
  gtk_widget_add_css_class (reset, "destructive-action");
  g_signal_connect (reset, "activated", G_CALLBACK (on_reset_activated), NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (saved_group), reset);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (saved_group));

  /* Like the other tabs: options of a switched-off feature can't be used (follows the pending switch). */
  g_settings_bind (settings, "folder-views-enabled", saved_group, "sensitive", G_SETTINGS_BIND_GET);
  return page;
}

/* Shown instead of the tabs when the schema isn't installed. */
static GtkWidget *
unavailable_page (void)
{
  GtkWidget *status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status), "dialog-warning-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "Settings Unavailable");
  adw_status_page_set_description (ADW_STATUS_PAGE (status),
                                   "The settings schema io.github.zyrotec.NautilusUtils is not installed. "
                                   "All features use their default settings.");
  return status;
}

/* ---- Dialog --------------------------------------------------------------------------------------------- */

static void
on_apply (GtkButton *button, gpointer data)
{
  (void) button;
  g_settings_apply (G_SETTINGS (data));
}

/* Unapplied changes are discarded when the dialog closes (N5). */
static void
on_closed (AdwDialog *dialog, gpointer data)
{
  (void) dialog;
  GSettings *settings = data;
  if (g_settings_get_has_unapplied (settings))
    g_settings_revert (settings);
}

static void
weak_ref_free (gpointer data)
{
  g_weak_ref_clear (data);
  g_free (data);
}

GtkWidget *
st_settings_dialog_get (GtkWidget *parent)
{
  GtkRoot *root = gtk_widget_get_root (parent);
  GWeakRef *ref = root != NULL ? g_object_get_data (G_OBJECT (root), DIALOG_KEY) : NULL;
  if (ref == NULL)
    return NULL;
  g_autoptr (GObject) dialog = g_weak_ref_get (ref);
  /* Borrowed: GTK keeps the presented dialog alive; our temporary ref is dropped here. */
  return dialog != NULL ? GTK_WIDGET (dialog) : NULL;
}

void
st_settings_dialog_present (GtkWidget *parent)
{
  GtkRoot *root = gtk_widget_get_root (parent);
  g_return_if_fail (root != NULL);
  if (st_settings_dialog_get (parent) != NULL)
    return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, "Nautilus Utils Settings");
  adw_dialog_set_content_width (dialog, 600);
  adw_dialog_set_content_height (dialog, 640);
  /* A dialog with breakpoints needs a minimum size (libadwaita warns otherwise); 360 × 294 is the HIG
   * smallest supported size. */
  gtk_widget_set_size_request (GTK_WIDGET (dialog), 360, 294);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  g_autoptr (GSettings) settings = nu_settings_new_delayed ();
  if (settings == NULL)
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), unavailable_page ());
  else
    {
      GtkWidget *stack = adw_view_stack_new ();
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), folder_preview_page (settings),
                                           "folder-preview", "Folder Preview", "folder-pictures-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), sidebar_drives_page (settings),
                                           "sidebar-drives", "Sidebar Drives", "drive-harddisk-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), folder_views_page (settings),
                                           "folder-views", "Folder Views", "view-grid-symbolic");
      adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), stack);

      GtkWidget *switcher = adw_view_switcher_new ();
      adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
      adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_STACK (stack));
      adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);

      /* Narrow (e.g. a bottom sheet on a small screen): tabs move to a bar at the bottom, title shown. */
      GtkWidget *switcher_bar = adw_view_switcher_bar_new ();
      adw_view_switcher_bar_set_stack (ADW_VIEW_SWITCHER_BAR (switcher_bar), ADW_VIEW_STACK (stack));
      adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), switcher_bar);
      AdwBreakpoint *narrow = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 500sp"));
      g_auto (GValue) reveal = G_VALUE_INIT;
      g_value_init (&reveal, G_TYPE_BOOLEAN);
      g_value_set_boolean (&reveal, TRUE);
      adw_breakpoint_add_setter (narrow, G_OBJECT (switcher_bar), "reveal", &reveal);
      g_auto (GValue) no_title = G_VALUE_INIT;
      g_value_init (&no_title, GTK_TYPE_WIDGET);
      adw_breakpoint_add_setter (narrow, G_OBJECT (header), "title-widget", &no_title);
      adw_dialog_add_breakpoint (dialog, narrow); /* takes ownership */

      GtkWidget *apply = gtk_button_new_with_mnemonic ("_Apply");
      gtk_widget_add_css_class (apply, "suggested-action");
      gtk_widget_set_tooltip_text (apply, "Apply the changes");
      g_object_bind_property (settings, "has-unapplied", apply, "sensitive", G_BINDING_SYNC_CREATE);
      /* The dialog keeps @settings alive (qdata below), so plain handlers with it as data are safe. */
      g_signal_connect (apply, "clicked", G_CALLBACK (on_apply), settings);
      adw_header_bar_pack_end (ADW_HEADER_BAR (header), apply);
      adw_dialog_set_default_widget (dialog, apply);

      g_signal_connect (dialog, "closed", G_CALLBACK (on_closed), settings);
      /* The dialog owns one ref on the delayed settings, released when it is finalized. */
      g_object_set_data_full (G_OBJECT (dialog), "st-settings", g_object_ref (settings), g_object_unref);
    }

  adw_dialog_set_child (dialog, toolbar);

  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, dialog);
  g_object_set_data_full (G_OBJECT (root), DIALOG_KEY, ref, weak_ref_free); /* replaces a stale ref */
  adw_dialog_present (dialog, GTK_WIDGET (root));
}
