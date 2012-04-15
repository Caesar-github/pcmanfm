/*
* Author: Hong Jen Yee (PCMan) <pcman.tw (AT) gmail.com>, (C) 2006
*
* Copyright: See COPYING file that comes with this distribution
*
*/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

/* turn on to debug GDK_THREADS_ENTER/GDK_THREADS_LEAVE related deadlocks */
#undef _DEBUG_THREAD

#include "private.h"

#include <gtk/gtk.h>
#include <glib.h>

#include <stdlib.h>
#include <string.h>

/* socket is used to keep single instance */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <signal.h>

#include "main-window.h"

#include "vfs-file-info.h"
#include "vfs-mime-type.h"
#include "vfs-app-desktop.h"

#include "vfs-file-monitor.h"
#include "vfs-volume.h"
#include "vfs-thumbnail-loader.h"

#include "ptk-utils.h"
#include "ptk-app-chooser.h"

#include "settings.h"

#include "desktop.h"

typedef enum{
    CMD_OPEN = 1,
    CMD_OPEN_TAB,
    CMD_FILE_PROP,
    CMD_FILE_MENU,
    CMD_DAEMON_MODE
}SocketEvent;

static gboolean initialized = FALSE;

static int sock;
GIOChannel* io_channel = NULL;

gboolean daemon_mode = FALSE;

static char* default_files[2] = {NULL, NULL};
static char** files = NULL;
static gboolean no_desktop = FALSE;
static gboolean old_show_desktop = FALSE;

static gboolean new_tab = FALSE;
static gboolean file_prop = FALSE;
static gboolean file_menu = FALSE;

/* for FUSE support, especially sshfs */
static char* ask_pass = NULL;

#ifdef HAVE_HAL
static char* mount = NULL;
static char* umount = NULL;
static char* eject = NULL;
#endif

static GOptionEntry opt_entries[] =
{
    { "no-desktop", '\0', 0, G_OPTION_ARG_NONE, &no_desktop, N_("Don't show desktop icons."), NULL },
    { "daemon-mode", 'd', 0, G_OPTION_ARG_NONE, &daemon_mode, N_("Run PCManFM as a daemon"), NULL },
    { "new-tab", 't', 0, G_OPTION_ARG_NONE, &new_tab, N_("Open folders in new tabs of the last used window instead of creating new windows"), NULL },
/*    { "file-prop", 'p', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &file_prop, NULL, NULL },
    { "file-menu", 'n', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &file_menu, NULL, NULL }, */
#ifdef HAVE_HAL
    /* hidden arguments used to mount volumes */
    { "mount", 'm', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &mount, NULL, NULL },
    { "umount", 'u', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &umount, NULL, NULL },
    { "eject", 'e', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &eject, NULL, NULL },
#endif
    { "ask-pass", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &ask_pass, NULL, NULL },
    {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &files, NULL, N_("[FILE1, FILE2,...]")},
    { NULL }
};

static gboolean single_instance_check();
static void single_instance_finalize();
static void get_socket_name( char* buf, int len );
static gboolean on_socket_event( GIOChannel* ioc, GIOCondition cond, gpointer data );

static void init_folder();
static void check_icon_theme();

static gboolean handle_parsed_commandline_args();

static FMMainWindow* create_main_window();
static void open_file( const char* path );

static GList* get_file_info_list( char** files );

gboolean on_socket_event( GIOChannel* ioc, GIOCondition cond, gpointer data )
{
    int client, r;
    socklen_t addr_len = 0;
    struct sockaddr_un client_addr ={ 0 };
    static char buf[ 1024 ];
    GString* args;
    char** file;
    SocketEvent cmd;

    if ( cond & G_IO_IN )
    {
        client = accept( g_io_channel_unix_get_fd( ioc ), (struct sockaddr *)&client_addr, &addr_len );
        if ( client != -1 )
        {
            args = g_string_new_len( NULL, 2048 );
            while( (r = read( client, buf, sizeof(buf) )) > 0 )
                g_string_append_len( args, buf, r);
            shutdown( client, 2 );
            close( client );

            new_tab = FALSE;
            file_prop = FALSE;
            file_menu = FALSE;

            switch( args->str[0] )
            {
            case CMD_OPEN_TAB:
                new_tab = TRUE;
                break;
            case CMD_FILE_PROP:
                file_prop = TRUE;
                break;
            case CMD_FILE_MENU:
                file_menu = TRUE;
                break;
            case CMD_DAEMON_MODE:
                daemon_mode = TRUE;
                g_string_free( args, TRUE );
                return TRUE;
            }

            if( args->str[ 1 ] )
                files = g_strsplit( args->str + 1, "\n", 0 );
            else
                files = NULL;
            g_string_free( args, TRUE );

            GDK_THREADS_ENTER();

            if( files )
            {
                for( file = files; *file; ++file )
                {
                    if( ! **file )  /* remove empty string at tail */
                        *file = NULL;
                }
            }
            handle_parsed_commandline_args();

            GDK_THREADS_LEAVE();
        }
    }

    return TRUE;
}

