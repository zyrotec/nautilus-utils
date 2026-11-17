/* "Nautilus Utils Settings" dialog (PLAN.md §9.1.2, decision N5).
 *
 * AdwDialog > AdwNavigationSplitView: a sidebar (AdwViewSwitcherSidebar, one row per feature) beside the
 * selected feature's page, with an Apply button in its header bar. The controls are bound to a *delayed*
 * GSettings (nu_settings_new_delayed), so nothing changes until Apply (g_settings_apply). Closing with
 * unapplied changes reverts them. Narrow dialogs collapse the split view (AdwBreakpoint): the sidebar becomes
 * a list page that opens each feature. Every control has an accessible label; colours come from the theme. */
#include "settings_dialog.h"

#include <adwaita.h>
#include <math.h>

#include "folder_icons_page.h"
#include "gt_ide.h"
#include "nu_settings.h"

#define DIALOG_KEY "st-settings-dialog" /* window qdata: GWeakRef to the open dialog */

static const char *const colour_mode_nicks[] = { "accent", "custom", "gradient" };
/* Folder preview enums (PLAN.md §11.3), NULL-terminated, in combo row order. */
static const char *const layout_nicks[] = { "grid", "stacked", NULL };
static const char *const stack_step_nicks[] = { "one", "three", NULL };
static const char *const depth_nicks[] = { "none", "blur", "transparency", "darken", NULL };
static const char *const position_nicks[] = { "top", "centre", "bottom", NULL };
static const char *const pane_position_nicks[] = { "right", "bottom", NULL };
static const char *const media_fit_nicks[] = { "shape", "box", NULL };

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

/* GtkScale:format-value with one decimal and @data (static) as the unit. Returns a new string (GTK frees). */
static char *
format_value_1 (GtkScale *scale, double value, gpointer data)
{
  (void) scale;
  return g_strdup_printf ("%.1f %s", value, (const char *) data);
}

/* A row with a slider for double @key in [min, max] (schema range), in steps of @step. */
static GtkWidget *
slider_row_d (GSettings  *settings,
              const char *key,
              const char *title,
              const char *subtitle,
              double      min,
              double      max,
              double      step,
              const char *unit)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, min, max, step);
  gtk_scale_set_digits (GTK_SCALE (scale), 1);
  gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
  gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func (GTK_SCALE (scale), format_value_1, (gpointer) unit, NULL);
  gtk_range_set_round_digits (GTK_RANGE (scale), 1);
  if (min < 0 && max > 0)
    gtk_scale_add_mark (GTK_SCALE (scale), 0, GTK_POS_BOTTOM, NULL);
  gtk_widget_set_size_request (scale, 180, -1);
  gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (scale), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_settings_bind (settings, key, gtk_range_get_adjustment (GTK_RANGE (scale)), "value", G_SETTINGS_BIND_DEFAULT);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), scale);
  return row;
}

/* A spin row for integer @key in [min, max] pixels (at the 64 × 64 icon size). */
/* Snaps the slider to multiples of 16 while it moves (the icon size, PLAN.md §13.12 P16). */
static void
snap_16 (GtkRange *range, gpointer data)
{
  (void) data;
  double v = gtk_range_get_value (range);
  double snapped = CLAMP (round (v / 16.0) * 16.0, 16.0, 256.0);
  if (fabs (v - snapped) > 0.01)
    gtk_range_set_value (range, snapped);
}

