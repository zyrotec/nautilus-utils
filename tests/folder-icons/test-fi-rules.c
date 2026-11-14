/* Tests for src/folder-icons/fi_rules.c (PLAN.md §12.7): rule snapshot, markers, and the decision table. */
#include "fi_rules.h"

static const NuIconRule sample[] = {
  { "node_modules", "/icons/node.svg" },
  { ".git", "file:///icons/git.png" },
  { "node_modules", "/icons/other.svg" }, /* duplicate: the first rule wins */
  { "", "/icons/x.svg" },                 /* invalid: empty name */
  { "a/b", "/icons/x.svg" },              /* invalid: not a single name */
  { "no-icon", "" },                      /* no icon: still a candidate (restore) */
  { "relative", "icons/x.svg" },          /* relative path: no icon */
};

static void
test_rules (void)
{
  const char *history[] = { "vendor", "", "x/y", NULL };
  g_autoptr (FiRules) r = fi_rules_new (sample, G_N_ELEMENTS (sample), history, TRUE);
  g_assert_cmpuint (fi_rules_get_n (r), ==, 2);
  g_assert_cmpstr (fi_rules_icon_for (r, "node_modules"), ==, "file:///icons/node.svg");
  g_assert_cmpstr (fi_rules_icon_for (r, ".git"), ==, "file:///icons/git.png");
  /* Exact, case-sensitive, whole name. */
  g_assert_null (fi_rules_icon_for (r, "Node_Modules"));
  g_assert_null (fi_rules_icon_for (r, "node_modules_old"));
  g_assert_null (fi_rules_icon_for (r, "no-icon"));
  g_assert_null (fi_rules_icon_for (r, NULL));
  g_assert_true (fi_rules_is_candidate (r, "node_modules"));
  g_assert_true (fi_rules_is_candidate (r, "no-icon"));
  g_assert_true (fi_rules_is_candidate (r, "relative"));
  g_assert_true (fi_rules_is_candidate (r, "vendor"));  /* history */
  g_assert_false (fi_rules_is_candidate (r, ""));
  g_assert_false (fi_rules_is_candidate (r, "x/y"));
  g_assert_false (fi_rules_is_candidate (r, "src"));
  g_assert_false (fi_rules_is_candidate (r, NULL));

  /* Switched off: no icons, but every name is still checked so marked folders get restored. */
  g_autoptr (FiRules) off = fi_rules_new (sample, G_N_ELEMENTS (sample), history, FALSE);
  g_assert_null (fi_rules_icon_for (off, "node_modules"));
  g_assert_true (fi_rules_is_candidate (off, "node_modules"));
  g_assert_true (fi_rules_is_candidate (off, "vendor"));

  g_autoptr (FiRules) empty = fi_rules_new (NULL, 0, NULL, TRUE);
  g_assert_cmpuint (fi_rules_get_n (empty), ==, 0);
  g_assert_false (fi_rules_is_candidate (empty, "node_modules"));
}

static void
test_marker (void)
{
  g_autofree char *m = fi_marker_new ("file:///icons/node.svg");
  g_assert_cmpstr (m, ==, "1;file:///icons/node.svg");
  g_autofree char *back = fi_marker_parse (m);
  g_assert_cmpstr (back, ==, "file:///icons/node.svg");
  g_assert_null (fi_marker_parse (NULL));
  g_assert_null (fi_marker_parse (""));
  g_assert_null (fi_marker_parse ("1;"));
  g_assert_null (fi_marker_parse ("2;file:///x"));
  g_assert_null (fi_marker_parse ("garbage"));
}

/* The decision table of PLAN.md §12.7. */
static void
test_decide (void)
{
  const char *R = "file:///icons/node.svg", *R2 = "file:///icons/new.svg";
  g_autofree char *M = fi_marker_new (R);

  /* No marker. */
  g_assert_cmpint (fi_decide (R, NULL, NULL, NULL), ==, FI_ACTION_SET);
  g_assert_cmpint (fi_decide (R, "", "", NULL), ==, FI_ACTION_SET);            /* empty = unset */
  g_assert_cmpint (fi_decide (R, "file:///mine.png", NULL, NULL), ==, FI_ACTION_NONE); /* user's icon */
  g_assert_cmpint (fi_decide (R, NULL, "folder-music", NULL), ==, FI_ACTION_NONE);     /* user's icon name */
  g_assert_cmpint (fi_decide (NULL, NULL, NULL, NULL), ==, FI_ACTION_NONE);
  g_assert_cmpint (fi_decide (NULL, "file:///mine.png", NULL, NULL), ==, FI_ACTION_NONE);

  /* Our marker, icon unchanged since. */
  g_assert_cmpint (fi_decide (R, R, NULL, M), ==, FI_ACTION_NONE);    /* already done */
  g_assert_cmpint (fi_decide (R2, R, NULL, M), ==, FI_ACTION_SET);    /* rule's icon changed */
  g_assert_cmpint (fi_decide (NULL, R, NULL, M), ==, FI_ACTION_CLEAR); /* rule removed / feature off */

  /* Our marker, but the user changed the icon since: only our marker goes, whatever the rule says. */
  g_assert_cmpint (fi_decide (R, "file:///mine.png", NULL, M), ==, FI_ACTION_CLEAR_MARKER);
  g_assert_cmpint (fi_decide (NULL, "file:///mine.png", NULL, M), ==, FI_ACTION_CLEAR_MARKER);
  g_assert_cmpint (fi_decide (R, NULL, NULL, M), ==, FI_ACTION_CLEAR_MARKER);         /* user removed it */
  g_assert_cmpint (fi_decide (R, R, "folder-music", M), ==, FI_ACTION_CLEAR_MARKER);  /* user set a name */
  g_assert_cmpint (fi_decide (R, R, NULL, "garbage"), ==, FI_ACTION_CLEAR_MARKER);    /* unknown marker */
  g_assert_cmpint (fi_decide (R, NULL, NULL, ""), ==, FI_ACTION_SET);                 /* empty = unset */
}

static void
test_stress (void)
{
  for (int i = 0; i < 2000; i++)
    {
      g_autoptr (FiRules) r = fi_rules_new (sample, (guint) (i % G_N_ELEMENTS (sample)) + 1,
                                            (const char *[]) { "vendor", NULL }, i % 2 == 0);
      g_assert_true (fi_rules_is_candidate (r, "node_modules"));
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fi-rules/rules", test_rules);
  g_test_add_func ("/fi-rules/marker", test_marker);
  g_test_add_func ("/fi-rules/decide", test_decide);
  g_test_add_func ("/fi-rules/stress", test_stress);
  return g_test_run ();
}
