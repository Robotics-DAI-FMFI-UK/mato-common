#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include "../../mato.h"
#include "AB.h"

void print_list_of_modules()
{
        printf("List of all modules:\n");
        GArray *modules_list = mato_get_list_of_all_modules();
        for(int i = 0; i < modules_list->len; i++)
        {
            module_info *info = g_array_index(modules_list, module_info *, i);
            printf("%d: module_id=%d, type=%s, name=%s\n", i, info->module_id, info->type, info->name);
        }
        mato_free_list_of_modules(modules_list);

        printf("List of all modules of type B:\n");
        modules_list = mato_get_list_of_modules("B");
        for(int i = 0; i < modules_list->len; i++)
        {
            module_info *info = g_array_index(modules_list, module_info *, i);
            printf("%d: module_id=%d, type=%s, name=%s\n", i, info->module_id, info->type, info->name);
        }
        mato_free_list_of_modules(modules_list);
}

int main(int argc, char **argv)
{
    int this_node_id = 0;
    if (argc > 1) sscanf(argv[1], "%d", &this_node_id);

    printf("----\nThis test is to be run from three different terminals:\n  ./test_distributed_AB 0\n  ./test_distributed_AB 1\n  ./test_distributed_AB 2\n----\n\n");

    printf("initializing framework...\n");
    mato_init(this_node_id, 0);

    do {
        printf("initializing module types...\n");
        A_init();
        B_init();

        printf("creating instances of modules...\n");
        char module_name[6];
        char mtype[2];
        mtype[1] = 0;
        int module_ids[2][2];
        for (int type = 'A'; type <= 'B'; type++)
          for (int ord = 0; ord < 2; ord++)
          {
            sprintf(module_name, "n%d_%c%d", this_node_id, type, ord);
            mtype[0] = type;
            module_ids[type - 'A'][ord] =  mato_create_new_module_instance(mtype, module_name);
          }

        printf("Waiting for modules in other frameworks to be created...\n");
        while (program_runs && (mato_get_number_of_modules() < 12)) usleep(100000);
        if (!program_runs) break;

        printf("framework %d sends HELLO message\n", this_node_id);
        mato_send_global_message(mato_main_program_module_id(), MESSAGE_HELLO, 3, "hi");

        print_list_of_modules();

        printf("starting...\n");
        mato_start();

        printf("main loop...\n");
        sleep(2);
        while (program_runs && (mato_threads_running() > 0)) sleep(1);

        printf("deleting instances...\n");
        for (int type = 'A'; type <= 'B'; type++)
          for (int ord = 0; ord < 2; ord++)
            mato_delete_module_instance(module_ids[type - 'A'][ord]);
    } while (0);

    mato_shutdown();

    printf("main program terminates.\n");
    return 0;
}

