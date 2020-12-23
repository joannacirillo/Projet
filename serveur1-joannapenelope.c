#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <math.h>

// #define CWND 10
#define SIZE_TAB 1000
#define SIZE_MESSAGE 1000
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct arg_struct{
  int arg_a; //nom de la socket utile
  struct sockaddr_in arg_client_addr;
  socklen_t arg_sockaddr_length;
  fd_set arg_socketDescriptorSet;
  int* arg_tab_ack;
  int* arg_window;
  int* arg_retransmission;
  struct timeval* send_time_tab;
  int* arg_cwnd;
};

struct arg_ca{
  int* arg_window;
  int* arg_cwnd;
  int* arg_mode;
  struct timeval* arg_srtt;
};


void display(int* array, int size){
  for(int i=0; i<size; i++){
    if(array[i]==1){
      printf("|%d", i);
    }
    if(array[i]==-1){
      printf("|(%d)", i);
    }
  }
  printf("|\n");
}

// FONCTION POUR CALCULER LE RTT (POUR LE TIMER DES ACKS)
struct timeval srtt_estimation(struct timeval old_srtt, struct timeval old_rtt){
  // SRTT(k) = alpha*SRTT(k-1) + (1-alpha)*RTT(k-1)
  double alpha = 0.3;
  struct timeval new_srtt;
  long new_srtt_db = alpha*(1000000*old_srtt.tv_sec + old_srtt.tv_usec) + (1-alpha)*(1000000*old_rtt.tv_sec + old_rtt.tv_usec);
  new_srtt.tv_sec = (time_t)(floor(new_srtt_db * 0.000001));
  new_srtt.tv_usec = (suseconds_t)(new_srtt_db - new_srtt.tv_sec*1000000);
  return new_srtt;
}

// FONCTION POUR LES MESSAGES DE CONNEXION (SYN, SYN-ACK, ACK)
void synack(int serverSocket, struct sockaddr_in client_addr, socklen_t sockaddr_length, int server_port2){
  // réception de SYN
  char syn[10] = "";
  int r = recvfrom(serverSocket, &syn, sizeof(syn), MSG_WAITALL, (struct sockaddr*)&client_addr, &sockaddr_length);
  if(r<0){perror(""); exit(0);}
  printf("%s received\n", syn);

  // envoi de SYN-ACK
  char synack[11] = "SYN-ACK";
  sprintf(synack+7, "%d", server_port2);
  int s = sendto(serverSocket, &synack, strlen(synack), 0, (struct sockaddr*)&client_addr, sockaddr_length);
  if(s<0){perror(""); exit(0);}
  printf("SYN-ACK sent\n");

  // réception de ACK
  char ack[10] = "";
  r = recvfrom(serverSocket, &ack, sizeof(ack), MSG_WAITALL, (struct sockaddr*)&client_addr, &sockaddr_length);
  if(r<0){perror(""); exit(0);}
  printf("%s received\n", ack);
}

// FONCTION ACCEPT QUI CRÉE UNE NOUVELLE SOCKET SUR UN 2ÈME PORT POUR LES MESSAGES UTILES
int acceptUDP(int server_port, int serverSocket, struct sockaddr_in client_addr, socklen_t sockaddr_length){

  int server_port2 = server_port+1;

  int serverSocket2 = socket(AF_INET, SOCK_DGRAM, 0);  // socket UDP pour les messages utiles
  printf("server socket #2: %d\n", serverSocket2);
  if(serverSocket2<0){exit(0);}

  struct sockaddr_in my_addr2;  // socket UDP pour les messages utiles
  memset((char*)&my_addr2, 0, sizeof(my_addr2));
  my_addr2.sin_family = AF_INET;
  my_addr2.sin_port = htons(server_port2);
  my_addr2.sin_addr.s_addr = INADDR_ANY;

  int b2 = bind(serverSocket2, (struct sockaddr*)&my_addr2, sizeof(my_addr2));
  if(b2<0){ perror("");}

  synack(serverSocket, client_addr, sockaddr_length, server_port2);

  return(serverSocket2); // retourne le descripteur de fichier de la nouvelle socket (sur le deuxième port) pour les messages utiles

}

void *congestion_avoidance(void *arguments){
  struct arg_ca *args = arguments;
  int* p_window = args->arg_window;
  int* p_cwnd = args->arg_cwnd;
  int* p_mode = args->arg_mode;
  struct timeval* p_srtt = args->arg_srtt;
  printf("Thread congestion avoidance\n");

  while(1){
    struct timeval t0;
    struct timeval t1;
    gettimeofday(&t0,0);
    while(1){
      gettimeofday(&t1,0);
      if((t1.tv_sec+t1.tv_usec*0.000001)>(t0.tv_sec+t0.tv_usec*0.000001)+((*p_srtt).tv_sec + (*p_srtt).tv_usec*0.000001)){
        //printf("srtt : %f\n",(*p_srtt).tv_sec + (*p_srtt).tv_usec*0.000001);
        printf("Update cwnd\n");
        if(*p_mode == 1){
          pthread_mutex_lock(&lock);
          *p_window = *p_window + 1;  // incrémentation de la fenêtre de transmission
          *p_cwnd = *p_cwnd + 1;
          pthread_mutex_unlock(&lock);
        }
        break;
      }
    }
  }
}

