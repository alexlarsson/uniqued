#define _GNU_SOURCE         /* See feature_test_macros(7) */

#include "unique-bytes.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#define ALL_SEALS (F_SEAL_SEAL | \
                   F_SEAL_SHRINK | \
                   F_SEAL_GROW |   \
                   F_SEAL_WRITE)

static gboolean
write_all_to_fd (int fd, const guchar *data, gsize len)
{
  while (len > 0)
    {
      ssize_t res = write (fd, data, len);
      if (res < 0)
        {
          if (errno == EINTR)
            continue;
          return FALSE;
        }

      len -= res;
      data += res;
    }

  return TRUE;
}

static int
steal_one_fd_from_list (GUnixFDList *fd_list,
                        gint32 handle)
{
  int n_fds, i, fd, *fds;

  if (fd_list == NULL ||
      handle >= g_unix_fd_list_get_length (fd_list))
    return -1;

  fds = g_unix_fd_list_steal_fds (fd_list, &n_fds);
  for (i = 0; i < n_fds; i++)
    {
      if (i != handle)
        close (fds[i]);
    }

  fd = fds[handle];
  g_free (fds);
  return fd;
}

/* We keek the bus alive in a static local because we need the client to keep living */
static GDBusConnection *
get_bus (void)
{
  static gsize bus = 0;

  if (g_once_init_enter (&bus))
    {
      GDBusConnection *the_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
      g_once_init_leave (&bus, (gsize)the_bus);
    }

  return (GDBusConnection *)bus;
}


static void
call_forget (guint32 id)
{
  GDBusConnection *bus = get_bus ();

  if (bus != NULL)
    g_dbus_connection_call (bus,
                            "org.freedesktop.portal.Unique",
                            "/org/freedesktop/portal/unique",
                            "org.freedesktop.portal.Unique",
                            "Forget",
                            g_variant_new ("(u)", id),
                            G_VARIANT_TYPE ("()"),
                            G_DBUS_CALL_FLAGS_NONE,
                            G_MAXINT,
                            NULL, NULL, NULL);
}

typedef struct {
  guint ref_count;
  gpointer data;
  gsize len;
  guint32 id;
} MappedData;

static MappedData *
mapped_data_new (gpointer data, gsize len)
{
  MappedData *d = g_slice_new0 (MappedData);
  d->data = data;
  d->len = len;
  d->ref_count = 1;
  return d;
}

static MappedData *
mapped_data_ref (MappedData *d)
{
  d->ref_count++;
  return d;
}

static void
mapped_data_unref (MappedData *d)
{
  d->ref_count--;
  if (d->ref_count == 0)
    {
      munmap (d->data, d->len);
      if (d->id != 0)
        call_forget (d->id);
      g_slice_free (MappedData, d);
    }
}


static gboolean
call_make_unique (int *memfd,
                  guint32 *id_out)
{
  GDBusConnection *bus = get_bus ();
  gboolean result = FALSE;
  GUnixFDList *fd_list;
  gint memfd_handle;

  if (bus == NULL)
    return FALSE;

  fd_list = g_unix_fd_list_new ();
  memfd_handle = g_unix_fd_list_append (fd_list, *memfd, NULL);
  if (memfd_handle != -1)
    {
      GUnixFDList *response_fd_list;
      GVariant *response =
        g_dbus_connection_call_with_unix_fd_list_sync (bus,
                                                       "org.freedesktop.portal.Unique",
                                                       "/org/freedesktop/portal/unique",
                                                       "org.freedesktop.portal.Unique",
                                                       "MakeUnique",
                                                       g_variant_new ("(h)", memfd_handle),
                                                       G_VARIANT_TYPE ("(ahu)"),
                                                       G_DBUS_CALL_FLAGS_NONE,
                                                       1000, /* msec timeout */
                                                       fd_list, &response_fd_list,
                                                       NULL, NULL);
      if (response != NULL)
        {
          GVariantIter *handle_iter;
          GVariant *handle_v;
          guint32 id;

          g_variant_get (response, "(ahu)", &handle_iter, &id);

          handle_v = g_variant_iter_next_value (handle_iter);
          if (handle_v)
            {
              int new_memfd = steal_one_fd_from_list (response_fd_list, g_variant_get_handle (handle_v));
              if (new_memfd != -1)
                {
                  close (*memfd);
                  *memfd = new_memfd;
                }

              g_variant_unref (handle_v);
            }
          g_variant_iter_free (handle_iter);

          *id_out = id;

          result = TRUE;

          g_clear_object (&response_fd_list);
          g_variant_unref (response);
        }
    }
    return FALSE;


  g_object_unref (fd_list);
  return result;
}

