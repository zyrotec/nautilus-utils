/* "Folder Icons" tab (PLAN.md §12.2): an on/off switch and an editable list of rules — folder name, icon,
 * remove — with an "Add Rule" button in the group header. The rows edit an in-memory list that is written
 * to the dialog's delayed GSettings after every change (pending until Apply). Choosing an icon copies the
 * file into the icon store (nu_icon_store, in a worker thread) and the rule keeps the copy's path. */
#include "folder_icons_page.h"

#include <adwaita.h>
#include <string.h>

#include "nu_icon_store.h"

#define ICON_PX  32
#define RULE_KEY "st-rule" /* row qdata: its Rule* (borrowed from Page.rules; cleared before the rule is freed) */

typedef struct
{
  char *name; /* Owned. */
  char *icon; /* Owned; "" = none chosen yet. */
} Rule;

/* Shared by the tab's widgets and pending async operations (atomic rc box; the tab holds one ref). */
typedef struct
{
  GSettings *settings; /* Owned ref: the dialog's delayed settings. */
  GPtrArray *rules;    /* Owned: Rule* (owned), in display order. */
  GtkWidget *group;    /* Weak pointer: the "Rules" group; NULL once destroyed. */
} Page;

static void
rule_free (gpointer data)
{
  Rule *r = data;
  g_free (r->name);
  g_free (r->icon);
  g_free (r);
}

static void
page_clear (gpointer data)
{
  Page *p = data;
  if (p->group != NULL)
    g_object_remove_weak_pointer (G_OBJECT (p->group), (gpointer *) &p->group);
  g_clear_object (&p->settings);
  g_clear_pointer (&p->rules, g_ptr_array_unref);
}

static void
page_unref (gpointer data)
{
  g_atomic_rc_box_release_full (data, page_clear);
}

/* Writes the list to the (delayed) settings. */
static void
save (Page *p)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < p->rules->len; i++)
    {
      Rule *r = g_ptr_array_index (p->rules, i);
      g_variant_builder_add (&b, "(ss)", r->name, r->icon);
    }
  g_settings_set_value (p->settings, "folder-icon-rules", g_variant_builder_end (&b)); /* consumes it */
}

/* Rows of the group that hold a rule, in order. Caller frees the array (rows borrowed). */
static GPtrArray *
rule_rows (Page *p)
{
  GPtrArray *rows = g_ptr_array_new ();
  if (p->group == NULL)
    return rows;
  /* AdwPreferencesGroup keeps its rows in a GtkListBox inside; walk its widget tree for our rows. */
  g_autoptr (GPtrArray) stack = g_ptr_array_new ();
  g_ptr_array_add (stack, p->group);
  while (stack->len > 0)
    {
      GtkWidget *w = g_ptr_array_steal_index (stack, 0);
      if (ADW_IS_ENTRY_ROW (w) && g_object_get_data (G_OBJECT (w), RULE_KEY) != NULL)
        {
          g_ptr_array_add (rows, w);
          continue;
        }
      for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
        g_ptr_array_add (stack, c);
    }
  return rows;
}

/* Marks rows whose name can't work: a '/' in it, or the same name as a rule above it. */
static void
validate (Page *p)
{
  g_autoptr (GPtrArray) rows = rule_rows (p);
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal); /* borrowed names */
  for (guint i = 0; i < p->rules->len; i++)
    {
      Rule *r = g_ptr_array_index (p->rules, i);
      GtkWidget *row = NULL;
      for (guint k = 0; k < rows->len && row == NULL; k++)
        if (g_object_get_data (G_OBJECT (g_ptr_array_index (rows, k)), RULE_KEY) == r)
          row = g_ptr_array_index (rows, k);
      const char *problem = NULL;
      if (strchr (r->name, '/') != NULL)
        problem = "A folder name can’t contain “/”";
      else if (*r->name != '\0' && g_hash_table_contains (seen, r->name))
        problem = "A rule above already uses this name; this one is ignored";
      else if (*r->name != '\0')
        g_hash_table_add (seen, r->name);
      if (row == NULL)
        continue;
      if (problem != NULL)
        gtk_widget_add_css_class (row, "error");
      else
        gtk_widget_remove_css_class (row, "error");
      gtk_widget_set_tooltip_text (row, problem);
    }
}