/*******************************************************************************
************ FONCTION EXÉCUTÉE PAR LE THREAD QUI REÇOIT LES ACK ****************
*******************************************************************************/
void *ack_routine(void *arguments){
  struct arg_struct *args = arguments;
  int a = args->arg_a;
  struct sockaddr_in client_addr = args->arg_client_addr;
  socklen_t sockaddr_length = args->arg_sockaddr_length;
  fd_set socketDescriptorSet = args->arg_socketDescriptorSet;
  int* tab_ack = args->arg_tab_ack;
  int* p_window = args->arg_window;
  int* p_retransmission = args->arg_retransmission;
  struct timeval *send_time_tab = args->send_time_tab;
  int* p_cwnd = args->arg_cwnd;

  struct timeval srtt;
  struct timeval srtt_for_select;
  srtt.tv_sec = 1; srtt.tv_usec = 0;
  struct timeval rtt;
  rtt.tv_sec = 1; rtt.tv_usec = 0;
  struct timeval receive_time_tab[SIZE_TAB];
  int sequenceNB;
  int mode = 0; //Mode de retransmission : 0:slow start 1:Congestion avoidance
  int ssthresh = 1000;

  printf("Thread created\n");

  // -- Création du thread pour congestion avoidance --
  pthread_t congestion_thread;

  // définition des arguments à passer au thread congestion avoidance :
  struct arg_ca args_congestion_avoidance;
  args_congestion_avoidance.arg_window = p_window;
  args_congestion_avoidance.arg_cwnd = p_cwnd;
  args_congestion_avoidance.arg_mode = &mode;
  args_congestion_avoidance.arg_srtt = &srtt;

  if(pthread_create(&congestion_thread, NULL, congestion_avoidance, (void*) &args_congestion_avoidance) != 0){
    perror("pthread_create() ack_thread:"); exit(0);
  }

  while(1){
    if(*p_cwnd>ssthresh){
      mode = 1;
    } else {
      mode = 0;
    }
    printf("mode : %d\n",mode);
    printf("cwnd : %d\n",*p_cwnd);
    if(mode==1){
      sleep(5000);
    }
    // réception du ACK avec select (non bloquant)
    FD_ZERO(&socketDescriptorSet);
    FD_SET(a, &socketDescriptorSet);
    srtt_for_select = srtt;
    select((a+1), &socketDescriptorSet, NULL, NULL, &srtt_for_select);

    if(FD_ISSET(a, &socketDescriptorSet)){  // il y a eu une activité sur serverSocket
      // réception du ACK
      char ack[11];
      memset(ack, 0, 11);
      int r = recvfrom(a, &ack, 10, MSG_WAITALL, (struct sockaddr *) &client_addr, &sockaddr_length);
      if(r<0){perror(""); exit(0);}
      char ackack[4];  // récupérer "ACK" dans "ACK00000N"
      memset(ackack, 0, 4);
      memcpy(ackack, ack, 3);
      char ackseq[7];  // récupérer "00000N" dans "ACK00000N"
      memset(ackseq, 0, 7);
      memcpy(ackseq, ack+3, 6);
      printf("ACK %d received\n", atoi(ackseq));
      sequenceNB = atoi(ackseq)%SIZE_TAB;
      gettimeofday(&receive_time_tab[sequenceNB],0);

      if(strcmp(ackack, "ACK")==0){

        if(tab_ack[sequenceNB]==1){  // ACK normal
          pthread_mutex_lock(&lock);
          *p_retransmission = 0;
          for(int i=0; i<*p_cwnd; i++){
            int index = sequenceNB-i;
            if(sequenceNB-i < 0){
              index = SIZE_TAB + index;
            }
            if(tab_ack[index]==1){
              tab_ack[index] = 2;  // 2 -> ce segment a été acquitté
              if(mode==0){
                *p_window = *p_window + 1;  // incrémentation de la fenêtre de transmission
                *p_cwnd = *p_cwnd + 1;
              }
            }
            else if(tab_ack[(sequenceNB-i)%SIZE_TAB]==0){
              break;
            }
          }
          pthread_mutex_unlock(&lock);
        }

        else if(tab_ack[sequenceNB]>=2 && tab_ack[(sequenceNB+1)%SIZE_TAB]==1){  // ACK dupliqué
          // if(tab_ack[sequenceNB]==2){
          //   printf("thread - ACK dupliqué 1 (%d)\n", atoi(ackseq));
          //   tab_ack[sequenceNB]=3;
          // }
          // else {
            printf("thread - ACK dupliqué 2 (%d)\n", atoi(ackseq));

            ssthresh = round(*p_window/2);
            printf("ssthresh : %d\n",ssthresh );
            pthread_mutex_lock(&lock);
            *p_retransmission = 1;
            *p_cwnd = ssthresh;
            pthread_mutex_unlock(&lock);
            mode = 1;
            // tab_ack[sequenceNB]=2;
          // }
        }

        // printf("thread: "); display(tab_ack, SIZE_TAB);

        if(tab_ack[(sequenceNB+1)%SIZE_TAB]==-1){
          printf("pthread_exit\n");
          pthread_exit(0);
        }

      }  // fin du if(strcmp(ack, "ACK"))
      timersub(&receive_time_tab[sequenceNB%SIZE_TAB], &send_time_tab[sequenceNB%SIZE_TAB], &rtt);
      srtt = srtt_estimation(srtt, rtt);

    }
    else{  // pas d'activité sur server socket
      printf("thread - timeout\n");
      ssthresh = round(*p_window/2);
      printf("ssthresh : %d\n",ssthresh );
      pthread_mutex_lock(&lock);
      *p_retransmission = 1;
      *p_cwnd = 1;
      pthread_mutex_unlock(&lock);
      mode = 0;
    }

  }
}

