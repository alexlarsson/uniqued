#define _GNU_SOURCE         /* See feature_test_macros(7) */
#define GETTEXT_PACKAGE "uniqued"

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

static gsize real_blob_size;
static gsize apparent_blob_size;

typedef struct {
  char *sha256;
  gsize len;
  int fd;
  int ref_count;
} Blob;

typedef struct {
  char *name;
  guint32 next_blob_id;
  GHashTable *blobs;
} Peer;

static GHashTable *peers;
static GHashTable *blobs;

static inline int
steal_fd (int *fdp)
{
  int fd = *fdp;
  *fdp = -1;
  return fd;
}

static inline void
close_fd (int *fdp)
{
  int fd = steal_fd (fdp);
  if (fd >= 0)
    close (fd);
}

#define auto_fd __attribute__((cleanup(close_fd)))

static void
print_stats (void)
{
  g_autofree gchar *real_size = g_format_size (real_blob_size);
  g_autofree gchar *apparent_size = g_format_size (apparent_blob_size);
  g_debug ("Total apparent memory size: %s, actual size: %s", apparent_size, real_size);
}

static Blob *
blob_ref (Blob *blob)
{
  blob->ref_count++;
  return blob;
}

static void
blob_unref (Blob *blob)
{
  blob->ref_count--;
  if (blob->ref_count == 0)
    {
      g_debug ("Blob for %s destroyed", blob->sha256);

      real_blob_size -= blob->len;
      g_hash_table_remove (blobs, blob->sha256);

      close (blob->fd);
      g_free (blob->sha256);
      g_free (blob);
    }
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Blob, blob_unref)

static Blob *
blob_new (int fd,
          const char *sha256)
{
  struct stat statbuf;

  Blob *blob = g_new0 (Blob, 1);

  blob->sha256 = g_strdup (sha256);
  blob->fd = fd;
  blob->ref_count = 1;

  if (fstat (fd, &statbuf) == 0)
    blob->len = statbuf.st_size;
  real_blob_size += blob->len;

  g_hash_table_insert (blobs, blob->sha256, blob);

  return blob;
}

static Blob *
lookup_blob (const char *sha256)
{
  Blob *blob = g_hash_table_lookup (blobs, sha256);

  if (blob)
    return blob_ref (blob);

  return NULL;
}

static void
removed_blob_from_peer_cb (Blob *blob)
{
  apparent_blob_size -= blob->len;
  blob_unref (blob);
}

/* return value owned by peers table, only destroyed when peer dies */
static Peer *
lookup_peer (const char *name)
{
  Peer *peer = g_hash_table_lookup (peers, name);

  if (peer == NULL)
    {
      peer = g_new0 (Peer, 1);
      peer->name = g_strdup (name);
      peer->next_blob_id = 1;
      peer->blobs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)removed_blob_from_peer_cb);
      g_hash_table_insert (peers, peer->name, peer);
    }

  return peer;
}

static void
peer_free (Peer *peer)
{
  g_hash_table_destroy (peer->blobs);
  g_free (peer->name);
  g_free (peer);
}

static guint32
add_blob_to_peer (const char *peer_name, Blob *blob)
{
  Peer *peer = lookup_peer (peer_name);
  guint32 blob_id = peer->next_blob_id++;

  apparent_blob_size += blob->len;
  g_hash_table_insert (peer->blobs, GUINT_TO_POINTER(blob_id), blob_ref (blob));

  g_debug ("Added blob %d (with sha256 %s) for peer %s", blob_id, blob->sha256, peer_name);

  return blob_id;
}

static void
remove_blob_from_peer (const char *peer_name, guint32 blob_id)
{
  Peer *peer = lookup_peer (peer_name);

  g_debug ("Removing blob %d for peer %s", blob_id, peer_name);

  g_hash_table_remove (peer->blobs, GUINT_TO_POINTER(blob_id));
}

static GDBusInterfaceInfo *
get_interface (void)
{
  static GDBusInterfaceInfo *interface_info;

  if (interface_info == NULL)
    {
      GError *error = NULL;
      GDBusNodeInfo *info;

      info = g_dbus_node_info_new_for_xml ("<node>"
                                           "  <interface name='org.freedesktop.portal.Unique'>"
                                           "    <method name='MakeUnique'>"
                                           "      <arg type='h' name='memfd' direction='in'/>"
                                           "      <arg type='ah' name='content' direction='out'/>"
                                           "      <arg type='u' name='handle' direction='out'/>"
                                           "    </method>"
                                           "    <method name='Forget'>"
                                           "      <arg type='u' name='handle' direction='in'/>"
                                           "    </method>"
                                           "  </interface>"
                                           "</node>", &error);
      if (info == NULL)
        g_error ("%s", error->message);
      interface_info = g_dbus_node_info_lookup_interface (info, "org.freedesktop.portal.Unique");
      g_assert (interface_info != NULL);
      g_dbus_interface_info_ref (interface_info);
      g_dbus_node_info_unref (info);
    }

  return interface_info;
}