static void
set_image (GtkWidget *image, const char *icon)
{
  if (icon != NULL && *icon != '\0')
    gtk_image_set_from_file (GTK_IMAGE (image), icon);
  else
    gtk_image_set_from_icon_name (GTK_IMAGE (image), "folder-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (image), ICON_PX);
}

static void
on_name_changed (GtkEditable *editable, gpointer data)
{
  Page *p = data;
  Rule *r = g_object_get_data (G_OBJECT (editable), RULE_KEY);
  if (r == NULL)
    return;
  g_free (r->name);
  r->name = g_strstrip (g_strdup (gtk_editable_get_text (editable)));
  validate (p);
  save (p);
}

static void
on_remove (GtkButton *button, gpointer data)
{
  Page *p = data;
  GtkWidget *row = gtk_widget_get_ancestor (GTK_WIDGET (button), ADW_TYPE_ENTRY_ROW);
  Rule *r = row != NULL ? g_object_get_data (G_OBJECT (row), RULE_KEY) : NULL;
  if (r == NULL || p->group == NULL)
    return;
  g_object_set_data (G_OBJECT (row), RULE_KEY, NULL); /* before the rule is freed */
  g_ptr_array_remove (p->rules, r);                   /* frees it */
  adw_preferences_group_remove (ADW_PREFERENCES_GROUP (p->group), row);
  validate (p);
  save (p);
}

/* ---- Choosing an icon: file chooser, then copy into the store ---- */

typedef struct
{
  Page    *page; /* Owned ref. */
  GWeakRef row;  /* The rule's row; the rule is found through it (gone with it). */
} Pick;

static void
pick_free (Pick *pick)
{
  page_unref (pick->page);
  g_weak_ref_clear (&pick->row);
  g_free (pick);
}

/* The icon was copied into the store. Consumes @data. */
static void
on_imported (GObject *source, GAsyncResult *res, gpointer data)
{
  (void) source;
  Pick *pick = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = nu_icon_store_import_finish (res, &error);
  g_autoptr (GtkWidget) row = g_weak_ref_get (&pick->row);
  Rule *r = row != NULL ? g_object_get_data (G_OBJECT (row), RULE_KEY) : NULL;
  if (path == NULL)
    g_debug ("folder icon not stored: %s", error->message);
  else if (r != NULL)
    {
      g_free (r->icon);
      r->icon = g_steal_pointer (&path);
      set_image (g_object_get_data (G_OBJECT (row), "st-image"), r->icon);
      save (pick->page);
    }
  pick_free (pick);
}

/* The file chooser closed. Consumes @data. */
static void
on_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
  Pick *pick = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, &error);
  if (file == NULL)
    {
      if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        g_debug ("icon chooser: %s", error->message);
      pick_free (pick);
      return;
    }
  nu_icon_store_import_async (file, NULL, NULL, on_imported, pick); /* pick handed over */
}

static void
on_choose (GtkButton *button, gpointer data)
{
  Page *p = data;
  GtkWidget *row = gtk_widget_get_ancestor (GTK_WIDGET (button), ADW_TYPE_ENTRY_ROW);
  if (row == NULL)
    return;
  g_autoptr (GtkFileDialog) chooser = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (chooser, "Choose Folder Icon");
  gtk_file_dialog_set_modal (chooser, TRUE);
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, "SVG and PNG Images");
  gtk_file_filter_add_mime_type (filter, "image/svg+xml");
  gtk_file_filter_add_mime_type (filter, "image/png");
  gtk_file_filter_add_suffix (filter, "svg");
  gtk_file_filter_add_suffix (filter, "png");
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (chooser, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (chooser, filter);

  Pick *pick = g_new0 (Pick, 1); /* freed by on_chosen / on_imported */
  pick->page = g_atomic_rc_box_acquire (p);
  g_weak_ref_init (&pick->row, row);
  GtkRoot *root = gtk_widget_get_root (row);
  gtk_file_dialog_open (chooser, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, on_chosen, pick);
}

