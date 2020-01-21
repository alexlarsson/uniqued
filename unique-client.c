#define GETTEXT_PACKAGE "uniqued"

#include <glib.h>
#include "unique-bytes.h"


static gboolean
free_data (gpointer user_data)
{
  GBytes *data = user_data;

  g_print ("Destroying data %p\n", data);
  g_bytes_unref (data);

  return G_SOURCE_REMOVE;
}

static gboolean
print_data (gpointer user_data)
{
  GBytes *data = user_data;

  g_print ("data after timeout: %p %s\n", data, (char *)g_bytes_get_data (data, NULL));

  return G_SOURCE_REMOVE;
}

static gboolean
do_exit (gpointer user_data)
{
  exit (0);
  return G_SOURCE_REMOVE;
}

int
main (int argc,
      char *argv[])
{
  GMainLoop *loop;
  char *str = "Hello, World!";
  GBytes *data1, *data2, *data3;

  data1 = g_bytes_new_unique_sync (str, strlen (str) + 1);
  g_print ("data1: %p %s\n", data1, (char *)g_bytes_get_data (data1, NULL));

  data2 = g_bytes_new_unique_sync (str, strlen (str) + 1);
  g_print ("data2: %p %s\n", data2, (char *)g_bytes_get_data (data2, NULL));

  data3 = g_bytes_new_unique_async (str, strlen (str) + 1);
  g_print ("data3: %p %s\n", data3, (char *)g_bytes_get_data (data3, NULL));

  loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add (1000, free_data, data1);
  g_timeout_add (2000, free_data, data2);
  g_timeout_add (3000, print_data, data3);
  g_timeout_add (4000, free_data, data3);
  g_timeout_add (5000, do_exit, NULL);
  g_print ("Running mainloop\n");
  g_main_loop_run (loop);


}