static void
make_unique_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  MappedData *mapped_data = user_data;
  GVariant *response;
  GUnixFDList *response_fd_list;
  guint32 id;

  response = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source_object),
                                                              &response_fd_list, res, NULL);
  if (response != NULL)
    {
      GVariantIter *handle_iter;
      GVariant *handle_v;

      g_variant_get (response, "(ahu)", &handle_iter, &id);

      handle_v = g_variant_iter_next_value (handle_iter);
      if (handle_v)
        {
          int new_memfd = steal_one_fd_from_list (response_fd_list, g_variant_get_handle (handle_v));
          if (new_memfd != -1)
            {
              /* Switch out the mapping to the new version */
              void *memfd_data = mmap (mapped_data->data, mapped_data->len, PROT_READ, MAP_PRIVATE | MAP_FIXED, new_memfd, 0);
              g_assert (memfd_data == mapped_data->data);
              close (new_memfd);
            }
          g_variant_unref (handle_v);
        }
      g_variant_iter_free (handle_iter);

      /* Ensure we forget the new blob */
      mapped_data->id = id;

      g_clear_object (&response_fd_list);
      g_variant_unref (response);
    }

  mapped_data_unref (mapped_data);
}

static void
call_make_unique_async (int memfd, MappedData *data)
{
  GDBusConnection *bus = get_bus ();
  GUnixFDList *fd_list = NULL;
  gint handle;

  if (bus == NULL)
    return;

  fd_list = g_unix_fd_list_new ();
  handle = g_unix_fd_list_append (fd_list, memfd, NULL);
  if (handle != -1)
    g_dbus_connection_call_with_unix_fd_list (bus,
                                              "org.freedesktop.portal.Unique",
                                              "/org/freedesktop/portal/unique",
                                              "org.freedesktop.portal.Unique",
                                              "MakeUnique",
                                              g_variant_new ("(h)", handle),
                                              G_VARIANT_TYPE ("(ahu)"),
                                              G_DBUS_CALL_FLAGS_NONE,
                                              G_MAXINT, /* No timeout */
                                              fd_list, NULL,
                                              make_unique_cb,
                                              mapped_data_ref (data));
  g_object_unref (fd_list);
}

static int
create_sealed_memfd_for_data (gconstpointer data, gsize len)
{
  int memfd = -1;
  static int count = 0;
  char *full_name = g_strdup_printf ("unique-%d-%d", getpid (), count++);

  memfd = memfd_create (full_name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
  g_free (full_name);
  if (memfd < 0)
    return -1;

  if (ftruncate (memfd, len) == 0 &&
      write_all_to_fd (memfd, data, len) &&
      fcntl (memfd, F_ADD_SEALS, (int) F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE) == 0)
    return memfd;

  close (memfd);
  return -1;
}

GBytes *
g_bytes_new_unique_sync (gconstpointer data, gsize len)
{
  int memfd = -1;
  void *memfd_data = NULL;
  guint32 id = 0;

  memfd = create_sealed_memfd_for_data (data, len);
  if (memfd >= 0)
    {
      if (call_make_unique (&memfd, &id))
        memfd_data = mmap (NULL, len, PROT_READ, MAP_PRIVATE, memfd, 0);
      close (memfd);

      if (memfd_data)
        {
          MappedData *d = mapped_data_new (memfd_data, len);
          d->id = id;
          return g_bytes_new_with_free_func (memfd_data, len, (GDestroyNotify)mapped_data_unref, d);
        }
    }

  /* Fall back to regular copy */
  return g_bytes_new (data, len);
}

GBytes *
g_bytes_new_unique_async (gconstpointer data, gsize len)
{
  int memfd = -1;
  void *memfd_data = NULL;

  memfd = create_sealed_memfd_for_data (data, len);
  if (memfd >= 0)
    {
      memfd_data = mmap (NULL, len, PROT_READ, MAP_PRIVATE, memfd, 0);
      if (memfd_data)
        {
          MappedData *d = mapped_data_new (memfd_data, len);
          call_make_unique_async (memfd, d);
          close (memfd);
          return g_bytes_new_with_free_func (memfd_data, len, (GDestroyNotify)mapped_data_unref, d);
        }

      close (memfd);
    }

  return g_bytes_new (data, len); /* Fall back to regular copy */
}
