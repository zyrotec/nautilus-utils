/* Git repository detection. See gt_repo.h. */
#define _POSIX_C_SOURCE 200809L
#include "gt_repo.h"

#include <string.h>
#include <sys/stat.h>

/* The git folder a ".git" file points to ("gitdir: X", X relative to the file's folder), or NULL. */
static char *
read_gitdir_file (const char *dot_git, const char *dir)
{
  g_autofree char *text = NULL;
  gsize len = 0;
  if (!g_file_get_contents (dot_git, &text, &len, NULL) || len > 4096 || !g_str_has_prefix (text, "gitdir: "))
    return NULL;
  g_autofree char *target = g_strstrip (g_strdup (text + strlen ("gitdir: ")));
  if (*target == '\0')
    return NULL;
  char *abs = g_path_is_absolute (target) ? g_strdup (target) : g_build_filename (dir, target, NULL);
  char *canon = g_canonicalize_filename (abs, NULL);
  g_free (abs);
  return canon;
}

char *
gt_repo_find_root (const char *path, char **git_dir)
{
  if (git_dir != NULL)
    *git_dir = NULL;
  if (path == NULL || !g_path_is_absolute (path))
    return NULL;
  g_autofree char *dir = g_canonicalize_filename (path, NULL);
  struct stat top;
  if (stat (dir, &top) != 0)
    return NULL;
  for (;;)
    {
      struct stat st;
      if (stat (dir, &st) != 0 || st.st_dev != top.st_dev)
        return NULL; /* left the filesystem: git stops there too (GIT_DISCOVERY_ACROSS_FILESYSTEM) */
      g_autofree char *dot_git = g_build_filename (dir, ".git", NULL);
      struct stat gs;
      if (stat (dot_git, &gs) == 0)
        {
          char *gd = NULL;
          if (S_ISDIR (gs.st_mode))
            {
              g_autofree char *head = g_build_filename (dot_git, "HEAD", NULL);
              if (g_file_test (head, G_FILE_TEST_IS_REGULAR))
                gd = g_strdup (dot_git);
            }
          else if (S_ISREG (gs.st_mode))
            gd = read_gitdir_file (dot_git, dir);
          if (gd != NULL)
            {
              if (git_dir != NULL)
                *git_dir = gd;
              else
                g_free (gd);
              return g_steal_pointer (&dir);
            }
        }
      g_autofree char *parent = g_path_get_dirname (dir);
      if (g_str_equal (parent, dir))
        return NULL;
      g_free (dir);
      dir = g_steal_pointer (&parent);
    }
}

char *
gt_repo_relative (const char *root, const char *path)
{
  if (root == NULL || path == NULL)
    return NULL;
  gsize rl = strlen (root);
  while (rl > 1 && root[rl - 1] == '/')
    rl--;
  if (strncmp (path, root, rl) != 0)
    return NULL;
  const char *rest = path + rl;
  if (*rest == '\0')
    return g_strdup ("");
  if (*rest != '/' && !(rl == 1 && root[0] == '/'))
    return NULL;
  while (*rest == '/')
    rest++;
  char *out = g_strdup (rest);
  gsize n = strlen (out);
  while (n > 0 && out[n - 1] == '/')
    out[--n] = '\0';
  return out;
}
