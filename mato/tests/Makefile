GLIB_INCLUDE=-I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/lib/aarch64-linux-gnu/glib-2.0/include -I/usr/lib/arm-linux-gnueabihf/glib-2.0/include

GLIB_LIBDIR=-L/usr/lib/aarch64-linux-gnu -L/usr/lib/x86_64_linux-gnu -L/usr/lib/arm-linux-gnueabihf
MATO_LIBS=-lglib-2.0 -lpthread

WITH_DEBUG=-g
# WITH_DEBUG=

MATO_SRCS=../mato.c ../mato_core.c ../mato_net.c 

all: test_two_modules_A test_modules_A_B test_A_B_with_copy test_A_B_with_borrowed_ptr test_distributed_AB

test_two_modules_A: 01_two_modules_A/test_two_modules_A.c 01_two_modules_A/A.c $(MATO_SRCS)
	gcc -o test_two_modules_A $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(WITH_DEBUG) $(MATO_LIBS)

test_modules_A_B: 02_modules_A_B/test_modules_A_B.c 02_modules_A_B/A.c 02_modules_A_B/B.c $(MATO_SRCS)
	gcc -o test_modules_A_B $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(MATO_LIBS) $(WITH_DEBUG)

test_A_B_with_copy: 03_A_B_with_copy/test_A_B_with_copy.c 03_A_B_with_copy/A.c 03_A_B_with_copy/B.c $(MATO_SRCS)
	gcc -o test_A_B_with_copy $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(MATO_LIBS) $(WITH_DEBUG)

test_A_B_with_borrowed_ptr: 04_A_B_with_borrowed_ptr/test_A_B_with_borrowed_ptr.c 04_A_B_with_borrowed_ptr/AB.c $(MATO_SRCS)
	gcc -o test_A_B_with_borrowed_ptr $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(MATO_LIBS) $(WITH_DEBUG)

test_distributed_AB: 05_distributed_AB/test_distributed_AB.c 05_distributed_AB/AB.c $(MATO_SRCS)
	gcc -o test_distributed_AB $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(MATO_LIBS) $(WITH_DEBUG)

clean:
	rm test_two_modules_A test_modules_A_B test_A_B_with_copy test_A_B_with_borrowed_ptr test_distributed_AB

docs:
	cd .. && doxygen mato.dox && cd tests
