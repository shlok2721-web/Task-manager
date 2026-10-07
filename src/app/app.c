#include "app.h"
#include "../ui/ui.h"
#include "../system/process_backend.h"
#include "../pages/performance/performance.h"
#include "../pages/processes/processes.h"
#include "../pages/applications/applications.h"
#include "../pages/startup/startup.h"
#include "../pages/services/services.h"
#include "../pages/storage/storage.h"
#include "../pages/users/users.h"
#include "../pages/fan_control/fan_control.h"

struct _App {
    GObject parent_instance;
    AdwApplication *application;
    AdwApplicationWindow *window;
    GtkStack *stack;
    ProcessBackend *process_backend;
    guint process_timer;
};

G_DEFINE_FINAL_TYPE(App, app, G_TYPE_OBJECT)

enum { PROCESSES_UPDATED, N_APP_SIGNALS };
static guint app_signals[N_APP_SIGNALS];

typedef struct {
    const char *name;
    const char *title;
    GtkWidget *(*create)(App *app);
} PageDefinition;

static const PageDefinition pages[] = {
    { "performance", "Performance", performance_page_create },
    { "processes", "Processes", processes_page_create },
    { "applications", "Applications", applications_page_create },
    { "startup", "Startup Applications", startup_page_create },
    { "services", "Services", services_page_create },
    { "storage", "Storage", storage_page_create },
    { "users", "Users & Sessions", users_page_create },
    { "fan-control", "Fan Control", fan_control_page_create },
};

static gboolean
process_tick(gpointer data)
{
    App *app = data;

    if (!app || !app->process_backend)
        return G_SOURCE_CONTINUE;


    gboolean changed =
        process_backend_refresh(app->process_backend);


    if (changed) {

        g_signal_emit(
            app,
            app_signals[PROCESSES_UPDATED],
            0
        );

    }


    return G_SOURCE_CONTINUE;
}

static void app_dispose(GObject *object)
{
    App *app = TASK_APP(object);
    if (app->process_timer) {
        g_source_remove(app->process_timer);
        app->process_timer = 0;
    }
    process_backend_free(app->process_backend);
    app->process_backend = NULL;
    g_clear_object(&app->window);
    g_clear_object(&app->application);
    G_OBJECT_CLASS(app_parent_class)->dispose(object);
}

static void app_class_init(AppClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = app_dispose;
    app_signals[PROCESSES_UPDATED] = g_signal_new(
        "processes-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void app_init(App *app)
{
    app->process_backend = process_backend_new();
}

AdwApplication *app_get_application(App *app) { return app ? app->application : NULL; }
GtkWindow *app_get_window(App *app) { return app ? GTK_WINDOW(app->window) : NULL; }
GtkStack *app_get_stack(App *app) { return app ? app->stack : NULL; }
GPtrArray *app_get_processes(App *app) { return app ? process_backend_get_processes(app->process_backend) : NULL; }

gulong app_connect_processes_updated(App *app, AppProcessesUpdatedFunc callback, gpointer user_data)
{
    if (!app || !callback) return 0;
    return g_signal_connect(app, "processes-updated", G_CALLBACK(callback), user_data);
}

void app_disconnect_processes_updated(App *app, gulong handler_id)
{
    if (app && handler_id) g_signal_handler_disconnect(app, handler_id);
}

gboolean app_process_is_running(App *app, const char *program_name)
{
    return app && process_backend_is_running(app->process_backend, program_name);
}

gboolean app_process_sample_pid(App *app, pid_t pid, ProcessSample *out)
{
    if (!app || !out)
        return FALSE;
    return process_backend_sample_pid_for_backend(app->process_backend, pid, out);
}

static GtkWidget *
create_pages(App *app, GtkStack *stack)
{
    guint count = G_N_ELEMENTS(pages);

    const char *limit_env = g_getenv("TASK_MANAGER_PAGE_LIMIT");

    if (limit_env && *limit_env) {
        char *end = NULL;
        unsigned long requested = strtoul(limit_env, &end, 10);

        if (end != limit_env && *end == '\0' && requested < count)
            count = (guint)requested;
    }


    for (guint i = 0; i < count; ++i) {

        GtkWidget *page = pages[i].create(app);

        if (!page) {
            continue;
        }


        gtk_stack_add_titled(
            stack,
            page,
            pages[i].name,
            pages[i].title);

    }

    gtk_stack_set_transition_type(
        stack,
        GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    gtk_stack_set_transition_duration(stack, 140);
    gtk_stack_set_vhomogeneous(stack, FALSE);
    gtk_stack_set_hhomogeneous(stack, FALSE);

    return GTK_WIDGET(stack);
}

void app_activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;
    App *existing = g_object_get_data(G_OBJECT(application), "task-manager-app");
    if (existing) {
        gtk_window_present(GTK_WINDOW(existing->window));
        return;
    }

    App *app = g_object_new(app_get_type(), NULL);
    app->application = g_object_ref(ADW_APPLICATION(application));

    ui_load_css();

    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app->application)));
    app->window = g_object_ref(window);
    gtk_window_set_title(GTK_WINDOW(window), "Task Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 1440, 900);
    gtk_widget_set_size_request(GTK_WIDGET(window), 900, 620);

    AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(ui_create_header()));

    app->stack = GTK_STACK(gtk_stack_new());
    gtk_widget_set_hexpand(GTK_WIDGET(app->stack), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(app->stack), TRUE);
    create_pages(app, app->stack);

    GtkWidget *sidebar = ui_create_sidebar(app->stack);
    AdwNavigationPage *side_page = adw_navigation_page_new(sidebar, "Navigation");
    AdwNavigationPage *content_page = adw_navigation_page_new(GTK_WIDGET(app->stack), "Content");

    AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    adw_navigation_split_view_set_sidebar(split, side_page);
    adw_navigation_split_view_set_content(split, content_page);
    adw_navigation_split_view_set_min_sidebar_width(split, 190);
    adw_navigation_split_view_set_max_sidebar_width(split, 280);
    adw_navigation_split_view_set_sidebar_width_fraction(split, 0.18);
    adw_toolbar_view_set_content(toolbar, GTK_WIDGET(split));

    adw_application_window_set_content(window, GTK_WIDGET(toolbar));

    g_object_set_data_full(G_OBJECT(application), "task-manager-app", app, g_object_unref);
    gtk_window_present(GTK_WINDOW(window));

    /*
     * TEMPORARY CRASH ISOLATION:
     *
     * The process backend refresh is disabled here intentionally.
     * Page construction succeeds, but the application crashes shortly
     * after startup when the recurring process timer begins running.
     *
     * Do not remove this comment until the process refresh ownership/
     * signal path has been fixed.
     */
    app->process_timer = g_timeout_add(1000, process_tick, app);
}