/* ---- Rows ---- */

static GtkWidget *
icon_button (const char *icon_name, const char *label, GCallback callback, Page *p)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon_name);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_tooltip_text (button, label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect (button, "clicked", callback, p);
  return button;
}

/* A row for @r (borrowed from p->rules). */
static GtkWidget *
make_row (Page *p, Rule *r)
{
  GtkWidget *row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Folder Name");
  gtk_editable_set_text (GTK_EDITABLE (row), r->name);
  g_object_set_data (G_OBJECT (row), RULE_KEY, r);

  GtkWidget *image = gtk_image_new ();
  set_image (image, r->icon);
  gtk_accessible_update_property (GTK_ACCESSIBLE (image), GTK_ACCESSIBLE_PROPERTY_LABEL, "Icon", -1);
  g_object_set_data (G_OBJECT (row), "st-image", image); /* borrowed: the row owns it */
  adw_entry_row_add_prefix (ADW_ENTRY_ROW (row), image);
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (row),
                            icon_button ("document-open-symbolic", "Choose Icon…", G_CALLBACK (on_choose), p));
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (row),
                            icon_button ("user-trash-symbolic", "Remove Rule", G_CALLBACK (on_remove), p));
  g_signal_connect (row, "changed", G_CALLBACK (on_name_changed), p);
  return row;
}

static void
on_add (GtkButton *button, gpointer data)
{
  (void) button;
  Page *p = data;
  if (p->group == NULL)
    return;
  Rule *r = g_new0 (Rule, 1); /* owned by p->rules */
  r->name = g_strdup ("");
  r->icon = g_strdup ("");
  g_ptr_array_add (p->rules, r);
  GtkWidget *row = make_row (p, r);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (p->group), row);
  gtk_widget_grab_focus (row);
  save (p);
}

GtkWidget *
st_folder_icons_page (GSettings *settings)
{
  g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);
  GtkWidget *page = adw_preferences_page_new ();

  GtkWidget *main_group = adw_preferences_group_new ();
  GtkWidget *on = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (on), "Icons by Folder Name");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (on),
                               "Every folder with a rule’s name gets the rule’s icon, everywhere, as its own "
                               "custom icon. Custom icons you set yourself are never changed.");
  g_settings_bind (settings, "folder-icons-enabled", on, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (main_group), on);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (main_group));

  Page *p = g_atomic_rc_box_new0 (Page);
  p->settings = g_object_ref (settings);
  p->rules = g_ptr_array_new_with_free_func (rule_free);
  g_object_set_data_full (G_OBJECT (page), "st-folder-icons", p, page_unref); /* the tab owns one ref */

  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), "Rules");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                         "The name must match exactly (capitals count), e.g. node_modules. "
                                         "Icons are SVG or PNG files; a copy is kept, so the original can be "
                                         "moved. Removing a rule restores the folders as Files shows them.");
  GtkWidget *add = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (add, "flat");
  gtk_widget_set_valign (add, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (add, "Add Rule");
  gtk_accessible_update_property (GTK_ACCESSIBLE (add), GTK_ACCESSIBLE_PROPERTY_LABEL, "Add Rule", -1);
  g_signal_connect (add, "clicked", G_CALLBACK (on_add), p);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (group), add);
  p->group = group;
  g_object_add_weak_pointer (G_OBJECT (group), (gpointer *) &p->group);

  g_autoptr (GVariant) value = g_settings_get_value (settings, "folder-icon-rules");
  for (gsize i = 0; i < g_variant_n_children (value); i++)
    {
      Rule *r = g_new0 (Rule, 1); /* owned by p->rules */
      g_variant_get_child (value, i, "(ss)", &r->name, &r->icon);
      g_ptr_array_add (p->rules, r);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), make_row (p, r));
    }
  validate (p);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));
  g_settings_bind (settings, "folder-icons-enabled", group, "sensitive", G_SETTINGS_BIND_GET);
  return page;
}