#define ALL_SEALS (F_SEAL_SEAL | \
                   F_SEAL_SHRINK | \
                   F_SEAL_GROW |   \
                   F_SEAL_WRITE)

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

static gboolean
checksum_from_fd (GChecksum *checksum,
                  int fd)
{
  guchar buf[64*1024];
  ssize_t res;
  off_t offset = 0;

  do
    {
      res = pread (fd, buf, sizeof (buf), offset);
      if (res == -1)
        return FALSE;

      if (res > 0)
        {
          g_checksum_update (checksum, buf, res);
          offset += res;
        }
    }
  while (res > 0);

  return TRUE;
}

static void
make_unique (GDBusConnection       *connection,
             const gchar           *sender,
             GVariant              *parameters,
             GDBusMethodInvocation *invocation)
{
  GDBusMessage *message = g_dbus_method_invocation_get_message (invocation);
  GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list (message);
  g_autoptr(GUnixFDList) ret_fds = NULL;
  gint32 handle;
  auto_fd int fd = -1;
  unsigned int seals;
  g_autoptr(GChecksum) checksum = NULL;
  g_autoptr(GVariantBuilder) array_builder = NULL;
  const gchar *sha256;
  g_autoptr(Blob) blob = NULL;
  guint32 blob_id;

  g_debug ("Got MakeUnique request from %s", sender);

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(h)")))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS, "Wrong argument types");
      return;
    }

  g_variant_get (parameters, "(h)", &handle);

  fd = steal_one_fd_from_list (fd_list, handle);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS, "No fd passed");
      return;
    }

  seals = fcntl (fd, F_GET_SEALS);
  if (seals == -1 ||  (seals & ALL_SEALS) != ALL_SEALS)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS, "Fd not sealed");
      return;
    }

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (!checksum_from_fd (checksum, fd))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS, "Can't read data");
      return;
    }

  sha256 = g_checksum_get_string (checksum);

  array_builder = g_variant_builder_new (G_VARIANT_TYPE ("ah"));

  ret_fds = g_unix_fd_list_new ();

  blob = lookup_blob (sha256);
  if (blob == NULL)
    {
      blob = blob_new (steal_fd (&fd), sha256);
      g_debug ("Created new blob for %s (size %ld)", sha256, blob->len);
    }
  else
    {
      gint fd_handle = g_unix_fd_list_append (ret_fds, blob->fd, NULL);
      if (fd_handle < 0)
        {
          g_dbus_method_invocation_return_error (invocation,  G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED, "Failed to dup fd");
          return;
        }

      g_debug ("Reusing old blob for %s", sha256);
      g_variant_builder_add (array_builder, "h", fd_handle);
    }

  blob_id = add_blob_to_peer (sender, blob);

  print_stats ();

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                           g_variant_new ("(ahu)", array_builder, blob_id),
                                                           ret_fds);
}

static void
forget (GDBusConnection       *connection,
        const gchar           *sender,
        GVariant              *parameters,
        GDBusMethodInvocation *invocation)
{
  guint32 blob_id;

  g_debug ("Got Forget request from %s", sender);

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(u)")))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS, "Wrong argument types");
      return;
    }

  g_variant_get (parameters, "(u)", &blob_id);

  remove_blob_from_peer (sender, blob_id);

  print_stats ();

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
method_call (GDBusConnection       *connection,
             const gchar           *sender,
             const gchar           *object_path,
             const gchar           *interface_name,
             const gchar           *method_name,
             GVariant              *parameters,
             GDBusMethodInvocation *invocation,
             gpointer               user_data)
{
  if (g_str_equal (method_name, "MakeUnique"))
    make_unique (connection,sender, parameters, invocation);
  else if (g_str_equal (method_name, "Forget"))
    forget (connection,sender, parameters, invocation);
  else
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "Method %s is not implemented on interface %s", method_name, interface_name);
}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      if (g_hash_table_remove (peers, name))
        {
          g_debug ("Peer %s died", name);
          print_stats ();
        }
    }
}

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  const GDBusInterfaceVTable vtable = {
    method_call,
  };

  g_dbus_connection_signal_subscribe (connection,
                                      DBUS_NAME_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "NameOwnerChanged",
                                      DBUS_PATH_DBUS,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  g_dbus_connection_register_object (connection, "/org/freedesktop/portal/unique", get_interface (),
                                     &vtable, NULL, NULL, NULL);

}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  exit (1);
}


static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
main (int argc, char **argv)
{
  guint owner_id;
  GMainLoop *loop;
  gboolean replace;
  gboolean verbose;
  GOptionContext *context;
  GDBusConnection *session_bus;
  GBusNameOwnerFlags flags;
  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output.", NULL },
    { NULL }
  };

  g_set_prgname (argv[0]);

  context = g_option_context_new ("");

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  replace = FALSE;
  verbose = FALSE;

  g_option_context_set_summary (context, "Uniqued");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      g_option_context_free (context);
      return 1;
    }

  if (verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("Can't find bus: %s\n", error->message);
      return 1;
    }

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.portal.Unique",
                             flags,
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  blobs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL); // No destroy, instead blob destry removes from hash
  peers = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)peer_free);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);
}