/* A 16–256 px slider in steps of 16 (snapped while it moves), with a mark at every 64 px. */
static GtkWidget *
icon_size_row (GSettings  *settings,
               const char *key,
               const char *title,
               const char *subtitle)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 16, 256, 16);
  gtk_scale_set_digits (GTK_SCALE (scale), 0);
  gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
  gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func (GTK_SCALE (scale), format_value, (gpointer) "px", NULL);
  for (int v = 64; v <= 256; v += 64)
    gtk_scale_add_mark (GTK_SCALE (scale), v, GTK_POS_BOTTOM, NULL);
  gtk_range_set_increments (GTK_RANGE (scale), 16, 64);
  gtk_widget_set_size_request (scale, 200, -1);
  gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (scale), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_signal_connect (scale, "value-changed", G_CALLBACK (snap_16), NULL);
  g_settings_bind_with_mapping (settings, key, gtk_range_get_adjustment (GTK_RANGE (scale)), "value",
                                G_SETTINGS_BIND_DEFAULT, int_to_double, double_to_int, NULL, NULL);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), scale);
  return row;
}

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
  GtkWidget *border_width = slider_row_d (settings, "folder-preview-border-width", "Border Width",
                                          "Pixels at the smallest icon size, scaled with the icons", 0.5, 6.0,
                                          0.5, "px");
  g_settings_bind (settings, "folder-preview-border", border_width, "sensitive", G_SETTINGS_BIND_GET);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (style_group), border_width);
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
                             slider_row_d (settings, "folder-preview-stack-spacing", "Spacing",
                                           "How far each picture shows above the one in front", 0.0, 8.0, 0.5, "px"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             slider_row_d (settings, "folder-preview-stack-offset", "Vertical Offset",
                                           "Fine-tune the position", -16.0, 16.0, 0.5, "px"));
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

/* P25: the folder previews' look into the pane's own stack options (pending until Apply, like every
 * setting). Sizes given at the 64 × 64 icon size are scaled to the pane: its stack is about FP_TO_PANE times
 * as wide as a 64 px folder's. */
#define FP_TO_PANE 4.0
static void
on_copy_fp_look (AdwButtonRow *row, gpointer data)
{
  (void) row;
  GSettings *s = data;
  g_settings_set_boolean (s, "details-pane-stack-border", g_settings_get_boolean (s, "folder-preview-border"));
  g_autofree char *colour = g_settings_get_string (s, "folder-preview-border-colour");
  g_settings_set_string (s, "details-pane-stack-border-colour", colour);
  double w = round (g_settings_get_double (s, "folder-preview-border-width") * FP_TO_PANE * 2) / 2;
  g_settings_set_double (s, "details-pane-stack-border-width", CLAMP (w, 0.5, 8.0));
  int r = (int) lround (g_settings_get_int (s, "folder-preview-corner-radius") * FP_TO_PANE);
  g_settings_set_int (s, "details-pane-stack-corner-radius", CLAMP (r, 0, 32));
  g_settings_set_boolean (s, "details-pane-stack-shadow", g_settings_get_boolean (s, "folder-preview-shadow"));
  g_settings_set_boolean (s, "details-pane-stack-rotation", g_settings_get_boolean (s, "folder-preview-rotation"));
  g_settings_set_boolean (s, "details-pane-stack-keep-aspect", g_settings_get_boolean (s, "folder-preview-keep-aspect"));
  g_settings_set_enum (s, "details-pane-stack-depth-effect", g_settings_get_enum (s, "folder-preview-depth-effect"));
  g_settings_set_int (s, "details-pane-stack-fade", g_settings_get_int (s, "folder-preview-fade"));
  g_settings_set_boolean (s, "details-pane-stack-media-square", FALSE);
}

/* Details pane (PLAN.md §13.4, §13.10). Shown/hidden and width/height are UI state, kept as the user leaves
 * them. */
