/* Folder-icons feature: set-up, tear-down and settings (PLAN.md §12). Called from src/extension.c.
 *
 * Files lists a folder → fi_provider → on_folder(): the applier checks it if its name is a candidate.
 * Rules change → a new FiRules snapshot (with removed rule names added to the history, so their folders are
 * still restored) → every recently seen folder whose name is a candidate is handed back to Files with
 * nautilus_file_info_invalidate_extension_info(), so Files calls the provider for it again. Only public
 * Nautilus API is used.
 */
#include "folder_icons.h"

#include <nautilus-extension.h>

#include "fi_apply.h"
#include "fi_provider.h"
#include "fi_rules.h"
#include "nu_settings.h"

#ifdef FI_DEBUG_DRIVER
#include <glib-unix.h>
#include <signal.h>
#endif

#define FI_MAX_INFLIGHT 4    /* folders processed at once (PLAN.md §12.3 rule 3) */
#define SEEN_MAX        4096 /* recently listed folders remembered for rule changes */

static GType       fi_types[1];
static FiApplier  *applier;           /* Owned. */
static GHashTable *seen;              /* Owned: uri (owned) → GWeakRef* (owned) to its NautilusFileInfo. */
static char       *signature;         /* Owned: the settings the current rules were built from. */
static GStrv       rule_names;        /* Owned: names of the current rules (to find removed ones). */
static guint       settings_listener; /* nu_settings listener id, 0 when not registered. */
#ifdef FI_DEBUG_DRIVER
static guint debug_signal_id;
#endif

static void
weak_ref_free (gpointer data)
{
  g_weak_ref_clear (data);
  g_free (data);
}

/* Drops entries whose file is gone; if still full, forgets everything (only costs live refreshes). */
static void
prune_seen (void)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init (&iter, seen);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      g_autoptr (GObject) obj = g_weak_ref_get (value);
      if (obj == NULL)
        g_hash_table_iter_remove (&iter);
    }
  if (g_hash_table_size (seen) >= SEEN_MAX)
    g_hash_table_remove_all (seen);
}

/* Remembers @file (weakly) under @uri for refreshes after a rule change. */
static void
remember (NautilusFileInfo *file, const char *uri)
{
  if (g_hash_table_contains (seen, uri))
    return;
  if (g_hash_table_size (seen) >= SEEN_MAX)
    prune_seen ();
  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, file);
  g_hash_table_insert (seen, g_strdup (uri), ref);
}

/* fi_provider callback: Files lists the directory @file (borrowed). */
static void
on_folder (NautilusFileInfo *file)
{
  if (applier == NULL)
    return;
  g_autoptr (GFile) location = nautilus_file_info_get_location (file);
  if (location == NULL)
    return;
  g_autofree char *uri = g_file_get_uri (location);
  remember (file, uri);
  g_autofree char *name = nautilus_file_info_get_name (file);
  fi_applier_check (applier, location, name);
}

/* Asks Files to call the provider again for every remembered folder that the current rules care about. */
static void
refresh_seen (void)
{
  FiRules *rules = fi_applier_get_rules (applier); /* borrowed */
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init (&iter, seen);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GObject *obj = g_weak_ref_get (value); /* new ref or NULL */
      if (obj == NULL)
        {
          g_hash_table_iter_remove (&iter);
          continue;
        }
      g_autofree char *name = nautilus_file_info_get_name (NAUTILUS_FILE_INFO (obj));
      if (fi_rules_is_candidate (rules, name))
        g_ptr_array_add (files, obj); /* the array owns the ref */
      else
        g_object_unref (obj);
    }
  /* Outside the iteration: invalidating may make Files call us back synchronously. */
  for (guint i = 0; i < files->len; i++)
    nautilus_file_info_invalidate_extension_info (g_ptr_array_index (files, i));
  if (files->len > 0)
    g_debug ("re-checking %u listed folders", files->len);
}

/* New string describing everything the rules depend on (to skip no-op notifications). */
static char *
make_signature (const NuConfig *c, const char *const *history)
{
  GString *s = g_string_new (c->folder_icons_enabled ? "on" : "off");
  for (guint i = 0; i < c->n_icon_rules; i++)
    g_string_append_printf (s, "\n%s\t%s", c->icon_rules[i].name, c->icon_rules[i].icon);
  g_string_append (s, "\n--");
  for (gsize i = 0; history != NULL && history[i] != NULL; i++)
    g_string_append_printf (s, "\n%s", history[i]);
  return g_string_free (s, FALSE);
}

