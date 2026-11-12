/* Tests for src/folder-views/fv_state.c (PLAN.md §10.6 Phase 1). */
#include <glib.h>

#include <string.h>

#include "config.h"
#include "fv_state.h"

static void
assert_view_equal (const NuFolderView *a, const NuFolderView *b)
{
  g_assert_cmpint (a->view, ==, b->view);
  g_assert_cmpuint (a->grid_zoom, ==, b->grid_zoom);
  g_assert_cmpuint (a->list_zoom, ==, b->list_zoom);
}

/* Every valid view round-trips through encode/decode, for several generations. */
static void
test_round_trip (void)
{
  const guint gens[] = { 0, 1, 7, 4096, G_MAXUINT };
  for (gsize g = 0; g < G_N_ELEMENTS (gens); g++)
    for (int view = NU_VIEW_LIST; view <= NU_VIEW_GRID; view++)
      for (int gz = 0; gz <= NU_GRID_ZOOM_MAX; gz++)
        for (int lz = 0; lz <= NU_LIST_ZOOM_MAX; lz++)
          {
            NuFolderView in = { (NuViewType) view, (guint8) gz, (guint8) lz };
            g_autofree char *text = fv_encode (&in, gens[g]);
            g_assert_nonnull (text);
            NuFolderView out = { 0 };
            g_assert_true (fv_decode (text, gens[g], &out));
            assert_view_equal (&in, &out);
          }
}

static void
test_encode_format (void)
{
  NuFolderView v = { NU_VIEW_GRID, 3, 1 };
  g_autofree char *text = fv_encode (&v, 12);
  g_assert_cmpstr (text, ==, "1;12;2;3;1");
}

/* Malformed, out-of-range, other-version and other-generation values are rejected, @out untouched. */
static void
test_decode_rejects (void)
{
  const char *bad[] = {
    NULL, "", ";", "1;0;2;2", "1;0;2;2;1;9", "1;0;2;2;1;", ";1;0;2;2;1",  /* field count */
    "2;0;2;2;1", "0;0;2;2;1", "x;0;2;2;1",                                /* version */
    "1;1;2;2;1",                                                          /* other generation */
    "1;0;0;2;1", "1;0;3;2;1", "1;0;9;2;1",                                /* view */
    "1;0;2;5;1", "1;0;2;2;3", "1;0;1;9;9",                                /* zoom range */
    "1;0;2;-1;1", "1;0;+2;2;1", "1;0; 2;2;1", "1;0;2;2;1 ", "1;0;2;2;1\n", /* not plain digits */
    "1;0;2;2;0x1", "1;0;2.0;2;1", "1;0;2;;1",
    "1;00000000000;2;2;1",                                                /* field too long */
    "1;0;2;2;1aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", /* too long */
    "1;0;2;2;\xc3\xa9",
  };
  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      NuFolderView keep = { NU_VIEW_LIST, 4, 2 };
      g_assert_false (fv_decode (bad[i], 0, &keep));
      g_assert_cmpint (keep.view, ==, NU_VIEW_LIST);
      g_assert_cmpuint (keep.grid_zoom, ==, 4);
      g_assert_cmpuint (keep.list_zoom, ==, 2);
    }
  /* Generation larger than a guint can hold. */
  NuFolderView out;
  g_assert_false (fv_decode ("1;4294967296;2;2;1", 0, &out));
  g_assert_true (fv_decode ("1;4294967295;2;2;1", G_MAXUINT, &out));
}

/* Reset All: values saved before the generation changed no longer count. */
static void
test_generation_reset (void)
{
  NuFolderView v = { NU_VIEW_LIST, 0, 2 }, out;
  g_autofree char *gen0 = fv_encode (&v, 0);
  g_assert_true (fv_decode (gen0, 0, &out));
  g_assert_false (fv_decode (gen0, 1, &out));
  g_autofree char *gen1 = fv_encode (&v, 1);
  g_assert_true (fv_decode (gen1, 1, &out));
  g_assert_false (fv_decode (gen1, 0, &out));
}

static void
test_encode_invalid (void)
{
  if (g_test_subprocess ())
    {
      NuFolderView bad = { 7, 0, 0 };
      g_autofree char *text = fv_encode (&bad, 0);
      g_assert_null (text);
      return;
    }
  g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed (); /* g_return_val_if_fail is fatal under G_DEBUG=fatal-warnings */
  g_test_trap_assert_stderr ("*nu_folder_view_valid*");
}

