#define _GNU_SOURCE         /* See feature_test_macros(7) */
#define GETTEXT_PACKAGE "uniqued"

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


static gboolean
call_make_unique (int *memfd,
                  guint32 *id_out)
{
  GDBusConnection *bus = get_bus ();
  g_autoptr(GVariant) response = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GUnixFDList) response_fd_list = NULL;
  g_autoptr(GVariantIter) handle_iter = NULL;
  g_autoptr(GVariant) handle_v = NULL;
  gint handle;
  guint32 id;

  if (bus == NULL)
    return FALSE;

  fd_list = g_unix_fd_list_new ();
  handle = g_unix_fd_list_append (fd_list, *memfd, NULL);
  if (handle == -1)
    return FALSE;

  response =
    g_dbus_connection_call_with_unix_fd_list_sync (bus,
                                                   "org.freedesktop.portal.Unique",
                                                   "/org/freedesktop/portal/unique",
                                                   "org.freedesktop.portal.Unique",
                                                   "MakeUnique",
                                                   g_variant_new ("(h)", handle),
                                                   G_VARIANT_TYPE ("(ahu)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   3000, /* msec timeout */
                                                   fd_list, &response_fd_list,
                                                   NULL, NULL);

  if (response == NULL)
    return FALSE;

  g_variant_get (response, "(ahu)", &handle_iter, &id);

  handle_v = g_variant_iter_next_value (handle_iter);
  if (handle_v)
    {
      int fd = steal_one_fd_from_list (response_fd_list, g_variant_get_handle (handle_v));
      if (fd == -1)
        return FALSE;

      close (*memfd);
      *memfd = fd;
    }
  else
      g_print ("No new fd!\n");

  *id_out = id;
  return TRUE;
}

typedef struct {
  gpointer data;
  gsize len;
  guint32 id;
} MappedData;

static void
unmap_data (MappedData *d)
{
  munmap (d->data, d->len);
  g_slice_free (MappedData, d);
  call_forget (d->id);
}

static int
create_sealed_memfd_for_data (gconstpointer data, gsize len)
{
  int memfd = -1;

  memfd = memfd_create ("test-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (memfd < 0)
    return -1;

  if (ftruncate (memfd, len) == 0 &&
      write_all_to_fd (memfd, data, len) &&
      fcntl (memfd, F_ADD_SEALS, (int) F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE) == 0)
    return memfd;

  close (memfd);
  return -1;
}

static GBytes *
make_unique_sync (gconstpointer data, gsize len)
{
  int memfd = -1;
  void *memfd_data = NULL;
  guint32 id = 0;

  memfd = create_sealed_memfd_for_data (data, len);
  if (memfd >= 0)
    {
      if (call_make_unique (&memfd, &id))
        memfd_data = mmap (NULL, len, PROT_READ, MAP_PRIVATE, memfd, 0);
    }

  if (memfd >= 0)
    close (memfd);

  if (memfd_data)
    {
      MappedData *d = g_slice_new (MappedData);

      d->data = memfd_data;
      d->len = len;
      d->id = id;

      return g_bytes_new_with_free_func (memfd_data, len, (GDestroyNotify)unmap_data, d);
    }
  else
    {
      g_warning ("Falling back to copy");
      return g_bytes_new (data, len); /* Fall back to regular copy */
    }
}

int main()
{
  char *str = "Hello, World!";
  GBytes *data = make_unique_sync (str, strlen (str) + 1);
  g_print ("Data: %s\n", (char *)g_bytes_get_data (data, NULL));

  sleep (5);
  g_print ("Destroying data\n");
  g_bytes_unref (data);
  sleep (5);
}
