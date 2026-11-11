#include "folder_paintable.h"

#include <math.h>

#include "config.h"

struct _FpFolderPaintable
{
  GObject       parent_instance;
  GdkPaintable *icon;       /* Owned ref or NULL: the theme folder icon Nautilus provided. */
  GdkTexture   *frame;      /* Owned ref or NULL: the 2×2 preview currently shown. */
  char         *frame_uri;  /* Owned: folder the frame belongs to (NULL when no frame). */
  int           size;       /* Logical icon size = intrinsic size. */
};

static void fp_folder_paintable_iface_init (GdkPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FpFolderPaintable, fp_folder_paintable, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE, fp_folder_paintable_iface_init))

void
fp_folder_paintable_frame_rect (double width, double height, graphene_rect_t *out)
{
  /* The face area plus the FRAME_PAD margin the frame texture includes on each side. */
  double w = FRAME_FACE_W * width * (1 + 2 * FRAME_PAD), h = FRAME_FACE_H * height * (1 + 2 * FRAME_PAD);
  graphene_rect_init (out, FRAME_FACE_CENTER_X * width - w / 2, FRAME_FACE_CENTER_Y * height - h / 2, w, h);
}

guint
fp_folder_paintable_frame_px (int size, int scale)
{
  if (size <= 0 || scale <= 0)
    return 0;
  return (guint) lround (FRAME_FACE_W * (1 + 2 * FRAME_PAD) * size * scale);
}

static void
fp_folder_paintable_snapshot (GdkPaintable *paintable, GdkSnapshot *snapshot, double width, double height)
{
  FpFolderPaintable *self = FP_FOLDER_PAINTABLE (paintable);
  if (self->icon != NULL)
    gdk_paintable_snapshot (self->icon, snapshot, width, height);
  if (self->frame != NULL)
    {
      graphene_rect_t rect;
      fp_folder_paintable_frame_rect (width, height, &rect);
      /* A plain texture node: frames are built at the exact device size, so no filter choice is needed
       * (only right after a zoom, until the new size arrives, is a frame scaled, with GSK's default linear
       * filtering). The Phase 6 stall detector put most frame-time stalls in the GPU renderer while we used
       * texture-*scale* nodes, which GSK renders through an extra pass. */
      gtk_snapshot_append_texture (GTK_SNAPSHOT (snapshot), self->frame, &rect);
    }
}

static int
fp_folder_paintable_get_intrinsic_width (GdkPaintable *paintable)
{
  return FP_FOLDER_PAINTABLE (paintable)->size;
}

static int
fp_folder_paintable_get_intrinsic_height (GdkPaintable *paintable)
{
  return FP_FOLDER_PAINTABLE (paintable)->size;
}

static void
fp_folder_paintable_iface_init (GdkPaintableInterface *iface)
{
  iface->snapshot = fp_folder_paintable_snapshot;
  iface->get_intrinsic_width = fp_folder_paintable_get_intrinsic_width;
  iface->get_intrinsic_height = fp_folder_paintable_get_intrinsic_height;
}

static void
fp_folder_paintable_finalize (GObject *object)
{
  FpFolderPaintable *self = FP_FOLDER_PAINTABLE (object);
  g_clear_object (&self->icon);
  g_clear_object (&self->frame);
  g_clear_pointer (&self->frame_uri, g_free);
  G_OBJECT_CLASS (fp_folder_paintable_parent_class)->finalize (object);
}

static void
fp_folder_paintable_class_init (FpFolderPaintableClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fp_folder_paintable_finalize;
}

static void
fp_folder_paintable_init (FpFolderPaintable *self)
{
  (void) self;
}

FpFolderPaintable *
fp_folder_paintable_new (GdkPaintable *icon, int size)
{
  g_return_val_if_fail (icon == NULL || GDK_IS_PAINTABLE (icon), NULL);
  FpFolderPaintable *self = g_object_new (FP_TYPE_FOLDER_PAINTABLE, NULL);
  self->icon = icon != NULL ? g_object_ref (icon) : NULL;
  self->size = MAX (size, 0);
  return self;
}

void
fp_folder_paintable_set_icon (FpFolderPaintable *self, GdkPaintable *icon)
{
  g_return_if_fail (FP_IS_FOLDER_PAINTABLE (self));
  g_return_if_fail (icon == NULL || GDK_IS_PAINTABLE (icon));
  g_return_if_fail (icon != GDK_PAINTABLE (self)); /* would recurse forever when drawing */
  if (self->icon == icon)
    return;
  g_set_object (&self->icon, icon);
  gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
}

void
fp_folder_paintable_set_size (FpFolderPaintable *self, int size)
{
  g_return_if_fail (FP_IS_FOLDER_PAINTABLE (self));
  size = MAX (size, 0);
  if (self->size == size)
    return;
  self->size = size;
  gdk_paintable_invalidate_size (GDK_PAINTABLE (self));
}

void
fp_folder_paintable_set_frame (FpFolderPaintable *self, GdkTexture *frame, const char *uri)
{
  g_return_if_fail (FP_IS_FOLDER_PAINTABLE (self));
  g_return_if_fail (frame == NULL || GDK_IS_TEXTURE (frame));
  if (self->frame == frame && g_strcmp0 (self->frame_uri, frame != NULL ? uri : NULL) == 0)
    return;
  g_set_object (&self->frame, frame);
  g_free (self->frame_uri);
  self->frame_uri = frame != NULL ? g_strdup (uri) : NULL;
  gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
}

GdkPaintable *
fp_folder_paintable_get_icon (FpFolderPaintable *self)
{
  g_return_val_if_fail (FP_IS_FOLDER_PAINTABLE (self), NULL);
  return self->icon;
}

GdkTexture *
fp_folder_paintable_get_frame (FpFolderPaintable *self)
{
  g_return_val_if_fail (FP_IS_FOLDER_PAINTABLE (self), NULL);
  return self->frame;
}

const char *
fp_folder_paintable_get_frame_uri (FpFolderPaintable *self)
{
  g_return_val_if_fail (FP_IS_FOLDER_PAINTABLE (self), NULL);
  return self->frame_uri;
}