static GtkWidget *
details_pane_page (GSettings *settings)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *main_group = group (NULL, NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (main_group),
                             switch_row (settings, "details-pane-enabled", "Details Pane",
                                         "A button next to View Options (or Alt+Shift+P) shows the selected "
                                         "item’s preview and properties next to the folder view"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (main_group));

  GtkWidget *pane_group = group ("Pane", NULL);
  GtkWidget *position = enum_row (settings, "details-pane-position", "Position",
                                  (const char *[]) { "Right", "Bottom", NULL }, pane_position_nicks);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (position), "Bottom shows a wide strip, like Windows 7");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group), position);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group),
                             switch_row (settings, "details-pane-border", "Border",
                                         "A line between the folder view and the pane"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group),
                             px_row (settings, "details-pane-margin", "Margin",
                                     "Space around the pane; above 0 it floats as a card", 0, 24));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group),
                             px_row (settings, "details-pane-padding", "Padding", "Space inside the pane", 6, 36));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group),
                             px_row (settings, "details-pane-radius", "Corner Radius", NULL, 0, 24));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group),
                             switch_row (settings, "details-pane-header-border", "Line Under the Header Bar",
                                         "While the pane is shown; not under the sidebar’s header"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group),
                             switch_row (settings, "details-pane-custom-colour", "Custom Background",
                                         "Give the pane its own colour instead of the view’s"));
  GtkWidget *pane_colour = colour_button_row (settings, "details-pane-colour", "Background Colour",
                                              &nu_settings_defaults ()->details_pane_colour);
  g_settings_bind (settings, "details-pane-custom-colour", pane_colour, "sensitive", G_SETTINGS_BIND_GET);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pane_group), pane_colour);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (pane_group));

  GtkWidget *preview_group = group ("Preview", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (preview_group),
                             icon_size_row (settings, "details-pane-icon-size", "Icon Size",
                                            "Files, folders and the front icon of a stack; pictures, videos and "
                                            "documents fill the box"));
  GtkWidget *fit = enum_row (settings, "details-pane-media-fit", "Pictures and Videos",
                             (const char *[]) { "Box Takes Their Shape", "Inside a Fixed Box", NULL }, media_fit_nicks);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (fit), "Either way they touch the box’s edges, without padding");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (preview_group), fit);
  GtkWidget *backdrop = switch_row (settings, "details-pane-preview-backdrop", "Blurred Background",
                                    "Fill the box around a picture or video with a blurred, darker copy of it");
  g_settings_bind_with_mapping (settings, "details-pane-media-fit", backdrop, "sensitive", G_SETTINGS_BIND_GET,
                                nick_is, NULL, (gpointer) "box", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (preview_group), backdrop);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (preview_group),
                             switch_row (settings, "details-pane-show-icon", "Icon Next to the Name", NULL));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (preview_group),
                             switch_row (settings, "details-pane-title-centered", "Centre the Name", NULL));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (preview_group),
                             px_row (settings, "details-pane-preview-radius", "Corner Radius",
                                     "Of the box around the preview", 0, 32));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (preview_group),
                             px_row (settings, "details-pane-preview-margin", "Margin",
                                     "Space around the box", 0, 32));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (preview_group));

  /* Several selected (P24–P27): the stacked folder previews' options, the pane's own copies. Border, corners
   * and box shadow only ever apply to pictures, videos and PDFs; icons and folders stay plain. */
  GtkWidget *stack_group = group ("Several Selected", "Shown as a stack, like the stacked folder previews.");
  GtkWidget *copy = adw_button_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (copy), "Copy the Folder Previews’ Look");
  adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (copy), FALSE);
  adw_button_row_set_start_icon_name (ADW_BUTTON_ROW (copy), "edit-copy-symbolic");
  gtk_widget_set_tooltip_text (copy, "Border, corners, shadow, depth, rotation, aspect ratio and fade from the "
                                     "Folder Preview page, scaled to the pane; change them below");
  g_signal_connect (copy, "activated", G_CALLBACK (on_copy_fp_look), settings);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group), copy);
  GtkWidget *depth = enum_row (settings, "details-pane-stack-depth-effect", "Depth Effect",
                               (const char *[]) { "None", "Blur", "Transparency", "Darken", NULL }, depth_nicks);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (depth), "How the items further back look");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group), depth);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             slider_row (settings, "details-pane-stack-spacing", "Spacing",
                                         "How far each item behind shows above the one in front", 0, 48, "px"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             switch_row (settings, "details-pane-stack-shadow", "Shadow", NULL));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             switch_row (settings, "details-pane-stack-rotation", "Random Rotation",
                                         "Tilt the items behind the front one by a small angle"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             slider_row (settings, "details-pane-stack-fade", "Bottom Fade",
                                         "Fade the stack out towards the bottom", 0, 100, "%"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stack_group),
                             switch_row (settings, "details-pane-stack-card", "Card Behind Icons",
                                         "A translucent card behind files and folders without a thumbnail"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (stack_group));

  GtkWidget *media_group = group ("Several Selected: Pictures, Videos and PDFs",
                                  "Icons and folders are never given a border or corners.");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group),
                             switch_row (settings, "details-pane-stack-media-square", "Squares the Size of Icons",
                                         "Instead of cards across the box; the picture fills the square, cropped"));
  GtkWidget *keep = switch_row (settings, "details-pane-stack-keep-aspect", "Keep Aspect Ratio",
                                "Whole pictures in their own shape instead of filling the card, cropped");
  g_settings_bind (settings, "details-pane-stack-media-square", keep, "sensitive",
                   G_SETTINGS_BIND_GET | G_SETTINGS_BIND_INVERT_BOOLEAN);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group), keep);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group),
                             switch_row (settings, "details-pane-stack-border", "Border", NULL));
  const NuConfig *d = nu_settings_defaults ();
  GtkWidget *bcolour = colour_button_row (settings, "details-pane-stack-border-colour", "Border Colour",
                                          &d->details_pane_stack_border_colour);
  g_settings_bind (settings, "details-pane-stack-border", bcolour, "sensitive", G_SETTINGS_BIND_GET);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group), bcolour);
  GtkWidget *bwidth = slider_row_d (settings, "details-pane-stack-border-width", "Border Width", NULL, 0.5, 8.0,
                                    0.5, "px");
  g_settings_bind (settings, "details-pane-stack-border", bwidth, "sensitive", G_SETTINGS_BIND_GET);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group), bwidth);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (media_group),
                             px_row (settings, "details-pane-stack-corner-radius", "Corner Radius", NULL, 0, 32));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (media_group));

  /* Options of a switched-off feature can't be edited (follows the pending switch). */
  g_settings_bind (settings, "details-pane-enabled", pane_group, "sensitive", G_SETTINGS_BIND_GET);
  g_settings_bind (settings, "details-pane-enabled", preview_group, "sensitive", G_SETTINGS_BIND_GET);
  g_settings_bind (settings, "details-pane-enabled", stack_group, "sensitive", G_SETTINGS_BIND_GET);
  g_settings_bind (settings, "details-pane-enabled", media_group, "sensitive", G_SETTINGS_BIND_GET);
  return page;
}

