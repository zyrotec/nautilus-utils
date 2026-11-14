/* Folder icons by name: rules and decisions (PLAN.md §12.7). GLib only, no I/O, no Nautilus types.
 *
 * An FiRules is an immutable snapshot of the settings: which folder names get which icon, and which names
 * are still worth checking because a removed rule may have marked folders with them. fi_decide() says what
 * to do with one folder, given what its metadata holds now.
 */
#pragma once

#include <glib.h>

#include "nu_settings.h"

G_BEGIN_DECLS

/* Our marker attribute, and Files' custom-icon attributes (PLAN.md §12.7 item 1). */
#define FI_ATTR_MARKER    "metadata::nautilus-utils-icon-rule"
#define FI_ATTR_ICON      "metadata::custom-icon"
#define FI_ATTR_ICON_NAME "metadata::custom-icon-name"

typedef struct _FiRules FiRules;

/* Snapshot of @n rules (strings copied) and the names in @history (NULL-terminated or NULL). Rules with an
 * empty name, a name containing '/', or an empty icon are ignored; for a duplicate name the first rule wins.
 * With @enabled FALSE no name has an icon, but every rule and history name is still a candidate, so marked
 * folders get restored. Caller owns the result. */
FiRules *fi_rules_new (const NuIconRule *rules,
                       guint             n,
                       const char *const *history,
                       gboolean          enabled);

void fi_rules_free (FiRules *rules);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FiRules, fi_rules_free)

/* The icon (a file:// URI) for folders called @name, or NULL. Borrowed, valid while @rules lives. */
const char *fi_rules_icon_for (FiRules    *rules,
                               const char *name);

/* TRUE if folders called @name must be checked (a rule or history name). */
gboolean fi_rules_is_candidate (FiRules    *rules,
                                const char *name);

/* Number of valid rules (for logs and tests). */
guint fi_rules_get_n (FiRules *rules);

/* Marker value for an icon we set: "1;<icon uri>". Caller frees. */
char *fi_marker_new (const char *icon_uri);

/* The icon URI in a marker, or NULL if @marker is NULL or not a known format. Caller frees. */
char *fi_marker_parse (const char *marker);

typedef enum
{
  FI_ACTION_NONE,         /* leave the folder alone */
  FI_ACTION_SET,          /* write icon_uri as the custom icon, and its marker */
  FI_ACTION_CLEAR,        /* remove the custom icon and our marker (restore) */
  FI_ACTION_CLEAR_MARKER, /* the user changed the icon since: remove only our marker */
} FiActionKind;

/* What to do for a folder whose rule icon is @rule_icon (NULL = no rule / feature off) and whose metadata
 * holds custom icon @icon, custom icon name @icon_name and our marker @marker (each NULL if unset).
 * For FI_ACTION_SET the icon to write is @rule_icon. Never touches a custom icon we didn't set. */
FiActionKind fi_decide (const char *rule_icon,
                        const char *icon,
                        const char *icon_name,
                        const char *marker);

G_END_DECLS