/*******************************************************************************
********************************** MAIN ****************************************
*******************************************************************************/

int main(int argc, char* argv[]){
  if(argc!=2){
    printf("Incorrect number of arguments. Please launch the program under the form: ./serveur <port-serveur>\n");
    exit(0);
  }

  // Configuration TCP

  int server_port = atoi(argv[1]);

  int serverSocket = socket(AF_INET, SOCK_DGRAM, 0);  // socket UDP
  printf("server socket #1: %d\n", serverSocket);
  if(serverSocket<0){exit(0);}

  struct sockaddr_in my_addr;  // socket UDP
  memset((char*)&my_addr, 0, sizeof(my_addr));
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons(server_port);
  my_addr.sin_addr.s_addr = INADDR_ANY;

  int b = bind(serverSocket, (struct sockaddr*)&my_addr, sizeof(my_addr));
  if(b<0){ perror("");}

  fd_set socketDescriptorSet;
  memset((char*)&socketDescriptorSet, 0, sizeof(socketDescriptorSet));

  struct sockaddr_in client_addr; // structure pour stocker le(s) client(s) UDP
  memset((char*)&client_addr, 0, sizeof(client_addr));
  socklen_t sockaddr_length = sizeof(client_addr);


  int a = acceptUDP(server_port, serverSocket, client_addr, sockaddr_length);

  // Envoi du fichier au client

  // while(1){  // tant que le client demande des fichiers
    char filename[100] = "";
    int r = recvfrom(a, &filename, sizeof(filename), MSG_WAITALL, (struct sockaddr*)&client_addr, &sockaddr_length);
    if(r<0){perror(""); exit(0);}

    struct timeval start;
    gettimeofday(&start,0);


    /***********************************************************************
    ************************** ENVOI DU FICHIER ****************************
    ***********************************************************************/

    // if(buffer[0]=='F' && buffer[1]=='I' && buffer[2]=='L' && buffer[3]=='E' && buffer[4]==' '){ // message received is asking for a file
      // char filename[50] = "";  // récupération du nom du fichier
      // for(int i=5; i<strlen(buffer)-1; i++){
      //   filename[i-5] = buffer[i];
      // }

      FILE* file = fopen(filename, "rb");  // ouverture du fichier à envoyer
      printf("%s opened\n", filename);
      int fr = 0;

      char file_data[994];
      int sequenceNB = 1;
      int tab_ack[SIZE_TAB];
      for(int i=0; i<SIZE_TAB; i++){
        tab_ack[i] = 0;
      }
      char tab_segments[SIZE_TAB][SIZE_MESSAGE];
      int cwnd = 1;
      int window = cwnd;
      int retransmission = 0;
      struct timeval send_time_tab[SIZE_TAB];
      for(int i=0; i<SIZE_TAB; i++){
        send_time_tab[i] = (struct timeval){0};
      }
      int size;

      // CRÉATION DU THREAD POUR LA RÉCEPTION DES ACK
      pthread_t ack_thread;

      // définition des arguments à passer au thread ack_thread :
      struct arg_struct args;
      args.arg_a = a;
      args.arg_client_addr = client_addr;
      args.arg_sockaddr_length = sockaddr_length;
      args.arg_socketDescriptorSet = socketDescriptorSet;
      args.arg_tab_ack = tab_ack;
      args.arg_window = &window;
      args.send_time_tab = send_time_tab;
      args.arg_retransmission = &retransmission;
      args.arg_cwnd = &cwnd;

      if(pthread_create(&ack_thread, NULL, ack_routine, (void*) &args) != 0){
        perror("pthread_create() ack_thread:"); exit(0);
      }
      if(pthread_mutex_init(&lock, NULL) != 0){  // initialisation du mutex
        perror("pthread_mutex_init():"); exit(0);
      }

      do{  // tant que le fichier n'a pas été envoyé en entier

        if(window>0 && retransmission==0){
          if(fr==sizeof(file_data) || fr==0){
            printf("cwnd : %d\n",cwnd);
            // Construction du paquet à envoyer (découpage du fichier + n° de séquence)
            char message[SIZE_MESSAGE] = "";
            sprintf(message, "%06d", sequenceNB);  // put sequence number in message
            fr = fread(file_data, 1, sizeof(file_data), file);
            memcpy(message + 6, file_data, fr);  // put read data in message after sequence number
            memcpy(&tab_segments[sequenceNB%SIZE_TAB], &message, fr+6); // put read data in tab_segments at index corresponding to current sequence number
            // memcpy(&tab_segments[sequenceNB%SIZE_TAB] + fr+7, &"\0", 1); // put read data in tab_segments at index corresponding to current sequence number
            // printf("size of tab_segments[%d]: %d\n", sequenceNB, sizeof(tab_segments[sequenceNB%SIZE_TAB]));

            // Envoi du paquet
            int s = sendto(a, message, fr+6, MSG_CONFIRM, (const struct sockaddr *) &client_addr, sockaddr_length);  // send message
            if(s<0){perror(""); exit(0);}
            printf("packet #%d sent\n", sequenceNB);
            // printf("size: %d\n", s);
            gettimeofday(&send_time_tab[sequenceNB%SIZE_TAB],0);

            // Mise à jour sliding window
            pthread_mutex_lock(&lock);
            window = window - 1;  // Décrémentation de la fenêtre de transmission
            tab_ack[sequenceNB%SIZE_TAB] = 1;  // On met le numéro de séquence du paquet envoyé dans le buffer
            sequenceNB = sequenceNB + 1;  // incrémentation du numéro de séquence
            if(fr!=sizeof(file_data)){
              tab_ack[sequenceNB%SIZE_TAB] = -1;
            }
            pthread_mutex_unlock(&lock);
          }
        }
        else if(retransmission==1){
          printf("RETRANSMISSION\n");
          printf("window : %d\n",window);
          for(int i=0; i<SIZE_TAB; i++){
            if(tab_ack[i]==1){  // si le segment n°i a été envoyé mais pas acquitté
              // Construction du paquet à envoyer (découpage du fichier + n° de séquence)
              if(feof(file) && i == (sequenceNB-1)%SIZE_TAB){
                size = fr+6;
              }
              else{
                size = sizeof(tab_segments[i]);
              }
              char message[SIZE_MESSAGE] = "";
              memcpy(&message, &tab_segments[i], size);  // put read data in message after sequence number

              // Envoi du paquet
              int s = sendto(a, message, size, MSG_CONFIRM, (const struct sockaddr *) &client_addr, sockaddr_length);  // send message
              if(s<0){perror(""); exit(0);}
              printf("packet #%d sent\n", i);
              gettimeofday(&send_time_tab[i],0);
            }
          }
          pthread_mutex_lock(&lock);
          window = cwnd;
          pthread_mutex_unlock(&lock);
          printf("window : %d\n",window);
          retransmission = 0;
        }

  }while(tab_ack[(sequenceNB-1)%SIZE_TAB]!=2 || !feof(file));  // le dernier paquet envoyé n'a pas encore été acquitté et le paquet suivant n'est pas le dernier

      pthread_join(ack_thread, NULL);

      // envoi d'un message DONE pour dire que c'est la fin
      int s = sendto(a, "FIN", 4, MSG_CONFIRM, (const struct sockaddr*)&client_addr, sockaddr_length);
      if(s<0){perror(""); exit(0);}
      printf("FIN sent\n");

      // char ack[5]="";
      // r = recvfrom(a, &ack, 5, MSG_WAITALL, (struct sockaddr *) &client_addr, &sockaddr_length);
      // if(r<0){perror(""); exit(0);}
      // if(strcmp(ack, "ACK")==0){printf("ACK received\n");}


    // }

    /******************* FIN DE L'ENVOI DU FICHIER ************************/

  close(serverSocket);

  struct timeval end;
  gettimeofday(&end,0);
  struct timeval total_time;
  timersub(&end, &start, &total_time);
  printf("total time: %ld' %ld''\n", total_time.tv_sec, total_time.tv_usec);



  return 0;
}