/* Builds the rules from the settings. Names of rules that were removed since last time are added to the
 * history (in the snapshot right away, and to the setting). */
static void
load_rules (void)
{
  const NuConfig *c = nu_settings_get ();
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < c->n_icon_rules; i++)
    g_ptr_array_add (names, g_strdup (c->icon_rules[i].name));
  g_ptr_array_add (names, NULL);

  g_autoptr (GStrvBuilder) history = g_strv_builder_new ();
  g_strv_builder_addv (history, (const char **) c->icon_rule_history);
  g_autoptr (GStrvBuilder) removed = g_strv_builder_new ();
  for (gsize i = 0; rule_names != NULL && rule_names[i] != NULL; i++)
    if (!g_strv_contains ((const char *const *) names->pdata, rule_names[i]) &&
        !g_strv_contains (c->icon_rule_history, rule_names[i]))
      {
        g_strv_builder_add (history, rule_names[i]);
        g_strv_builder_add (removed, rule_names[i]);
      }
  g_auto (GStrv) all_history = g_strv_builder_end (history);
  g_auto (GStrv) removed_names = g_strv_builder_end (removed);
  g_strfreev (rule_names);
  rule_names = (GStrv) g_ptr_array_free (g_steal_pointer (&names), FALSE);

  g_autofree char *sig = make_signature (c, (const char *const *) all_history);
  if (signature != NULL && g_str_equal (sig, signature))
    return; /* e.g. only our own history write came back */
  g_free (signature);
  signature = g_steal_pointer (&sig);

  FiRules *rules = fi_rules_new (c->icon_rules, c->n_icon_rules, (const char *const *) all_history,
                                 c->folder_icons_enabled);
  g_debug ("rules: %u, %s", fi_rules_get_n (rules), c->folder_icons_enabled ? "on" : "off");
  fi_applier_set_rules (applier, rules); /* takes it */
  if (removed_names[0] != NULL)
    nu_settings_remember_icon_rule_names ((const char *const *) removed_names);
  refresh_seen ();
}

static void
on_settings_changed (NuChanged changed, gpointer data)
{
  (void) data;
  if (changed & NU_CHANGED_FOLDER_ICONS)
    load_rules ();
}

#ifdef FI_DEBUG_DRIVER
/* Debug driver (only in -Ddebug_driver=true builds, tools/smoke-nautilus.sh): "fi-dump" logs the counters. */
static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (applier == NULL || path == NULL || !g_file_get_contents (path, &cmd, NULL, NULL))
    return G_SOURCE_CONTINUE;
  g_strstrip (cmd);
  if (g_str_equal (cmd, "fi-dump"))
    g_message ("fi dump: rules=%u set=%u cleared=%u errors=%u pending=%u inflight=%u seen=%u",
               fi_rules_get_n (fi_applier_get_rules (applier)), fi_applier_get_n_set (applier),
               fi_applier_get_n_cleared (applier), fi_applier_get_n_errors (applier),
               fi_applier_get_n_pending (applier), fi_applier_get_n_inflight (), g_hash_table_size (seen));
  return G_SOURCE_CONTINUE;
}
#endif

void
fi_feature_initialize (GTypeModule *module)
{
  fi_info_provider_register (module);
  fi_types[0] = FI_TYPE_INFO_PROVIDER;
  applier = fi_applier_new (fi_applier_gio_backend (), NULL, FI_MAX_INFLIGHT);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, weak_ref_free);
  load_rules ();
  if (nu_settings_available ())
    settings_listener = nu_settings_add_listener (on_settings_changed, NULL);
  fi_info_provider_set_folder_func (on_folder);
#ifdef FI_DEBUG_DRIVER
  debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
  g_debug ("folder-icons loaded");
}

void
fi_feature_shutdown (void)
{
#ifdef FI_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
#endif
  fi_info_provider_set_folder_func (NULL);
  nu_settings_remove_listener (settings_listener);
  settings_listener = 0;
  g_clear_pointer (&applier, fi_applier_free); /* running jobs finish on their own */
  g_clear_pointer (&seen, g_hash_table_unref);
  g_clear_pointer (&signature, g_free);
  g_clear_pointer (&rule_names, g_strfreev);
  g_debug ("folder-icons shut down");
}

const GType *
fi_feature_list_types (int *num_types)
{
  *num_types = G_N_ELEMENTS (fi_types);
  return fi_types;
}
