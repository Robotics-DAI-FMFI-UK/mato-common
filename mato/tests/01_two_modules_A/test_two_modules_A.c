#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include "../../mato.h"
#include "A.h"

int a1, a2;

int main(int argc, char **argv)
{
    int this_node_id = 0;
    if (argc > 1) sscanf(argv[1], "%d", &this_node_id);
    
    printf("initializing framework...\n");
    mato_init(this_node_id);

    printf("initializing module types...\n");
    A_init();

    printf("creating instances of modules...\n");

    a1 = mato_create_new_module_instance("A", "A1");
    a2 = mato_create_new_module_instance("A", "A2");
	   
    printf("List of all modules:\n");
    GArray *modules_list = mato_get_list_of_all_modules();
    for(int i = 0; i < modules_list->len; i++)
    {
		module_info *info = g_array_index(modules_list, module_info *, i);	
		printf("%d: module_id=%d, type=%s, name=%s\n", i, info->module_id, info->type, info->name);
    }
    mato_free_list_of_modules(modules_list);

    printf("List of all modules of type A:\n");	
	modules_list = mato_get_list_of_modules("A");
    for(int i = 0; i < modules_list->len; i++)
    {
		module_info *info = g_array_index(modules_list, module_info *, i);	
		printf("%d: module_id=%d, type=%s, name=%s\n", i, info->module_id, info->type, info->name);
    }
    mato_free_list_of_modules(modules_list);
	
    printf("starting...\n");
    mato_start();

    printf("sending hello message...\n");
    mato_send_global_message(mato_main_program_module_id(), MESSAGE_HELLO, 9, "greeting");

    printf("main loop...\n");

    sleep(2);
    while (mato_threads_running() > 0) sleep(1); 

    printf("deleting instances...\n");
    mato_delete_module_instance(a1);
    mato_delete_module_instance(a2);

    mato_shutdown();

    printf("main program terminates.\n");
    return 0;
}
	
