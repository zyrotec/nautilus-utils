/* Minimal NautilusFileInfo implementation for tests (Nautilus' real one only exists inside Nautilus). */
#pragma once

#include <nautilus-extension.h>

G_BEGIN_DECLS

#define FAKE_TYPE_FILE_INFO (fake_file_info_get_type ())
G_DECLARE_FINAL_TYPE (FakeFileInfo, fake_file_info, FAKE, FILE_INFO, GObject)

/* Caller owns the returned object; free with g_object_unref(). @uri is copied. */
FakeFileInfo *fake_file_info_new (const char *uri,
                                  gboolean    is_directory);

/* Returns a sunk closure like the one Nautilus passes as update_complete; the caller owns it and must
 * g_closure_unref() it. Each invocation increments *n_invoked (which must outlive the closure). */
GClosure *fake_update_closure_new (guint *n_invoked);

G_END_DECLS
