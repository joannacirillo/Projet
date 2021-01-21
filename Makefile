all: serveur1-joannapenelope serveur2-joannapenelope serveur3-joannapenelope

serveur1-joannapenelope: serveur1-joannapenelope.o
	gcc serveur1-joannapenelope.o -o serveur1-joannapenelope -pthread -lm

serveur2-joannapenelope: serveur2-joannapenelope.o
	gcc serveur2-joannapenelope.o -o serveur2-joannapenelope -pthread -lm

serveur3-joannapenelope: serveur3-joannapenelope.o
	gcc serveur3-joannapenelope.o -o serveur3-joannapenelope -pthread -lm

serveur1-joannapenelope.o: serveur1-joannapenelope.c include/serveur1-joannapenelope.h
	gcc -Wall -c serveur1-joannapenelope.c -Iinclude -o serveur1-joannapenelope.o -pthread -lm

serveur2-joannapenelope.o: serveur2-joannapenelope.c include/serveur2-joannapenelope.h
	gcc -Wall -c serveur2-joannapenelope.c -Iinclude -o serveur2-joannapenelope.o -pthread -lm

serveur3-joannapenelope.o: serveur3-joannapenelope.c  include/serveur3-joannapenelope.h
	gcc -Wall -c serveur3-joannapenelope.c -Iinclude -o serveur3-joannapenelope.o -pthread -lm

clean:
	rm -f hello *.o