/* "Open In": the first installed IDE (""), or one of the installed IDEs by desktop ID. Pending until Apply, like
 * every setting. */
static void
on_ide_selected (AdwComboRow *row, GParamSpec *pspec, gpointer data)
{
  (void) pspec;
  GSettings *settings = data;
  GPtrArray *ids = g_object_get_data (G_OBJECT (row), "gt-ide-ids");
  guint i = adw_combo_row_get_selected (row);
  if (ids != NULL && i < ids->len)
    g_settings_set_string (settings, "git-ide", g_ptr_array_index (ids, i));
}

static GtkWidget *
ide_row (GSettings *settings)
{
  GtkStringList *names = gtk_string_list_new (NULL);
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  GList *ides = gt_ide_list ();
  g_autofree char *first = g_strdup_printf ("First Installed%s%s%s", ides != NULL ? " (" : "",
                                            ides != NULL ? g_app_info_get_name (ides->data) : "", ides != NULL ? ")" : "");
  gtk_string_list_append (names, first);
  g_ptr_array_add (ids, g_strdup (""));
  for (GList *l = ides; l != NULL; l = l->next)
    {
      gtk_string_list_append (names, g_app_info_get_name (l->data));
      g_ptr_array_add (ids, g_strdup (g_app_info_get_id (l->data)));
    }
  g_list_free_full (ides, g_object_unref);
  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Open In");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), "The IDE the status bar’s Open button uses");
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (names));
  g_object_unref (names);
  g_object_set_data_full (G_OBJECT (row), "gt-ide-ids", ids, (GDestroyNotify) g_ptr_array_unref);
  g_autofree char *current = g_settings_get_string (settings, "git-ide");
  for (guint i = 0; i < ids->len; i++)
    if (g_str_equal (g_ptr_array_index (ids, i), current))
      adw_combo_row_set_selected (ADW_COMBO_ROW (row), i);
  g_signal_connect (row, "notify::selected", G_CALLBACK (on_ide_selected), settings);
  return row;
}

