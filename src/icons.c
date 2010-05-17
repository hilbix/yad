
#include "yad.h"

static GtkWidget *icon_view;

enum {
  COL_NAME,
  COL_TOOLTIP,
  COL_PIXBUF,
  COL_COMMAND,
  COL_TERM,
  NUM_COLS
};

typedef struct {
  gchar *name;
  gchar *comment;
  GdkPixbuf *pixbuf;
  gchar *command;
  gboolean in_term;
} DEntry;

static void
activate_cb (GtkIconView *view, GtkTreePath *path, gpointer data)
{
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_icon_view_get_model (view);
  gchar *cmd;
  gboolean *in_term;

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter,
		      COL_COMMAND, &cmd,
		      COL_TERM, &in_term,
		      -1);

  if (in_term)
    {
      gchar *tcmd;
      
      tcmd = g_strdup_printf (options.icons_data.term, cmd);
      g_spawn_command_line_async (tcmd, NULL);
      g_free (tcmd);
    }
  else
    g_spawn_command_line_async (cmd, NULL);
}

static gboolean
handle_stdin (GIOChannel * channel,
              GIOCondition condition, gpointer data)
{
  static GtkTreeIter iter;
  static gint column_count = 0;
  static gint row_count = 0;
  static gboolean first_time = TRUE;
  GtkTreeModel *model = gtk_icon_view_get_model (GTK_ICON_VIEW (icon_view));
                                                            
  if (first_time)                                                 
    {                                                       
      first_time = FALSE;                                         
      gtk_list_store_append (GTK_LIST_STORE (model), &iter);
    }                                                             
                                          
  if ((condition == G_IO_IN) || (condition == G_IO_IN + G_IO_HUP))
    {                                     
      GError *err = NULL;                 
      GString *string = g_string_new (NULL);       

      while (channel->is_readable != TRUE) ;

      do              
        {                                                                   
	  GdkPixbuf *pb;
          gint status;                
                                                                            
          do                          
            {                                                               
              status =                
                g_io_channel_read_line_string (channel, string, NULL, &err);

              while (gtk_events_pending ())   
                gtk_main_iteration ();
            }                               
          while (status == G_IO_STATUS_AGAIN);              
	  strip_new_line (string->str);
                                            
          if (status != G_IO_STATUS_NORMAL)                 
            {                               
              if (err)                                      
                {                           
                  g_printerr ("yad_icons_handle_stdin(): %s", err->message);
                  g_error_free (err);
		  err = NULL;
                }                           
              continue;                                             
            }

          if (column_count == NUM_COLS)                                    
            {                               
              /* We're starting a new row */                                
              column_count = 0;             
              row_count++;                                                  
              gtk_list_store_append (GTK_LIST_STORE (model), &iter);
            }                                                               
	  
	  switch (column_count)
	    {
	    case COL_NAME:
	    case COL_TOOLTIP:
	    case COL_COMMAND:
	      gtk_list_store_set (GTK_LIST_STORE (model), &iter, column_count, string->str, -1);
	      break;
	    case COL_PIXBUF:
	      pb = get_pixbuf (string->str);
	      gtk_list_store_set (GTK_LIST_STORE (model), &iter, column_count, pb, -1);
	      g_object_unref (pb);
	      break;
	    case COL_TERM:
	      if (g_ascii_strcasecmp (string->str, "true") == 0)
		gtk_list_store_set (GTK_LIST_STORE (model), &iter, column_count, TRUE, -1);
	      else
		gtk_list_store_set (GTK_LIST_STORE (model), &iter, column_count, FALSE, -1);
	      break;
	    }

	  column_count++;
        }                                                                           
      while (g_io_channel_get_buffer_condition (channel) == G_IO_IN);                     
      g_string_free (string, TRUE);                                                 
    }                                                                                     

  if (condition != G_IO_IN)                                                               
    {                                                                               
      g_io_channel_shutdown (channel, TRUE, NULL);                                        
      return FALSE;                                                                 
    }                                                                                     

  return TRUE;                                                                      
}

