/* See fi_rules.h. */
#include "fi_rules.h"

#include <string.h>

#define MARKER_PREFIX "1;"

struct _FiRules
{
  GHashTable *icons;      /* Owned: folder name (owned) → icon URI (owned); empty when disabled. */
  GHashTable *candidates; /* Owned: set of names (owned) to check: rule and history names. */
  guint       n_valid;
};

static gboolean
valid_name (const char *name)
{
  return name != NULL && *name != '\0' && strchr (name, '/') == NULL;
}

/* Returns a new URI for @icon (a path, or already a URI), or NULL if empty. */
static char *
icon_uri (const char *icon)
{
  if (icon == NULL || *icon == '\0')
    return NULL;
  if (g_path_is_absolute (icon))
    return g_filename_to_uri (icon, NULL, NULL);
  if (strstr (icon, "://") != NULL)
    return g_strdup (icon);
  return NULL; /* a relative path means nothing here */
}

/* Returns a new FiRules (caller frees). */
FiRules *
fi_rules_new (const NuIconRule *rules, guint n, const char *const *history, gboolean enabled)
{
  FiRules *r = g_new0 (FiRules, 1);
  r->icons = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r->candidates = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < n && rules != NULL; i++)
    {
      if (!valid_name (rules[i].name))
        continue;
      g_hash_table_add (r->candidates, g_strdup (rules[i].name));
      char *uri = icon_uri (rules[i].icon); /* owned; handed to the table or freed */
      if (uri == NULL || g_hash_table_contains (r->icons, rules[i].name))
        {
          g_free (uri); /* no icon, or a later duplicate: the first rule wins */
          continue;
        }
      r->n_valid++;
      if (enabled)
        g_hash_table_insert (r->icons, g_strdup (rules[i].name), uri);
      else
        g_free (uri);
    }
  for (gsize i = 0; history != NULL && history[i] != NULL; i++)
    if (valid_name (history[i]))
      g_hash_table_add (r->candidates, g_strdup (history[i]));
  return r;
}

void
fi_rules_free (FiRules *rules)
{
  if (rules == NULL)
    return;
  g_clear_pointer (&rules->icons, g_hash_table_unref);
  g_clear_pointer (&rules->candidates, g_hash_table_unref);
  g_free (rules);
}

const char *
fi_rules_icon_for (FiRules *rules, const char *name)
{
  g_return_val_if_fail (rules != NULL, NULL);
  return name != NULL ? g_hash_table_lookup (rules->icons, name) : NULL;
}

gboolean
fi_rules_is_candidate (FiRules *rules, const char *name)
{
  g_return_val_if_fail (rules != NULL, FALSE);
  return name != NULL && g_hash_table_contains (rules->candidates, name);
}

guint
fi_rules_get_n (FiRules *rules)
{
  g_return_val_if_fail (rules != NULL, 0);
  return rules->n_valid;
}

/* Returns a new string (caller frees). */
char *
fi_marker_new (const char *icon_uri)
{
  g_return_val_if_fail (icon_uri != NULL, NULL);
  return g_strconcat (MARKER_PREFIX, icon_uri, NULL);
}

/* Returns a new string (caller frees) or NULL. */
char *
fi_marker_parse (const char *marker)
{
  if (marker == NULL || !g_str_has_prefix (marker, MARKER_PREFIX) || marker[strlen (MARKER_PREFIX)] == '\0')
    return NULL;
  return g_strdup (marker + strlen (MARKER_PREFIX));
}

FiActionKind
fi_decide (const char *rule_icon, const char *icon, const char *icon_name, const char *marker)
{
  g_autofree char *ours = fi_marker_parse (marker);
  if (marker != NULL && *marker != '\0')
    {
      if (ours == NULL || g_strcmp0 (icon, ours) != 0 || (icon_name != NULL && *icon_name != '\0'))
        return FI_ACTION_CLEAR_MARKER; /* unknown marker, or the user changed the icon since */
      if (rule_icon == NULL)
        return FI_ACTION_CLEAR; /* rule removed / feature off: restore */
      return g_str_equal (rule_icon, ours) ? FI_ACTION_NONE : FI_ACTION_SET;
    }
  if (rule_icon == NULL)
    return FI_ACTION_NONE;
  if ((icon != NULL && *icon != '\0') || (icon_name != NULL && *icon_name != '\0'))
    return FI_ACTION_NONE; /* the user's own custom icon: never touched */
  return FI_ACTION_SET;
}