/* Git (PLAN.md §14). */
static GtkWidget *
git_page (GSettings *settings)
{
  /* The tab's branch icon is in the Git feature's resources (registered at load, also when Git is off). */
  static gboolean icons_added;
  if (!icons_added && gdk_display_get_default () != NULL)
    {
      icons_added = TRUE;
      gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                        "/io/github/zyrotec/NautilusUtils/git/icons");
    }
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *main_group = group (NULL, NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (main_group),
                             switch_row (settings, "git-enabled", "Git",
                                         "Inside a Git repository: a status bar, Git columns for the list view, the "
                                         "Git view in the details pane"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (main_group));

  GtkWidget *look = group ("Show", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (look),
                             switch_row (settings, "git-status-bar", "Status Bar",
                                         "Branch, commits to push and pull, operations and Open in IDE, under the "
                                         "folder view"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (look),
                             switch_row (settings, "git-history-graph", "History Graph",
                                         "The details pane’s Git view draws the branch lines next to the commits; "
                                         "off: a plain list"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (look),
                             switch_row (settings, "git-commit-tooltip", "Commit Details on Hover",
                                         "Hovering a commit in the Git view shows its whole message, hashes and "
                                         "changes in a tooltip"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (look),
                             switch_row (settings, "git-emblems", "Status Emblems",
                                         "Mark modified, added, untracked… files and folders with a small emblem, in "
                                         "the grid and the list view"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (look));
  g_settings_bind (settings, "git-enabled", look, "sensitive", G_SETTINGS_BIND_GET);

  GtkWidget *bar = group ("Status Bar", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar),
                             switch_row (settings, "git-bar-border", "Border", "A line between the folder view and the bar"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar),
                             switch_row (settings, "git-bar-custom-colour", "Custom Background",
                                         "Give the bar its own colour instead of the view’s"));
  GtkWidget *bar_colour = colour_button_row (settings, "git-bar-colour", "Background Colour",
                                             &nu_settings_defaults ()->git_bar_colour);
  g_settings_bind (settings, "git-bar-custom-colour", bar_colour, "sensitive", G_SETTINGS_BIND_GET);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bar), bar_colour);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (bar));
  g_settings_bind (settings, "git-enabled", bar, "sensitive", G_SETTINGS_BIND_GET);

  GtkWidget *ide = group ("Open in IDE", NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (ide), ide_row (settings));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (ide));
  g_settings_bind (settings, "git-enabled", ide, "sensitive", G_SETTINGS_BIND_GET);
  return page;
}

/* Navigation buttons in the header bar. */
static GtkWidget *
navigation_page (GSettings *settings)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *g = group (NULL, NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g),
                             switch_row (settings, "nav-buttons-enabled", "Up and Refresh Buttons",
                                         "Right of the Back and Forward buttons: go to the parent folder (Alt+Up) "
                                         "and reload the folder (F5)"));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (g));
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

/* The content pane is titled after the selected feature. */
static void
on_tab_changed (GObject *stack, GParamSpec *pspec, gpointer data)
{
  (void) pspec;
  GtkWidget *child = adw_view_stack_get_visible_child (ADW_VIEW_STACK (stack));
  AdwViewStackPage *page = child != NULL ? adw_view_stack_get_page (ADW_VIEW_STACK (stack), child) : NULL;
  const char *title = page != NULL ? adw_view_stack_page_get_title (page) : NULL;
  if (title != NULL)
    adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (data), title);
}