static void
test_location_kind (void)
{
  static const struct
  {
    const char    *uri;
    FvLocationKind kind;
  } cases[] = {
    { "file:///home/user/Pictures", FV_LOCATION_SAVE },
    { "file:///", FV_LOCATION_SAVE },
    { "file:///mnt/Personal%20Drive", FV_LOCATION_SAVE },
    { "trash:///", FV_LOCATION_SAVE },
    { "recent:///", FV_LOCATION_SAVE },
    { "smb://server/share/dir", FV_LOCATION_SAVE },
    { "sftp://host/home", FV_LOCATION_SAVE },
    { "network:///", FV_LOCATION_IGNORE },
    { "network://", FV_LOCATION_IGNORE },
    { "NETWORK:///", FV_LOCATION_IGNORE },
    { "network:///dnssd-domain-local", FV_LOCATION_SAVE },
    { "starred:///", FV_LOCATION_DEFAULT_ONLY },
    { "other-locations:///", FV_LOCATION_DEFAULT_ONLY },
    { "x-nautilus-search://0/", FV_LOCATION_DEFAULT_ONLY },
    { NULL, FV_LOCATION_IGNORE },
    { "", FV_LOCATION_IGNORE },
    { "/home/user", FV_LOCATION_IGNORE },
    { "not a uri", FV_LOCATION_IGNORE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_test_message ("uri: %s", cases[i].uri);
      g_assert_cmpint (fv_location_kind (cases[i].uri), ==, cases[i].kind);
    }
}

static void
test_zoom_sizes (void)
{
  /* Every level maps to a size and back. */
  for (int z = 0; z <= NU_GRID_ZOOM_MAX; z++)
    g_assert_cmpint (fv_zoom_for_icon_size (NU_VIEW_GRID, fv_icon_size_for_zoom (NU_VIEW_GRID, z)), ==, z);
  for (int z = 0; z <= NU_LIST_ZOOM_MAX; z++)
    g_assert_cmpint (fv_zoom_for_icon_size (NU_VIEW_LIST, fv_icon_size_for_zoom (NU_VIEW_LIST, z)), ==, z);
  /* Nautilus 51.1 values measured in Phase 0. */
  g_assert_cmpuint (fv_icon_size_for_zoom (NU_VIEW_GRID, 2), ==, 96);
  g_assert_cmpuint (fv_icon_size_for_zoom (NU_VIEW_GRID, 4), ==, 256);
  g_assert_cmpuint (fv_icon_size_for_zoom (NU_VIEW_LIST, 1), ==, 32);
  /* Unknown sizes, levels and views. */
  g_assert_cmpint (fv_zoom_for_icon_size (NU_VIEW_GRID, 32), ==, -1);
  g_assert_cmpint (fv_zoom_for_icon_size (NU_VIEW_LIST, 96), ==, -1);
  g_assert_cmpint (fv_zoom_for_icon_size ((NuViewType) 3, 96), ==, -1);
  g_assert_cmpuint (fv_icon_size_for_zoom (NU_VIEW_GRID, -1), ==, 0);
  g_assert_cmpuint (fv_icon_size_for_zoom (NU_VIEW_GRID, 5), ==, 0);
  g_assert_cmpuint (fv_icon_size_for_zoom (NU_VIEW_LIST, 3), ==, 0);
  g_assert_cmpuint (fv_icon_size_for_zoom ((NuViewType) 0, 1), ==, 0);

  NuFolderView g = { NU_VIEW_GRID, 3, 0 }, l = { NU_VIEW_LIST, 3, 0 };
  g_assert_cmpint (fv_folder_view_zoom (&g), ==, 3);
  g_assert_cmpint (fv_folder_view_zoom (&l), ==, 0);
}

static void
test_cache_basic (void)
{
  g_autoptr (FvCache) c = fv_cache_new (8);
  NuFolderView v = { NU_VIEW_GRID, 4, 1 }, out = { NU_VIEW_LIST, 0, 0 };
  g_assert_cmpint (fv_cache_lookup (c, "file:///a", &out), ==, FV_CACHE_MISS);
  g_assert_cmpint (fv_cache_lookup (c, NULL, &out), ==, FV_CACHE_MISS);

  fv_cache_store (c, "file:///a", &v);
  fv_cache_store (c, "file:///b", NULL);
  g_assert_cmpuint (fv_cache_size (c), ==, 2);
  g_assert_cmpint (fv_cache_lookup (c, "file:///a", &out), ==, FV_CACHE_SAVED);
  assert_view_equal (&v, &out);

  NuFolderView untouched = { NU_VIEW_LIST, 1, 1 };
  g_assert_cmpint (fv_cache_lookup (c, "file:///b", &untouched), ==, FV_CACHE_NONE);
  g_assert_cmpint (untouched.view, ==, NU_VIEW_LIST);

  /* Overwrite: none → saved, saved → none, same entry. */
  fv_cache_store (c, "file:///b", &v);
  g_assert_cmpint (fv_cache_lookup (c, "file:///b", &out), ==, FV_CACHE_SAVED);
  fv_cache_store (c, "file:///a", NULL);
  g_assert_cmpint (fv_cache_lookup (c, "file:///a", &out), ==, FV_CACHE_NONE);
  g_assert_cmpuint (fv_cache_size (c), ==, 2);

  /* The cache copies the URI. */
  g_autofree char *uri = g_strdup ("file:///c");
  fv_cache_store (c, uri, &v);
  uri[8] = 'x';
  g_assert_cmpint (fv_cache_lookup (c, "file:///c", &out), ==, FV_CACHE_SAVED);

  fv_cache_clear (c);
  g_assert_cmpuint (fv_cache_size (c), ==, 0);
  g_assert_cmpint (fv_cache_lookup (c, "file:///b", &out), ==, FV_CACHE_MISS);
  fv_cache_store (c, "file:///b", NULL); /* usable after clear */
  g_assert_cmpuint (fv_cache_size (c), ==, 1);
  fv_cache_free (NULL);
}

/* Least recently used entries go first; a lookup counts as use. */
static void
test_cache_lru (void)
{
  g_autoptr (FvCache) c = fv_cache_new (3);
  NuFolderView v = { NU_VIEW_LIST, 0, 0 }, out;
  fv_cache_store (c, "a", &v);
  fv_cache_store (c, "b", &v);
  fv_cache_store (c, "c", &v);
  g_assert_cmpint (fv_cache_lookup (c, "a", &out), ==, FV_CACHE_SAVED); /* a is now the newest */
  fv_cache_store (c, "d", &v);                                          /* drops b */
  g_assert_cmpuint (fv_cache_size (c), ==, 3);
  g_assert_cmpint (fv_cache_lookup (c, "b", &out), ==, FV_CACHE_MISS);
  g_assert_cmpint (fv_cache_lookup (c, "c", &out), ==, FV_CACHE_SAVED); /* order now: c d a */
  fv_cache_store (c, "d", NULL);                                        /* re-store = use: d c a */
  fv_cache_store (c, "e", &v);                                          /* drops a */
  g_assert_cmpint (fv_cache_lookup (c, "a", &out), ==, FV_CACHE_MISS);
  g_assert_cmpint (fv_cache_lookup (c, "c", &out), ==, FV_CACHE_SAVED);
  g_assert_cmpint (fv_cache_lookup (c, "d", &out), ==, FV_CACHE_NONE);
  g_assert_cmpint (fv_cache_lookup (c, "e", &out), ==, FV_CACHE_SAVED);
}

/* Fill far past the limit, with lookups and clears in between: size stays bounded, nothing leaks. */
static void
test_cache_stress (void)
{
  g_autoptr (FvCache) c = fv_cache_new (FV_CACHE_MAX);
  NuFolderView out;
  for (guint round = 0; round < 5; round++)
    {
      for (guint i = 0; i < 3 * FV_CACHE_MAX; i++)
        {
          g_autofree char *uri = g_strdup_printf ("file:///stress/%u/%u", round, i);
          NuFolderView v = { (i % 2) ? NU_VIEW_GRID : NU_VIEW_LIST, (guint8) (i % 5), (guint8) (i % 3) };
          fv_cache_store (c, uri, (i % 7) ? &v : NULL);
          if (i % 3 == 0 && i > 0)
            {
              g_autofree char *prev = g_strdup_printf ("file:///stress/%u/%u", round, i - 1);
              g_assert_cmpint (fv_cache_lookup (c, prev, &out), !=, FV_CACHE_MISS);
            }
        }
      g_assert_cmpuint (fv_cache_size (c), ==, FV_CACHE_MAX);
      /* The oldest of this round are gone, the newest are there. */
      g_autofree char *oldest = g_strdup_printf ("file:///stress/%u/0", round);
      g_assert_cmpint (fv_cache_lookup (c, oldest, &out), ==, FV_CACHE_MISS);
      g_autofree char *newest = g_strdup_printf ("file:///stress/%u/%u", round, 3 * FV_CACHE_MAX - 2);
      g_assert_cmpint (fv_cache_lookup (c, newest, &out), ==, FV_CACHE_SAVED);
      if (round % 2)
        fv_cache_clear (c);
    }
  /* Many create/free cycles. */
  for (guint i = 0; i < 1000; i++)
    {
      FvCache *t = fv_cache_new (1 + i % 4);
      NuFolderView v = { NU_VIEW_GRID, 0, 0 };
      for (guint j = 0; j < 6; j++)
        {
          g_autofree char *uri = g_strdup_printf ("u%u", j);
          fv_cache_store (t, uri, &v);
        }
      g_assert_cmpuint (fv_cache_size (t), ==, 1 + i % 4);
      fv_cache_free (t);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fv-state/round-trip", test_round_trip);
  g_test_add_func ("/fv-state/encode-format", test_encode_format);
  g_test_add_func ("/fv-state/decode-rejects", test_decode_rejects);
  g_test_add_func ("/fv-state/generation-reset", test_generation_reset);
  g_test_add_func ("/fv-state/encode-invalid", test_encode_invalid);
  g_test_add_func ("/fv-state/location-kind", test_location_kind);
  g_test_add_func ("/fv-state/zoom-sizes", test_zoom_sizes);
  g_test_add_func ("/fv-state/cache-basic", test_cache_basic);
  g_test_add_func ("/fv-state/cache-lru", test_cache_lru);
  g_test_add_func ("/fv-state/cache-stress", test_cache_stress);
  return g_test_run ();
}