void get_socket_name( char* buf, int len )
{
    char* dpy = gdk_get_display();
    g_snprintf( buf, len, "/tmp/.pcmanfm-socket%s-%s", dpy, g_get_user_name() );
    g_free( dpy );
}

gboolean single_instance_check()
{
    struct sockaddr_un addr;
    int addr_len;
    int ret;
    int reuse;

    if ( ( sock = socket( AF_UNIX, SOCK_STREAM, 0 ) ) == -1 )
    {
        ret = 1;
        goto _exit;
    }

    addr.sun_family = AF_UNIX;
    get_socket_name( addr.sun_path, sizeof( addr.sun_path ) );
#ifdef SUN_LEN
    addr_len = SUN_LEN( &addr );
#else

    addr_len = strlen( addr.sun_path ) + sizeof( addr.sun_family );
#endif

    /* try to connect to existing instance */
    if ( connect( sock, ( struct sockaddr* ) & addr, addr_len ) == 0 )
    {
        /* connected successfully */
        char** file;
        char cmd = CMD_OPEN;

        if( daemon_mode )
            cmd = CMD_DAEMON_MODE;
        else if( new_tab )
            cmd = CMD_OPEN_TAB;
        else if( file_prop )
            cmd = CMD_FILE_PROP;
        else if( file_menu )
            cmd = CMD_FILE_MENU;

        write( sock, &cmd, sizeof(char) );

        if( files )
        {
            for( file = files; *file; ++file )
            {
                write( sock, *file, strlen( *file ) );
                write( sock, "\n", 1 );
            }
        }

        shutdown( sock, 2 );
        close( sock );
        ret = 0;
        goto _exit;
    }

    /* There is no existing server, and we are in the first instance. */
    unlink( addr.sun_path ); /* delete old socket file if it exists. */
    reuse = 1;
    ret = setsockopt( sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    if ( bind( sock, ( struct sockaddr* ) & addr, addr_len ) == -1 )
    {
        ret = 1;
        goto _exit;
    }

    io_channel = g_io_channel_unix_new( sock );
    g_io_channel_set_encoding( io_channel, NULL, NULL );
    g_io_channel_set_buffered( io_channel, FALSE );

    g_io_add_watch( io_channel, G_IO_IN,
                    ( GIOFunc ) on_socket_event, NULL );

    if ( listen( sock, 5 ) == -1 )
    {
        ret = 1;
        goto _exit;
    }
    return ;

_exit:

    gdk_notify_startup_complete();
    exit( ret );
}

void single_instance_finalize()
{
    char lock_file[ 256 ];

    shutdown( sock, 2 );
    g_io_channel_unref( io_channel );
    close( sock );

    get_socket_name( lock_file, sizeof( lock_file ) );
    unlink( lock_file );
}

FMMainWindow* create_main_window()
{
    FMMainWindow * main_window = FM_MAIN_WINDOW(fm_main_window_new ());
    gtk_window_set_default_size( GTK_WINDOW( main_window ),
                                 app_settings.width, app_settings.height );
    if ( app_settings.maximized )
    {
        gtk_window_maximize( GTK_WINDOW( main_window ) );
    }
    gtk_widget_show ( GTK_WIDGET( main_window ) );
    return main_window;
}

void check_icon_theme()
{
    GtkSettings * settings;
    char* theme;
    const char* title = N_( "GTK+ icon theme is not properly set" );
    const char* error_msg =
        N_( "<big><b>%s</b></big>\n\n"
            "This usually means you don't have an XSETTINGS manager running.  "
            "Desktop environment like GNOME or XFCE automatically execute their "
            "XSETTING managers like gnome-settings-daemon or xfce-mcs-manager.\n\n"
            "<b>If you don't use these desktop environments, "
            "you have two choices:\n"
            "1. run an XSETTINGS manager, or\n"
            "2. simply specify an icon theme in ~/.gtkrc-2.0.</b>\n"
            "For example to use the Tango icon theme add a line:\n"
            "<i><b>gtk-icon-theme-name=\"Tango\"</b></i> in your ~/.gtkrc-2.0. (create it if no such file)\n\n"
            "<b>NOTICE: The icon theme you choose should be compatible with GNOME, "
            "or the file icons cannot be displayed correctly.</b>  "
            "Due to the differences in icon naming of GNOME and KDE, KDE themes cannot be used.  "
            "Currently there is no standard for this, but it will be solved by freedesktop.org in the future." );
    settings = gtk_settings_get_default();
    g_object_get( settings, "gtk-icon-theme-name", &theme, NULL );

    /* No icon theme available */
    if ( !theme || !*theme || 0 == strcmp( theme, "hicolor" ) )
    {
        GtkWidget * dlg;
        dlg = gtk_message_dialog_new_with_markup( NULL,
                                                  GTK_DIALOG_MODAL,
                                                  GTK_MESSAGE_ERROR,
                                                  GTK_BUTTONS_OK,
                                                  _( error_msg ), _( title ) );
        gtk_window_set_title( GTK_WINDOW( dlg ), _( title ) );
        gtk_dialog_run( GTK_DIALOG( dlg ) );
        gtk_widget_destroy( dlg );
    }
    g_free( theme );
}

#ifdef _DEBUG_THREAD

G_LOCK_DEFINE(gdk_lock);
void debug_gdk_threads_enter (const char* message)
{
    g_debug( "Thread %p tries to get GDK lock: %s", g_thread_self (), message );
    G_LOCK(gdk_lock);
    g_debug( "Thread %p got GDK lock: %s", g_thread_self (), message );
}

static void _debug_gdk_threads_enter ()
{
    debug_gdk_threads_enter( "called from GTK+ internal" );
}

void debug_gdk_threads_leave( const char* message )
{
    g_debug( "Thread %p tries to release GDK lock: %s", g_thread_self (), message );
    G_LOCK(gdk_lock);
    g_debug( "Thread %p released GDK lock: %s", g_thread_self (), message );
}

static void _debug_gdk_threads_leave()
{
    debug_gdk_threads_leave( "called from GTK+ internal" );
}
#endif

void init_folder()
{
    if( G_LIKELY(initialized) )
        return;

    app_settings.bookmarks = ptk_bookmarks_get();

    vfs_volume_init();
    vfs_thumbnail_init();

    vfs_mime_type_set_icon_size( app_settings.big_icon_size,
                                 app_settings.small_icon_size );
    vfs_file_info_set_thumbnail_size( app_settings.big_icon_size,
                                      app_settings.small_icon_size );

    check_icon_theme();
    initialized = TRUE;
}

#ifdef HAVE_HAL

/* FIXME: Currently, this cannot be supported without HAL */

static int handle_mount( char** argv )
{
    gboolean success;
    vfs_volume_init();
    if( mount )
        success = vfs_volume_mount_by_udi( mount, NULL );
    else if( umount )
        success = vfs_volume_umount_by_udi( umount, NULL );
    else /* if( eject ) */
        success = vfs_volume_eject_by_udi( eject, NULL );
    vfs_volume_finalize();
    return success ? 0 : 1;
}
#endif

GList* get_file_info_list( char** file_paths )
{
    GList* file_list = NULL;
    char** file;
    VFSFileInfo* fi;

    for( file = file_paths; *file; ++file )
    {
        fi = vfs_file_info_new();
        if( vfs_file_info_get( fi, *file, NULL ) )
            file_list = g_list_append( file_list, fi );
        else
            vfs_file_info_unref( fi );
    }

    return file_list;
}

gboolean delayed_popup( GtkWidget* popup )
{
    GDK_THREADS_ENTER();

    gtk_menu_popup( GTK_MENU( popup ), NULL, NULL,
                    NULL, NULL, 0, gtk_get_current_event_time() );

    GDK_THREADS_LEAVE();

    return FALSE;
}

gboolean handle_parsed_commandline_args()
{
    FMMainWindow * main_window = NULL;
    char** file;

    /* If no files are specified, open home dir by defualt. */
    if( G_LIKELY( ! files ) )
    {
        files = default_files;
        files[0] = (char *) g_get_home_dir();
    }

    /* get the last active window, if available */
    if( new_tab )
    {
        main_window = fm_main_window_get_last_active();
    }
    else if( file_menu )    /* show popup menu for files */
    {
        /* FIXME: This doesn't work properly */
        GtkMenu* popup;
        GList* file_list;

        if( ! daemon_mode )
        {
            g_warning( "--file-menu is only availble when pcmanfm daemon is running." );
            return FALSE;
        }

        file_list = get_file_info_list( files );

        if( file_list )
        {
            char* dir_name = g_path_get_dirname( files[0] );
            popup = ptk_file_menu_new(
                        files[0], (VFSFileInfo*)file_list->data,
                        dir_name,
                        file_list, NULL );
            /* FIXME: I have no idea why this crap is needed.
             *  Without this delay, the menu will fail to popup. */
            g_timeout_add( 150, (GSourceFunc)delayed_popup, popup );
        }

        if( files != default_files )
            g_free( files );
        files = NULL;
        return TRUE;
    }
    else if( file_prop )    /* show file properties dialog */
    {
        GList* file_list;

        file_list = get_file_info_list( files );
        if( file_list )
        {
            GtkWidget* dlg;
            char* dir_name = g_path_get_dirname( files[0] );
            dlg = file_properties_dlg_new( NULL, dir_name, file_list );
            gtk_dialog_run( dlg );
            gtk_widget_destroy( dlg );
            g_free( dir_name );

            if( files != default_files )
                g_free( files );
            return FALSE;
        }
    }

    /* open files passed in command line arguments */
    for( file = files; *file; ++file )
    {
        char* file_path, *real_path;

        if( ! **file )  /* skip empty string */
            continue;

        if( g_str_has_prefix( *file, "file:" ) ) /* It's a URI */
        {
            file_path = g_filename_from_uri( *file, NULL, NULL );
            g_free( *file );
            *file = file_path;
        }
        else
            file_path = *file;

        real_path = vfs_file_resolve_path( NULL, file_path );
        if( g_file_test( real_path, G_FILE_TEST_IS_DIR ) )
        {
            if( G_UNLIKELY( ! main_window ) )   /* create main window if needed */
            {
                /* initialize things required by folder view... */
                if( G_UNLIKELY( ! daemon_mode ) )
                    init_folder();
                main_window = create_main_window();
            }
            fm_main_window_add_new_tab( main_window, real_path,
                                        app_settings.show_side_pane,
                                        app_settings.side_pane_mode );
        }
        else
        {
            open_file( real_path );
        }
        g_free( real_path );
    }

    if( files != default_files )
        g_strfreev( files );

    files = NULL;

    return TRUE;
}

static int ask_for_password()
{
    int ret = 1;
    GtkWidget* dlg = gtk_dialog_new_with_buttons( _("Authantication"), NULL,
                                GTK_DIALOG_MODAL,
                                GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                GTK_STOCK_OK, GTK_RESPONSE_OK, NULL );
    GtkWidget* hbox = gtk_hbox_new( FALSE, 4 );
    GtkWidget* img = gtk_image_new_from_stock( GTK_STOCK_DIALOG_AUTHENTICATION, GTK_ICON_SIZE_DIALOG );
    GtkWidget* label = gtk_label_new(_(ask_pass));
    GtkWidget* entry = gtk_entry_new();
#if 0
    const char* default_prompt = _("Password: ");
#endif

    gtk_entry_set_visibility( (GtkEntry*)entry, FALSE );
    gtk_entry_set_activates_default( entry, TRUE );
    gtk_dialog_set_default_response( (GtkDialog*)dlg, GTK_RESPONSE_OK );

    gtk_box_pack_start( (GtkBox*)hbox, img, FALSE, TRUE, 2 );
    gtk_box_pack_start( (GtkBox*)hbox, label, FALSE, TRUE, 2 );
    gtk_box_pack_start( (GtkBox*)hbox, entry, TRUE, TRUE, 2 );

    gtk_box_pack_start( (GtkBox*)((GtkDialog*)dlg)->vbox, hbox, TRUE, TRUE, 2 );
    gtk_widget_show_all( dlg );
    if( gtk_dialog_run( (GtkDialog*)dlg ) == GTK_RESPONSE_OK )
    {
        printf( gtk_entry_get_text(entry) );
        ret = 0;
    }
    gtk_widget_destroy( dlg );
    return ret;
}

int main ( int argc, char *argv[] )
{
    gboolean run = FALSE;
    GError* err = NULL;

#ifdef ENABLE_NLS
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    /* Initialize multithreading
         No matter we use threads or not, it's safer to initialize this earlier. */
#ifdef _DEBUG_THREAD
    gdk_threads_set_lock_functions(_debug_gdk_threads_enter, _debug_gdk_threads_leave);
#endif
    g_thread_init( NULL );
    gdk_threads_init ();

    /* initialize GTK+ and parse the command line arguments */
    if( G_UNLIKELY( ! gtk_init_with_args( &argc, &argv, "", opt_entries, GETTEXT_PACKAGE, &err ) ) )
        return 1;

#if HAVE_HAL
    /* If the user wants to mount/umount/eject a device */
    if( G_UNLIKELY( mount || umount || eject ) )
        return handle_mount( argv );
#endif

    /* ask the user for password, used by sshfs */
    if( G_UNLIKELY(ask_pass) )
        return ask_for_password();

    /* ensure that there is only one instance of pcmanfm.
         if there is an existing instance, command line arguments
         will be passed to the existing instance, and exit() will be called here.  */
    single_instance_check( argc - 1, argv + 1 );

    load_settings();    /* load config file */

    /* initialize the file alteration monitor */
    if( G_UNLIKELY( ! vfs_file_monitor_init() ) )
    {
        ptk_show_error( NULL, _("Error"), _("Error: Unable to establish connection with FAM.\n\nDo you have \"FAM\" or \"Gamin\" installed and running?") );
        vfs_file_monitor_clean();
        free_settings();
        return 1;
    }

    /* check if the filename encoding is UTF-8 */
    vfs_file_info_set_utf8_filename( g_get_filename_charsets( NULL ) );

    /* Initialize our mime-type system */
    vfs_mime_type_init();

    /* temporarily turn off desktop if needed */
    if( G_LIKELY( no_desktop ) )
    {
        /* No matter what the value of show_desktop is, we don't showdesktop icons
         * if --no-desktop argument is passed by the users. */
        old_show_desktop = app_settings.show_desktop;
        /* This config value will be restored before saving config files, if needed. */
        app_settings.show_desktop = FALSE;
    }

    /* handle the parsed result of command line args */
    if( daemon_mode || app_settings.show_desktop )
    {
        init_folder();
        run = TRUE; /* we always need to run the main loop for daemon mode */

        /* FIXME: are these necessary?? */
        signal( SIGPIPE, SIG_IGN );
        signal( SIGHUP, gtk_main_quit );
        signal( SIGINT, gtk_main_quit );
        signal( SIGTERM, gtk_main_quit );
    }
    else
        run = handle_parsed_commandline_args();

    if( app_settings.show_desktop )
    {
        fm_turn_on_desktop_icons();
    }

    if( run )   /* run the main loop */
        gtk_main();

    single_instance_finalize();

    if( app_settings.show_desktop )
        fm_turn_off_desktop_icons();

    if( no_desktop )    /* desktop icons is temporarily supressed */
    {
        if( old_show_desktop )  /* restore original settings */
        {
            old_show_desktop = app_settings.show_desktop;
            app_settings.show_desktop = TRUE;
        }
    }

    if( run )
        save_settings();    /* write config file */

    free_settings();

    vfs_volume_finalize();
    vfs_mime_type_clean();
    vfs_file_monitor_clean();

    return 0;
}

void open_file( const char* path )
{
    GError * err;
    char *msg, *error_msg;
    VFSFileInfo* file;
    VFSMimeType* mime_type;
    gboolean opened;
    char* app_name;

    if ( ! g_file_test( path, G_FILE_TEST_EXISTS ) )
    {
        ptk_show_error( NULL, _("Error"), _( "File doesn't exist" ) );
        return ;
    }

    file = vfs_file_info_new();
    vfs_file_info_get( file, path, NULL );
    mime_type = vfs_file_info_get_mime_type( file );
    opened = FALSE;
    err = NULL;

    app_name = vfs_mime_type_get_default_action( mime_type );
    if ( app_name )
    {
        opened = vfs_file_info_open_file( file, path, &err );
        g_free( app_name );
    }
    else
    {
        VFSAppDesktop* app;
        GList* files;

        app_name = (char *) ptk_choose_app_for_mime_type( NULL, mime_type );
        if ( app_name )
        {
            app = vfs_app_desktop_new( app_name );
            if ( ! vfs_app_desktop_get_exec( app ) )
                app->exec = g_strdup( app_name ); /* This is a command line */
            files = g_list_prepend( NULL, (gpointer) path );
            opened = vfs_app_desktop_open_files( gdk_screen_get_default(),
                                                 NULL, app, files, &err );
            g_free( files->data );
            g_list_free( files );
            vfs_app_desktop_unref( app );
            g_free( app_name );
        }
        else
            opened = TRUE;
    }

    if ( !opened )
    {
        char * disp_path;
        if ( err && err->message )
        {
            error_msg = err->message;
        }
        else
            error_msg = _( "Don't know how to open the file" );
        disp_path = g_filename_display_name( path );
        msg = g_strdup_printf( _( "Unable to open file:\n\"%s\"\n%s" ), disp_path, error_msg );
        g_free( disp_path );
        ptk_show_error( NULL, _("Error"), msg );
        g_free( msg );
        if ( err )
            g_error_free( err );
    }
    vfs_mime_type_unref( mime_type );
    vfs_file_info_unref( file );
}