static void
on_sidebar_activated (AdwViewSwitcherSidebar *sidebar, gpointer data)
{
  (void) sidebar;
  adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (data), TRUE);
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
  adw_dialog_set_content_width (dialog, 860); /* sidebar + a full-width preferences page */
  adw_dialog_set_content_height (dialog, 640);
  /* A dialog with breakpoints needs a minimum size (libadwaita warns otherwise); 360 × 294 is the HIG
   * smallest supported size. */
  gtk_widget_set_size_request (GTK_WIDGET (dialog), 360, 294);

  g_autoptr (GSettings) settings = nu_settings_new_delayed ();
  if (settings == NULL)
    {
      GtkWidget *toolbar = adw_toolbar_view_new ();
      adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());
      adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), unavailable_page ());
      adw_dialog_set_child (dialog, toolbar);
    }
  else
    {
      GtkWidget *stack = adw_view_stack_new ();
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), folder_preview_page (settings),
                                           "folder-preview", "Folder Preview", "folder-pictures-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), sidebar_drives_page (settings),
                                           "sidebar-drives", "Sidebar Drives", "drive-harddisk-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), folder_views_page (settings),
                                           "folder-views", "Folder Views", "view-grid-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), st_folder_icons_page (settings),
                                           "folder-icons", "Folder Icons", "folder-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), details_pane_page (settings),
                                           "details-pane", "Details Pane", "sidebar-show-right-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), git_page (settings), "git", "Git",
                                           "nu-git-branch-symbolic");
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), navigation_page (settings), "navigation",
                                           "Navigation", "go-up-symbolic");

      /* Sidebar pane: one row per feature. */
      GtkWidget *sidebar = adw_view_switcher_sidebar_new ();
      adw_view_switcher_sidebar_set_stack (ADW_VIEW_SWITCHER_SIDEBAR (sidebar), ADW_VIEW_STACK (stack));
      GtkWidget *side_toolbar = adw_toolbar_view_new ();
      adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (side_toolbar), adw_header_bar_new ());
      adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (side_toolbar), sidebar);
      AdwNavigationPage *side_page = adw_navigation_page_new (side_toolbar, "Nautilus Utils");

      /* Content pane: the selected feature's page, titled after it, with Apply at the header bar's end. */
      GtkWidget *toolbar = adw_toolbar_view_new ();
      GtkWidget *header = adw_header_bar_new ();
      adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
      adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), stack);
      GtkWidget *apply = gtk_button_new_with_mnemonic ("_Apply");
      gtk_widget_add_css_class (apply, "suggested-action");
      gtk_widget_set_tooltip_text (apply, "Apply the changes");
      g_object_bind_property (settings, "has-unapplied", apply, "sensitive", G_BINDING_SYNC_CREATE);
      /* The dialog keeps @settings alive (qdata below), so plain handlers with it as data are safe. */
      g_signal_connect (apply, "clicked", G_CALLBACK (on_apply), settings);
      adw_header_bar_pack_end (ADW_HEADER_BAR (header), apply);
      adw_dialog_set_default_widget (dialog, apply);
      AdwNavigationPage *content_page = adw_navigation_page_new (toolbar, "Folder Preview");
      g_signal_connect_object (stack, "notify::visible-child", G_CALLBACK (on_tab_changed), content_page, 0);

      GtkWidget *split = adw_navigation_split_view_new ();
      adw_navigation_split_view_set_min_sidebar_width (ADW_NAVIGATION_SPLIT_VIEW (split), 200);
      adw_navigation_split_view_set_sidebar (ADW_NAVIGATION_SPLIT_VIEW (split), side_page);
      adw_navigation_split_view_set_content (ADW_NAVIGATION_SPLIT_VIEW (split), content_page);
      /* Picking a feature shows its page (only matters when collapsed). */
      g_signal_connect_object (sidebar, "activated", G_CALLBACK (on_sidebar_activated), split, 0);

      /* Narrow (e.g. a bottom sheet on a small screen): one pane at a time, the sidebar as a list page. */
      AdwBreakpoint *narrow = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 600sp"));
      g_auto (GValue) collapsed = G_VALUE_INIT;
      g_value_init (&collapsed, G_TYPE_BOOLEAN);
      g_value_set_boolean (&collapsed, TRUE);
      adw_breakpoint_add_setter (narrow, G_OBJECT (split), "collapsed", &collapsed);
      g_auto (GValue) page_mode = G_VALUE_INIT;
      g_value_init (&page_mode, ADW_TYPE_SIDEBAR_MODE);
      g_value_set_enum (&page_mode, ADW_SIDEBAR_MODE_PAGE);
      adw_breakpoint_add_setter (narrow, G_OBJECT (sidebar), "mode", &page_mode);
      adw_dialog_add_breakpoint (dialog, narrow); /* takes ownership */

      g_signal_connect (dialog, "closed", G_CALLBACK (on_closed), settings);
      /* The dialog owns one ref on the delayed settings, released when it is finalized. */
      g_object_set_data_full (G_OBJECT (dialog), "st-settings", g_object_ref (settings), g_object_unref);
      adw_dialog_set_child (dialog, split);
    }

  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, dialog);
  g_object_set_data_full (G_OBJECT (root), DIALOG_KEY, ref, weak_ref_free); /* replaces a stale ref */
  adw_dialog_present (dialog, GTK_WIDGET (root));
}
