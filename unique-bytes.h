#include <glib.h>

GBytes * g_bytes_new_unique_async (gconstpointer data, gsize len);
GBytes * g_bytes_new_unique_sync (gconstpointer data, gsize len);