static DEntry *
parse_desktop_file (gchar *filename)
{
  DEntry *ent;
  GKeyFile *kf;
  GError *err = NULL;
  static GdkPixbuf *fb = NULL;

  if (!fb)
    fb = gtk_icon_theme_load_icon (settings.icon_theme, "unknown",
				   48, GTK_ICON_LOOKUP_GENERIC_FALLBACK,
				   NULL);

  ent = g_new0 (DEntry, 1);
  kf = g_key_file_new ();

  if (g_key_file_load_from_file (kf, filename, 0, &err))
    {
      gchar *icon;

      if (g_key_file_has_group (kf, "Desktop Entry"))
	{
	  gint i;

	  ent->name = g_key_file_get_locale_string (kf, "Desktop Entry", "Name", NULL, NULL);
	  ent->comment = g_key_file_get_locale_string (kf, "Desktop Entry", "Comment", NULL, NULL);
	  ent->command = g_key_file_get_locale_string (kf, "Desktop Entry", "Exec", NULL, NULL);
	  icon = g_key_file_get_string (kf, "Desktop Entry", "Icon", NULL);
	  if (icon)
	    {
	      ent->pixbuf = gtk_icon_theme_load_icon (settings.icon_theme, icon, 48, 
						      GTK_ICON_LOOKUP_GENERIC_FALLBACK, NULL);
	      if (!ent->pixbuf)
		ent->pixbuf = fb;
	      g_free (icon);
	    }
	}
    }
  else
    g_printerr (_("Unable to parse file %s: %s"), filename, err->message);
  
  g_key_file_free (kf);

  return ent;
}

static void
read_dir (GtkListStore *store)
{
  GDir *dir;
  const gchar *filename;
  GError *err = NULL;

  dir = g_dir_open (options.icons_data.directory, 0, &err);
  if (!dir)
    {
      g_printerr (_("Unable to open directory %s: %s"), 
		  options.icons_data.directory, err->message);
      return;
    }

  while ((filename = g_dir_read_name (dir)) != NULL)
    {
      DEntry *ent;
      GtkTreeIter iter;
      gchar *fullname;

      if (!g_str_has_suffix (filename, ".desktop"))
	continue;

      fullname = g_build_filename (options.icons_data.directory, filename, NULL);
      ent = parse_desktop_file (fullname);
      g_free (fullname);

      if (ent->name)
	{
	  gtk_list_store_append (store, &iter);
	  gtk_list_store_set (store, &iter,
			      COL_NAME, ent->name,
			      COL_TOOLTIP, ent->comment ? ent->comment : "",
			      COL_PIXBUF, ent->pixbuf,
			      COL_COMMAND, ent->command ? ent->command : "",
			      COL_TERM, ent->in_term,
			      -1);
	}

      /* free desktop entry */
      g_free (ent->name);
      g_free (ent->comment);
      g_free (ent->command);
      if (ent->pixbuf)
	g_object_unref (ent->pixbuf);
      g_free (ent);
    }

  g_dir_close (dir);
}

GtkWidget *
icons_create_widget (GtkWidget *dlg)
{
  GtkWidget *w;
  GtkListStore *store;

  w = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (w),
				  GTK_POLICY_AUTOMATIC,
				  GTK_POLICY_AUTOMATIC);

  store = gtk_list_store_new (NUM_COLS,
			      G_TYPE_STRING,
			      G_TYPE_STRING,
			      GDK_TYPE_PIXBUF,
			      G_TYPE_STRING,
			      G_TYPE_BOOLEAN);

  icon_view = gtk_icon_view_new_with_model (GTK_TREE_MODEL (store));
  gtk_icon_view_set_text_column (GTK_ICON_VIEW (icon_view), COL_NAME);
  gtk_icon_view_set_pixbuf_column (GTK_ICON_VIEW (icon_view), COL_PIXBUF);
  gtk_icon_view_set_tooltip_column (GTK_ICON_VIEW (icon_view), COL_TOOLTIP);

  /* handle directory */
  if (options.icons_data.directory)
    read_dir (store);

  g_object_unref (store);

  /* read from stdin */
  if (options.icons_data.stdin)
    {
      GIOChannel *channel;

      channel = g_io_channel_unix_new (0);
      g_io_channel_set_encoding (channel, NULL, NULL);
      g_io_channel_set_flags (channel, G_IO_FLAG_NONBLOCK, NULL);
      g_io_add_watch (channel, G_IO_IN | G_IO_HUP, handle_stdin, NULL);
    }

  if (options.icons_data.dnd)
    {
      printf ("dnd ok\n");
    }

  g_signal_connect (G_OBJECT (icon_view), "item-activated",
		    G_CALLBACK (activate_cb), NULL);
  gtk_container_add (GTK_CONTAINER (w), icon_view);
  
  return w;
}
