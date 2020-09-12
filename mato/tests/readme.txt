Test programs for the Mato control framework.

They are meant to serve also as a short tutorial to the framework.

The first four tests run on a single computational node (a single
computer), whereas, the fifth test runs in a distributed
environment: three computers, each starts its own copy of the
main program.

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
    - the module type init function A_init() should register the
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
    - in all tests: pressing CTRL-C sends a signal that is
      caught and leads to controlled shutdown of the framework
      (for the distributed case: only on that particular node).


02_modules_A_B/

  Adding another module type: now we have module types A and B.
  The main program creates two instances of each type,
  named A1, A2, B1, B2. Otherwise, it does the same as in the
  previous test.

  The modules register to receive messages from another module
  (so called subscribing).
  They create a subscribe loop: a1 -> b2 -> a2 -> b1 -> a1
  The subscription type is direct_data_ptr, i.e. the message
  callback receives a pointer to read-only data and it is
  expected to finish very fast. After the callback function
  returns, the module is not allowed to access the message data.
  Each module is producing 5 messages, containing a unique integer.
  Upon receipt of a message from a subscribed channel,
  each module with the exception of B2 posts (i.e. forwards)
  the received message further on after modifying the contained
  integer.

  To notice:

    - we are using a pipe() mechanism to synchronize the
      startup process, the startup sequence is as follows:

       1. the framework itself is initialized,
       2. all module types are initialized (and thus registered
           in the framework),
       3. all instances are created (and thus their instance
           data, but the modules are not started yet),
       4. all modules are started (and thus they register
           for the output channels of other modules, and
           start their threads, but the threads wait for
           notification before they really start to work),
       5. the main program sends a global message that
           is cought by all the modules, consequently
           notifying their threads (by writing to the
           synchronization pipe) that everything is ready
           (i.e. all modules have finished subscribing)
           so that the module threads can start posting the data...


03_A_B_with_copy/

  Exactly the same as previous test, with the exception that
  the subscribed module receives a copy of the data instead
  of read-only pointer. It is expected to deallocate the
  data when it is not needed anymore. In this case, the
  data are released immediatelly after the integer value
  is extracted, but it could have been done any time later.


04_A_B_with_borrowed_ptr/

  In the previous two tests, the module types A and B were
  implemented separately as two file pairs (A.c, A.h) and
  (B.c, B.h). Since they behave very similarly, they were
  merged into a single pair (AB.c, AB.h), while the
  differences are the names and types of the module instances
  and the modules they are subscribing to.

  In this test the subscribe callbacks receive a "borrowed pointer"
  to data, which is the same as a direct data pointer - it
  points to read-only data, but the module is allowed to use
  this data even after the callback function returns: in fact
  it can use the data as long as it finds useful, but it is
  required to tell the framework when the data is not needed
  anymore by calling mato_release_data().

  The modules thus create a separate thread, which is consuming
  all the data. The message callback only sends a pointer
  to the received message data to a pipe, which wakes up
  the message eating thread and returns immediately. The
  awaken message eating thread consumes the message - i.e.
  forwards it further by posting it with a modified value
  after sleeping some module-specific time, and then notifies
  the framework that the message is not needed anymore.


05_distributed_AB/

  This test is run three times from computers according
  to a config file mato_nodes.conf (it can be the same,
  such as localhost, but it does not matter for
  the functionality). The test assumes there are three
  nodes (i.e. computers) where it is started - with
  different single command-line argument 0, 1, or 2.
  Each node creates 4 modules (A1, A2, B1, B2), and
  the names are prefixed by "nX_", since the module
  names are global across the whole distributed computation
  environment.

  As in previous tests, there is a subscription loop
  with forwarding with a single exception at the end
  of the loop. The difference is that the modules are
  subscribed across the computational nodes, even though
  the distribution is transparent for the modules:
  they do not care which computer the other node where
  the message originates runs.

  The architecture of the distributed system is fully
  connected: each node is connected to each node.
  Whenever some node disconnects, the other nodes
  try to reconnect. The modules should in principle
  take care of such situations - in this test we
  assume no disconnections, but the nodes can be
  started in arbitrary order.

  The message data are received in the form of
  borrowed pointer and consumed in a separate
  thread as in the previous test.

  To notice:

    The startup procedure now has more steps: the
    modules wait for the moment when all nodes are
    mutually interconnected. This is verified by
    the main programs of the nodes checking the
    number of modules present in the system. As soon
    as the number of modules seen by a node is 12,
    all nodes must be connected to this node, and
    the node sends out a global notification message
    from its main program and lets its modules to
    subscribe to other modules.
    All modules monitor the notification messages,
    after receiving 3 messages (i.e. from all nodes),
    they can be sure, all nodes are interconnected.
    Finally, the modules wait a little bit to let
    all other modules finish subscribing. Theretically,
    here some modules may start posting data before
    some other modules are subscribed, and some messages
    could be "lost" - however, it is best practice
    to write modules in such a way that they are
    not dependent on receiving all messages from
    the startup, rather to act properly as soon
    as the messages start coming. Another level of
    synchronization barrier could ensure that the
    messages are not sent before all nodes finished
    their subscriptions.

06_messages/

  In the above tests, the scenario of data transmition was
  based on the fact that the one who receives a message knows
  where does the data originate and either asks for them
  directly (mato_get_data()), or subscribes to them.
  However, in some other scenarios, a module may provide
  a service, where other (uknown) modules send requests.
  In this situation we need a mechanism to send a message
  to a particular destination module.
  Similarly as the mato_send_global_message() which sends
  a broadcast to all modules, any module can send any data
  to any other module using mato_send_message(). 
  The message is handled immediatelly in the same thread
  (on the same node, or from communication thread if sent
  remotely), so precaution need to be taken to avoid delays
  and possible deadlocks. 
  The direct messages are received in the same callbacks
  as the global (broadcast) messages.

  In this example, we assume two compuational nodes (0, and 1),
  each creating modules A1, A2, B1, B2. The messages received
  are pushed to queue the same way as in the previous test
  and handled in a separate thread.

  The demo here is to generate all prime numbers lower than 100
  in a sequence: each module generates a next prime and sends it
  further in a message forwarding loop (all 8 modules are 
  part of this single forwarding loop). Since every module starts
  with generating 2 and sending it further, the whole generating
  loop occurs 8 times (at the same time, in parallel, asynchronously,
  i.e. primes < 100 are generated 8 times). After a module receives
  a prime that has no follow-up (97 in this case), it will
  announce it to every other module using a broadcast message.
  When all modules receive 7(+1) notifications about the end
  of prime list, they know all lists were generated, and terminate.

07_logs_with_distributed/

  This is the very same test as 05_distributed, however, it now
  uses logging, instead of printing to console using printf().
  Mato framework has a simple logging system, where all messages
  are printed to the console (if required) and saved to a file
  with a timestamp, name of the thread that produces the message,
  and the log severity level. 

08_mato_config/

  Mato framework contains a simple text-file based config file
  parser. You can use a your config file to configure the framework
  and your application with a text config file, where lines take
  the form 
   variable: value
  The parser recognizes values of type int, double, and char *,
  and everything behind a # character is treated as a comment
  at the same line. This example uses an example config file
  test_config.cfg to demonstrate how to use this feature.

