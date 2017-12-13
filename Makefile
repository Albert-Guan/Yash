yash: yash.o
		gcc -o yash yash.o
yash.o: yash.c
		gcc -c yash.c
clean:
		rm *.o yash
