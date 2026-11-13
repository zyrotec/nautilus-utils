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
  FpDecor       decor;      /* Its back/overlay are owned refs (or NULL), released in finalize / set_decor. */
};

static void fp_folder_paintable_iface_init (GdkPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FpFolderPaintable, fp_folder_paintable, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE, fp_folder_paintable_iface_init))

void
fp_decor_init_grid (FpDecor *decor)
{
  *decor = (FpDecor) { .layout = FP_LAYOUT_GRID };
  /* The face area plus the FRAME_PAD margin the frame texture includes on each side. */
  double w = FRAME_FACE_W * (1 + 2 * FRAME_PAD), h = FRAME_FACE_H * (1 + 2 * FRAME_PAD);
  graphene_rect_init (&decor->frame_rect, (float) (FRAME_FACE_CENTER_X - w / 2), (float) (FRAME_FACE_CENTER_Y - h / 2),
                      (float) w, (float) h);
}

void
fp_decor_init_stacked (FpDecor *decor, const FpStyle *style, const graphene_rect_t *body, FpStackPosition position,
                       int offset)
{
  *decor = (FpDecor) { .layout = FP_LAYOUT_STACKED };
  double w = fp_style_stack_width (style), x = (1.0 - w) / 2; /* stack width and left edge */
  double y;
  switch (position)
    {
    case FP_STACK_TOP:
      y = STACK_EDGE;
      break;
    case FP_STACK_BOTTOM:
      y = 1.0 - STACK_EDGE - STACK_H * w;
      break;
    case FP_STACK_CENTRE:
    default:
      y = body->origin.y + STACK_BODY_TOP * body->size.height;
      break;
    }
  y += offset / REF_ICON_PX;
  double pad = STACK_PAD * w; /* the frame texture's margin, as in composite.c */
  graphene_rect_init (&decor->frame_rect, (float) (x - pad), (float) (y - pad), (float) (w + 2 * pad),
                      (float) (STACK_H * w + 2 * pad));
}

guint
fp_decor_frame_px (const FpDecor *decor, int size, int scale)
{
  if (size <= 0 || scale <= 0)
    return 0;
  return (guint) lround (decor->frame_rect.size.width * size * scale);
}

/* Scales a rectangle in icon fractions to a @width × @height icon. */
static void
scale_rect (const graphene_rect_t *unit, double width, double height, graphene_rect_t *out)
{
  graphene_rect_init (out, (float) (unit->origin.x * width), (float) (unit->origin.y * height),
                      (float) (unit->size.width * width), (float) (unit->size.height * height));
}

void
fp_folder_paintable_frame_rect (double width, double height, graphene_rect_t *out)
{
  FpDecor grid;
  fp_decor_init_grid (&grid);
  scale_rect (&grid.frame_rect, width, height, out);
}

guint
fp_folder_paintable_frame_px (int size, int scale)
{
  FpDecor grid;
  fp_decor_init_grid (&grid);
  return fp_decor_frame_px (&grid, size, scale);
}

static void
fp_folder_paintable_snapshot (GdkPaintable *paintable, GdkSnapshot *snapshot, double width, double height)
{
  FpFolderPaintable *self = FP_FOLDER_PAINTABLE (paintable);
  /* The custom back and overlay only go with a frame (V3: folders without previews keep the theme icon). */
  if (self->frame != NULL && self->decor.back != NULL)
    gtk_snapshot_append_texture (GTK_SNAPSHOT (snapshot), self->decor.back,
                                 &GRAPHENE_RECT_INIT (0, 0, (float) width, (float) height));
  else if (self->icon != NULL)
    gdk_paintable_snapshot (self->icon, snapshot, width, height);
  if (self->frame != NULL)
    {
      graphene_rect_t rect;
      scale_rect (&self->decor.frame_rect, width, height, &rect);
      /* A plain texture node: frames are built at the exact device size, so no filter choice is needed
       * (only right after a zoom, until the new size arrives, is a frame scaled, with GSK's default linear
       * filtering). The Phase 6 stall detector put most frame-time stalls in the GPU renderer while we used
       * texture-*scale* nodes, which GSK renders through an extra pass. */
      gtk_snapshot_append_texture (GTK_SNAPSHOT (snapshot), self->frame, &rect);
      if (self->decor.overlay != NULL)
        gtk_snapshot_append_texture (GTK_SNAPSHOT (snapshot), self->decor.overlay,
                                     &GRAPHENE_RECT_INIT (0, 0, (float) width, (float) height));
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
  g_clear_object (&self->decor.back);
  g_clear_object (&self->decor.overlay);
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
  fp_decor_init_grid (&self->decor);
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

void
fp_folder_paintable_set_decor (FpFolderPaintable *self, const FpDecor *decor)
{
  g_return_if_fail (FP_IS_FOLDER_PAINTABLE (self));
  FpDecor next;
  if (decor != NULL)
    next = *decor;
  else
    fp_decor_init_grid (&next);
  if (next.layout == self->decor.layout && next.back == self->decor.back && next.overlay == self->decor.overlay &&
      graphene_rect_equal (&next.frame_rect, &self->decor.frame_rect))
    return;
  /* Take the new refs before dropping the old ones (they may be the same textures). */
  if (next.back != NULL)
    g_object_ref (next.back);
  if (next.overlay != NULL)
    g_object_ref (next.overlay);
  g_clear_object (&self->decor.back);
  g_clear_object (&self->decor.overlay);
  self->decor = next; /* now owns the refs taken above */
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
