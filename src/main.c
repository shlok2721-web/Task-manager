#include <adwaita.h>
#include "app/app.h"

int main(int argc, char **argv)
{
    g_set_application_name("Task Manager");
    AdwApplication *application = adw_application_new("com.example.TaskManager", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(app_activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
