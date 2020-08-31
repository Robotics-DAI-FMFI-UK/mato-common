Test programs for the Mato control framework. 

They are meant to serve also as a short tutorial to the framework.

The first four tests are meant to run on a single computational node 
(a single computer), whereas, the fifth test runs in a distributed
environment (three computers, each starts its own main program).

Run make to build all tests.


01_two_modules_A/

  The most simple test with single module type: A.

  The main program:

    Creates two module instances of type A named A1 and A2.
    Prints list of running modules.
    Starts the framework, sends a global message from main program,
    then waits until all modules' threads finish, finally
    deletes the module instances and shutdowns the framework.


  The module of type A:

    Has a very simple instance data structure - just to remember its
    own module_id. When started, it creates its own thread, from 
    its thread it posts 10 messages containing simple integer 0..9,
    then it quits. 
    On a global message, it prints its contents.

  To notice:
    - the module type init functino A_init() should register the 
      module type in the framework using the module type
      specification structure that defines the callback functions
      and the number of channels
    - the create instance callback function should allocate
      instance data and return it
    - when posting messages, allocate the buffer by calling
      mato_get_data_buffer()
    - when a thread starts, it calls mato_inc_thread_count(),
      and before it terminates, it calls mato_dec_thread_count().
    - the delete instance callback function should deallocate
      the instance data
    - to send a global message from the main program, use
      the special module id of the main program returned
      by the mato_main_program_module_id() function.


02_modules_A_B/



03_A_B_with_copy/



04_A_B_with_borrowed_ptr/



05_distributed_AB/


