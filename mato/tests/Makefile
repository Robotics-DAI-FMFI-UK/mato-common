GLIB_INCLUDE=-I /usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/lib/aarch64-linux-gnu/glib-2.0/include 

GLIB_LIBDIR=-L/usr/lib/aarch64-linux-gnu -L/usr/lib/x86_64_linux-gnu

all: test_two_modules_A test_modules_A_B

test_two_modules_A: 01_two_modules_A/test_two_modules_A.c 01_two_modules_A/A.c ../mato.c
	gcc -o test_two_modules_A 01_two_modules_A/test_two_modules_A.c 01_two_modules_A/A.c ../mato.c -I/usr/include/glib-2.0 $(GLIB_INCLUDE) -lglib-2.0 $(GLIB_LIBDIR) -lpthread -g


test_modules_A_B: 02_modules_A_B/test_modules_A_B.c 02_modules_A_B/A.c 02_modules_A_B/B.c ../mato.c
	gcc -o test_modules_A_B 02_modules_A_B/test_modules_A_B.c 02_modules_A_B/A.c 02_modules_A_B/B.c ../mato.c -I/usr/include/glib-2.0 $(GLIB_INCLUDE) -lglib-2.0 $(GLIB_LIBDIR) -lpthread -g

clean:
	rm test_two_modules_A test_modules_A_B


