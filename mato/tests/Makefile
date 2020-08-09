GLIB_INCLUDE=-I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/lib/aarch64-linux-gnu/glib-2.0/include 

GLIB_LIBDIR=-L/usr/lib/aarch64-linux-gnu -L/usr/lib/x86_64_linux-gnu
MATO_LIBS=-lglib-2.0 -lpthread 

# WITH_DEBUG=-g
WITH_DEBUG=

all: test_two_modules_A test_modules_A_B test_A_B_with_copy

test_two_modules_A: 01_two_modules_A/test_two_modules_A.c 01_two_modules_A/A.c ../mato.c
	gcc -o test_two_modules_A $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(WITH_DEBUG) $(MATO_LIBS)


test_modules_A_B: 02_modules_A_B/test_modules_A_B.c 02_modules_A_B/A.c 02_modules_A_B/B.c ../mato.c
	gcc -o test_modules_A_B $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(MATO_LIBS) $(WITH_DEBUG)

test_A_B_with_copy: 03_A_B_with_copy/test_A_B_with_copy.c 03_A_B_with_copy/A.c 03_A_B_with_copy/B.c ../mato.c
	gcc -o test_A_B_with_copy $^ $(GLIB_INCLUDE) $(GLIB_LIBDIR) $(MATO_LIBS) $(WITH_DEBUG)


clean:
	rm test_two_modules_A test_modules_A_B test_A_B_with_copy


