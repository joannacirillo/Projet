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

#define SIZE_TAB 1000
#define SIZE_MESSAGE 1480
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
  double* arg_cwnd;
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

/******************************************************************************
 * ********* FONCTION POUR CALCULER LE RTT (POUR LE TIMER DES ACKS) ***********
 * ****************************************************************************/
struct timeval srtt_estimation(struct timeval old_srtt, struct timeval old_rtt){
  // SRTT(k) = alpha*SRTT(k-1) + (1-alpha)*RTT(k-1)
  double alpha = 0.5;
  struct timeval new_srtt;
  long new_srtt_db = alpha*(1000000*old_srtt.tv_sec + old_srtt.tv_usec) + (1-alpha)*(1000000*old_rtt.tv_sec + old_rtt.tv_usec);
  new_srtt.tv_sec = (time_t)(floor(new_srtt_db * 0.000001));
  new_srtt.tv_usec = (suseconds_t)(new_srtt_db - new_srtt.tv_sec*1000000);
  return new_srtt;
}

/******************************************************************************
 * ****** FONCTION POUR LES MESSAGES DE CONNEXION (SYN, SYN-ACK, ACK) *********
 * ****************************************************************************/
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

/********************************************************************************************
 ** FONCTION ACCEPT QUI CRÉE UNE NOUVELLE SOCKET SUR UN 2ÈME PORT POUR LES MESSAGES UTILES **
 *******************************************************************************************/
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


/*******************************************************************************
************ FONCTION EXÉCUTÉE PAR LE THREAD QUI REÇOIT LES ACK ****************
****************************** SANS AFFICHAGE **********************************
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
  double* p_cwnd = args->arg_cwnd;

  struct timeval srtt;
  struct timeval srtt_for_select;
  srtt.tv_sec = 1; srtt.tv_usec = 0;
  struct timeval rtt;
  rtt.tv_sec = 1; rtt.tv_usec = 0;
  struct timeval receive_time_tab[SIZE_TAB];
  int sequenceNB;
  int mode = 0; // mode de retransmission (0 : slow start, 1 : congestion avoidance)
  int ssthresh = 1000;


  while(1){
    mode = (*p_cwnd >= ssthresh);
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
      sequenceNB = atoi(ackseq)%SIZE_TAB;
      gettimeofday(&receive_time_tab[sequenceNB],0);

      if(strcmp(ackack, "ACK")==0){ // si le message reçu est bien un ack

        if(tab_ack[sequenceNB]==1){  // ACK normal (segment correspondant déjà envoyé et ack pas encore reçu)
          pthread_mutex_lock(&lock);
          *p_retransmission = 0;
          tab_ack[sequenceNB] = 3;  // 3 -> ce segment a été acquitté
          *p_window = *p_window - 1;  // décrémentation du nombre de paquets envoyés non acquittés
          // incrémentation de la fenêtre de congestion
          if(mode==0){ // slow start
            *p_cwnd = *p_cwnd + 1;
          }
          else if(mode==1){ // congestion avoidance
            *p_cwnd = *p_cwnd + 1/(*p_cwnd);
          }
          for(int i=1; i<SIZE_TAB; i++){
            int index = sequenceNB-i;
            if(sequenceNB-i < 0){ index = SIZE_TAB + index; }
            if(tab_ack[index]==1){
              tab_ack[index] = 2;  // 2 -> ce segment n'a été acquitté mais un ack supérieur a été reçu (donc le paquet a bien été reçu par le client)
              *p_window = *p_window - 1;  // décrémentation du nombre de paquets envoyés non acquittés
              // incrémentation de la fenêtre de congestion
              if(mode==0){ // slow start
                *p_cwnd = *p_cwnd + 1;
              }
              else if(mode==1){ // congestion avoidance
                *p_cwnd = *p_cwnd + 1/(*p_cwnd);
              }
            }
            else if(tab_ack[index]!=1){
              break;
            }
          }
          pthread_mutex_unlock(&lock);
        }

        else if(tab_ack[sequenceNB]==2){  // ACK normal (segment correspondant déjà envoyé et ack pas encore reçu mais ack supérieur déjà reçu)
          pthread_mutex_lock(&lock);
          *p_retransmission = 0;
          tab_ack[sequenceNB] = 3;  // 3 -> ce segment a été acquitté
          pthread_mutex_unlock(&lock);
        }

        else if(tab_ack[sequenceNB]==3 && tab_ack[(sequenceNB+1)%SIZE_TAB]==1){  // ACK dupliqué (1 et 2) (pas de retransmission)
            pthread_mutex_lock(&lock);
            tab_ack[sequenceNB]+=1;
            pthread_mutex_unlock(&lock);
        }

        else if(tab_ack[sequenceNB]>=6 && tab_ack[sequenceNB]%2==0 && tab_ack[(sequenceNB+1)%SIZE_TAB]==1){  // 3ème ACK dupliqué (=> retransmission)
            // congestion avoidance
            ssthresh = round(*p_cwnd/2);
            if(ssthresh==0){ ssthresh = 1; }

            pthread_mutex_lock(&lock);
            *p_retransmission = 1;
            *p_cwnd = ssthresh;
            tab_ack[sequenceNB]+=1;
            pthread_mutex_unlock(&lock);
        }

        if(tab_ack[(sequenceNB+1)%SIZE_TAB]==-1){
          pthread_exit(0);
        }

      }
      timersub(&receive_time_tab[sequenceNB%SIZE_TAB], &send_time_tab[sequenceNB%SIZE_TAB], &rtt);
      srtt = srtt_estimation(srtt, rtt);

    }
    else{  // pas d'activité sur server socket
      // congestion avoidance
      ssthresh = round(*p_cwnd/2);
      if(ssthresh==0){ ssthresh = 1; }

      pthread_mutex_lock(&lock);
      *p_retransmission = 1;
      *p_cwnd = 1;
      pthread_mutex_unlock(&lock);
    }

  }
}









/*******************************************************************************
************ FONCTION EXÉCUTÉE PAR LE THREAD QUI REÇOIT LES ACK ****************
****************************** AVEC AFFICHAGE **********************************
*******************************************************************************/
void *ack_routine_with_display(void *arguments){
  struct arg_struct *args = arguments;
  int a = args->arg_a;
  struct sockaddr_in client_addr = args->arg_client_addr;
  socklen_t sockaddr_length = args->arg_sockaddr_length;
  fd_set socketDescriptorSet = args->arg_socketDescriptorSet;
  int* tab_ack = args->arg_tab_ack;
  int* p_window = args->arg_window;
  int* p_retransmission = args->arg_retransmission;
  struct timeval *send_time_tab = args->send_time_tab;
  double* p_cwnd = args->arg_cwnd;

  struct timeval srtt;
  struct timeval srtt_for_select;
  srtt.tv_sec = 1; srtt.tv_usec = 0;
  struct timeval rtt;
  rtt.tv_sec = 1; rtt.tv_usec = 0;
  struct timeval receive_time_tab[SIZE_TAB];
  int sequenceNB;
  int mode = 0; // mode de retransmission (0 : slow start, 1 : congestion avoidance)
  int ssthresh = 1000;

  printf("Thread created\n");

  while(1){
    mode = (*p_cwnd >= ssthresh);
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

      if(strcmp(ackack, "ACK")==0){ // si le message reçu est bien un ack

        if(tab_ack[sequenceNB]==1){  // ACK normal (segment correspondant déjà envoyé et ack pas encore reçu)
          printf("ACK %d received\n", atoi(ackseq));
          pthread_mutex_lock(&lock);
          *p_retransmission = 0;
          tab_ack[sequenceNB] = 3;  // 3 -> ce segment a été acquitté
          *p_window = *p_window - 1;  // décrémentation du nombre de paquets envoyés non acquittés
          // incrémentation de la fenêtre de congestion
          if(mode==0){ // slow start
            *p_cwnd = *p_cwnd + 1;
          }
          else if(mode==1){ // congestion avoidance
            *p_cwnd = *p_cwnd + 1/(*p_cwnd);
          }
          for(int i=1; i<SIZE_TAB; i++){
            int index = sequenceNB-i;
            if(sequenceNB-i < 0){ index = SIZE_TAB + index; }
            if(tab_ack[index]==1){
              tab_ack[index] = 2;  // 2 -> ce segment n'a été acquitté mais un ack supérieur a été reçu (donc le paquet a bien été reçu par le client)
              *p_window = *p_window - 1;  // décrémentation du nombre de paquets envoyés non acquittés
              // incrémentation de la fenêtre de congestion
              if(mode==0){ // slow start
                *p_cwnd = *p_cwnd + 1;
              }
              else if(mode==1){ // congestion avoidance
                *p_cwnd = *p_cwnd + 1/(*p_cwnd);
              }
            }
            else if(tab_ack[index]!=1){
              break;
            }
          }
          pthread_mutex_unlock(&lock);
        }

        else if(tab_ack[sequenceNB]==2){  // ACK normal (segment correspondant déjà envoyé et ack pas encore reçu mais ack supérieur déjà reçu)
          printf("ACK %d received\n", atoi(ackseq));
          pthread_mutex_lock(&lock);
          *p_retransmission = 0;
          tab_ack[sequenceNB] = 3;  // 3 -> ce segment a été acquitté
          pthread_mutex_unlock(&lock);
        }

        else if(tab_ack[sequenceNB]==3 && tab_ack[(sequenceNB+1)%SIZE_TAB]==1){  // ACK dupliqué (1 et 2) (pas de retransmission)
            printf("thread - ACK dupliqué 1 (%d)\n", atoi(ackseq));
            pthread_mutex_lock(&lock);
            tab_ack[sequenceNB]+=1;
            pthread_mutex_unlock(&lock);
        }

        else if(tab_ack[sequenceNB]>=6 && tab_ack[sequenceNB]%2==0 && tab_ack[(sequenceNB+1)%SIZE_TAB]==1){  // 3ème ACK dupliqué (=> retransmission)
            printf("thread - ACK dupliqué 2 (%d)\n", atoi(ackseq));
            // congestion avoidance
            ssthresh = round(*p_cwnd/2);
            if(ssthresh==0){ ssthresh = 1; }
            printf("ssthresh : %d\n",ssthresh );

            pthread_mutex_lock(&lock);
            *p_retransmission = 1;
            *p_cwnd = ssthresh;
            tab_ack[sequenceNB]+=1;
            pthread_mutex_unlock(&lock);
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
      // congestion avoidance
      printf("thread - timeout\n");
      ssthresh = round(*p_cwnd/2);
      if(ssthresh==0){ ssthresh = 1; }
      printf("ssthresh : %d\n",ssthresh );

      pthread_mutex_lock(&lock);
      *p_retransmission = 1;
      *p_cwnd = 1;
      pthread_mutex_unlock(&lock);
    }

  }
}







/*******************************************************************************
********************************** MAIN ****************************************
*******************************************************************************/

int main(int argc, char* argv[]){
  if(argc!=2 && argc!=3){
    printf("Incorrect number of arguments. Please launch the program under the form: ./serveur <port-serveur>\n");
    exit(0);
  }

  // SANS AFFICHAGE
  if(argc==3){
    if(atoi(argv[2])==0){
      // Configuration TCP
      int server_port = atoi(argv[1]);
      int new_port = server_port; // numéro de port qui changera à chaque nouvelle connexion de client

      int serverSocket = socket(AF_INET, SOCK_DGRAM, 0);  // socket UDP
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

      struct sockaddr_in client_addr; // structure pour stocker les clients UDP
      memset((char*)&client_addr, 0, sizeof(client_addr));
      socklen_t sockaddr_length = sizeof(client_addr);

      int fr = 0; // taille des données lues dans le fichier - initialisation
      char file_data[SIZE_MESSAGE-6]; // données lues dans le fichier - initialisation
      int sequenceNB = 1; // numéro de séquence d'un paquet à envoyer - initialisation
      int tab_ack[SIZE_TAB]; /* tableau contenant le statut d'un paquet :
      0 : paquet pas encore envoyé
      1 : paquet envoyé et ACK pas encore reçu
      2 : ack pas encore reçu, mais ack d'un numéro de séquence supérieur reçu (donc paquet reçu par le client) (pour que le vrai ack ne soit pas considéré comme dupliqué et pour ne pas retransmettre ce paquet puisqu'il a déjà été reçu par le client)
      3 : ack reçu
      4 : ack dupliqué reçu
      5, 6, 7, etc. : nouveaux acks dupliqués (incrémentation de 1 à chaque nouvel ack reçu) (=> on peut décider à partir de combien d'acks dupliqués on retransmet) */
      for(int i=0; i<SIZE_TAB; i++){
        tab_ack[i] = 0;
      }
      char tab_segments[SIZE_TAB][SIZE_MESSAGE]; // tableau contenant tous les messages envoyés (pour pouvoir retransmettre sans relire dans le fichier)
      double cwnd = 1;  // taille de la fenêtre de transmission
      int window = 0; // nombre de paquets envoyés mais pas encore acquittés
      int retransmission = 0;  // booléen indiquant si on doit retransmettre (0 : non, 1 : oui)
      int nb_retransmissions = 0; // nombre de retransmissions pendant toute la durée d'envoi du fichier
      struct timeval send_time_tab[SIZE_TAB];  // tableau contenant les temps d'envoi de chaque paquet
      for(int i=0; i<SIZE_TAB; i++){
        send_time_tab[i] = (struct timeval){0};
      }
      int size;  // taille du message envoyé

      // CRÉATION DU THREAD POUR LA RÉCEPTION DES ACK
      pthread_t ack_thread;

      // définition des arguments à passer au thread ack_thread :
      struct arg_struct args;
      args.arg_a = 0;
      args.arg_client_addr = client_addr;
      args.arg_sockaddr_length = sockaddr_length;
      args.arg_socketDescriptorSet = socketDescriptorSet;
      args.arg_tab_ack = tab_ack;
      args.arg_window = &window;
      args.send_time_tab = send_time_tab;
      args.arg_retransmission = &retransmission;
      args.arg_cwnd = &cwnd;

      while(1){
        FD_ZERO(&socketDescriptorSet);
        FD_SET(serverSocket, &socketDescriptorSet);
        select(serverSocket+1, &socketDescriptorSet, NULL, NULL, NULL);
        if(FD_ISSET(serverSocket, &socketDescriptorSet)){  // connexion d'un nouveau client
          int a = acceptUDP(new_port, serverSocket, client_addr, sockaddr_length);
          new_port += 1;
          args.arg_a = a;

          int pid = fork(); // création d'un processus fils pour traiter ce client
          
          if(pid==0){  // processus fils
            close(serverSocket);

            // réception du nom du fichier
            char filename[100] = "";
            int r = recvfrom(a, &filename, sizeof(filename), MSG_WAITALL, (struct sockaddr*)&client_addr, &sockaddr_length);
            if(r<0){perror(""); exit(0);}
            long total_size_file = 0;  // taille du fichier (pour calculer le débit à la fin)
            if(!strcmp(filename, "ef1663en.pdf")){ total_size_file = 2154227*8; }
            else if(!strcmp(filename, "projet2020.pdf")){ total_size_file = 103197*8; }

            // démarrage du timer
            struct timeval start;
            gettimeofday(&start,0);

            // ouverture du fichier à envoyer
            FILE* file = fopen(filename, "rb");

            // création du thread pour la réception des ack
            if(pthread_create(&ack_thread, NULL, ack_routine, (void*) &args) != 0){
              perror("pthread_create() ack_thread:"); exit(0);
            }
            // initialisation du mutex
            if(pthread_mutex_init(&lock, NULL) != 0){
              perror("pthread_mutex_init():"); exit(0);
            }

            do{  // tant que le fichier n'a pas été envoyé en entier

              if(floor(cwnd)-window>0 && retransmission==0){
                if(fr==sizeof(file_data) || fr==0){
                  // Construction du paquet à envoyer (découpage du fichier + n° de séquence)
                  char message[SIZE_MESSAGE] = "";
                  sprintf(message, "%06d", sequenceNB);  // put sequence number in message
                  fr = fread(file_data, 1, sizeof(file_data), file);
                  memcpy(message + 6, file_data, fr);  // put read data in message after sequence number
                  memcpy(&tab_segments[sequenceNB%SIZE_TAB], &message, fr+6); // put read data in tab_segments at index corresponding to current sequence number

                  // Envoi du paquet
                  int s = sendto(a, message, fr+6, MSG_CONFIRM, (const struct sockaddr *) &client_addr, sockaddr_length);  // send message
                  if(s<0){perror(""); exit(0);}
                  gettimeofday(&send_time_tab[sequenceNB%SIZE_TAB],0);

                  // Mise à jour sliding window
                  pthread_mutex_lock(&lock);
                  window = window + 1;  // Incrémentation du nombre de paquets envoyés mais pas encore acquittés
                  tab_ack[sequenceNB%SIZE_TAB] = 1;  // On met le numéro de séquence du paquet envoyé dans le buffer
                  sequenceNB = sequenceNB + 1;  // incrémentation du numéro de séquence
                  if(fr!=sizeof(file_data)){
                    tab_ack[sequenceNB%SIZE_TAB] = -1;
                  }
                  pthread_mutex_unlock(&lock);
                }
              }
              else if(retransmission==1){
                nb_retransmissions += 1;
                window = 0;
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
                    gettimeofday(&send_time_tab[i],0);
                    pthread_mutex_lock(&lock);
                    window += 1;
                    pthread_mutex_unlock(&lock);
                  }
                }

                retransmission = 0;
              }
            }while(tab_ack[(sequenceNB-1)%SIZE_TAB]<=2 || !feof(file));  // le dernier paquet envoyé n'a pas encore été acquitté et le paquet suivant n'est pas le dernier

            pthread_join(ack_thread, NULL);

            // envoi d'un message FIN pour dire que c'est la fin
            int s = sendto(a, "FIN", 4, MSG_CONFIRM, (const struct sockaddr*)&client_addr, sockaddr_length);
            if(s<0){perror(""); exit(0);}

            /******************* FIN DE L'ENVOI DU FICHIER ************************/

              struct timeval end;
              gettimeofday(&end,0);
              struct timeval total_time;
              timersub(&end, &start, &total_time);
              printf("total time: %ld' %ld''\n", total_time.tv_sec, total_time.tv_usec);
              if(total_size_file != 0){
                double debit = (double)(total_size_file) / (double)((total_time.tv_sec * 1000000 + total_time.tv_usec));
                printf("débit : %f\n", debit);
              }
              printf("nombre de retransmissions: %d\n", nb_retransmissions);

              printf("pid %d is over\n", pid);
              exit(0);
          }

          else if(pid>0){  // processus père
            close(a);
          }

        }

      }

      close(serverSocket);

      return 0;
    }
  }




  // AVEC AFFICHAGE
  else{
        // Configuration TCP

    int server_port = atoi(argv[1]);
    int new_port = server_port; // numéro de port qui changera à chaque nouvelle connexion de client

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

    struct sockaddr_in client_addr; // structure pour stocker les clients UDP
    memset((char*)&client_addr, 0, sizeof(client_addr));
    socklen_t sockaddr_length = sizeof(client_addr);

    int fr = 0; // taille des données lues dans le fichier - initialisation
    char file_data[994]; // données lues dans le fichier - initialisation
    int sequenceNB = 1; // numéro de séquence d'un paquet à envoyer - initialisation
    int tab_ack[SIZE_TAB]; /* tableau contenant le statut d'un paquet :
    0 : paquet pas encore envoyé
    1 : paquet envoyé et ACK pas encore reçu
    2 : ack pas encore reçu, mais ack d'un numéro de séquence supérieur reçu (donc paquet reçu par le client) (pour que le vrai ack ne soit pas considéré comme dupliqué et pour ne pas retransmettre ce paquet puisqu'il a déjà été reçu par le client)
    3 : ack reçu
    4 : ack dupliqué reçu
    5, 6, 7, etc. : nouveaux acks dupliqués (incrémentation de 1 à chaque nouvel ack reçu) (=> on peut décider à partir de combien d'acks dupliqués on retransmet) */
    for(int i=0; i<SIZE_TAB; i++){
      tab_ack[i] = 0;
    }
    char tab_segments[SIZE_TAB][SIZE_MESSAGE]; // tableau contenant tous les messages envoyés (pour pouvoir retransmettre sans relire dans le fichier)
    double cwnd = 1;  // taille de la fenêtre de transmission
    int window = 0; // nombre de paquets envoyés mais pas encore acquittés
    int retransmission = 0;  // booléen indiquant si on doit retransmettre (0 : non, 1 : oui)
    int nb_retransmissions = 0; // nombre de retransmissions pendant toute la durée d'envoi du fichier
    struct timeval send_time_tab[SIZE_TAB];  // tableau contenant les temps d'envoi de chaque paquet
    for(int i=0; i<SIZE_TAB; i++){
      send_time_tab[i] = (struct timeval){0};
    }
    int size;  // taille du message envoyé

    // CRÉATION DU THREAD POUR LA RÉCEPTION DES ACK
    pthread_t ack_thread;

    // définition des arguments à passer au thread ack_thread :
    struct arg_struct args;
    args.arg_a = 0;
    args.arg_client_addr = client_addr;
    args.arg_sockaddr_length = sockaddr_length;
    args.arg_socketDescriptorSet = socketDescriptorSet;
    args.arg_tab_ack = tab_ack;
    args.arg_window = &window;
    args.send_time_tab = send_time_tab;
    args.arg_retransmission = &retransmission;
    args.arg_cwnd = &cwnd;

    while(1){
      FD_ZERO(&socketDescriptorSet);
      FD_SET(serverSocket, &socketDescriptorSet);
      select(serverSocket+1, &socketDescriptorSet, NULL, NULL, NULL);
      if(FD_ISSET(serverSocket, &socketDescriptorSet)){  // connexion d'un nouveau client
        int a = acceptUDP(new_port, serverSocket, client_addr, sockaddr_length);
        new_port += 1;
        args.arg_a = a;

        int pid = fork(); // création d'un processus fils pour traiter ce client
        
        if(pid==0){  // processus fils
          printf("socket utile : %d\n", a);
          close(serverSocket);

          // réception du nom du fichier
          char filename[100] = "";
          int r = recvfrom(a, &filename, sizeof(filename), MSG_WAITALL, (struct sockaddr*)&client_addr, &sockaddr_length);
          if(r<0){perror(""); exit(0);}
          long total_size_file = 0;  // taille du fichier (pour calculer le débit à la fin)
          if(!strcmp(filename, "ef1663en.pdf")){ total_size_file = 2154227*8; }
          else if(!strcmp(filename, "projet2020.pdf")){ total_size_file = 103197*8; }

          // démarrage du timer
          struct timeval start;
          gettimeofday(&start,0);

          // ouverture du fichier à envoyer
          FILE* file = fopen(filename, "rb");
          printf("%s opened\n", filename);

          // création du thread pour la réception des ack
          if(pthread_create(&ack_thread, NULL, ack_routine_with_display, (void*) &args) != 0){
            perror("pthread_create() ack_thread:"); exit(0);
          }
          // initialisation du mutex
          if(pthread_mutex_init(&lock, NULL) != 0){
            perror("pthread_mutex_init():"); exit(0);
          }

          do{  // tant que le fichier n'a pas été envoyé en entier

            if(floor(cwnd)-window>0 && retransmission==0){
              if(fr==sizeof(file_data) || fr==0){
                // Construction du paquet à envoyer (découpage du fichier + n° de séquence)
                char message[SIZE_MESSAGE] = "";
                sprintf(message, "%06d", sequenceNB);  // put sequence number in message
                fr = fread(file_data, 1, sizeof(file_data), file);
                memcpy(message + 6, file_data, fr);  // put read data in message after sequence number
                memcpy(&tab_segments[sequenceNB%SIZE_TAB], &message, fr+6); // put read data in tab_segments at index corresponding to current sequence number

                // Envoi du paquet
                int s = sendto(a, message, fr+6, MSG_CONFIRM, (const struct sockaddr *) &client_addr, sockaddr_length);  // send message
                if(s<0){perror(""); exit(0);}
                printf("packet #%d sent\n", sequenceNB);
                gettimeofday(&send_time_tab[sequenceNB%SIZE_TAB],0);

                // Mise à jour sliding window
                pthread_mutex_lock(&lock);
                window = window + 1;  // Incrémentation du nombre de paquets envoyés mais pas encore acquittés
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
              nb_retransmissions += 1;
              window = 0;
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
                  pthread_mutex_lock(&lock);
                  window += 1;
                  pthread_mutex_unlock(&lock);
                }
              }

              retransmission = 0;
            }
          }while(tab_ack[(sequenceNB-1)%SIZE_TAB]<=2 || !feof(file));  // le dernier paquet envoyé n'a pas encore été acquitté et le paquet suivant n'est pas le dernier

          pthread_join(ack_thread, NULL);

          // envoi d'un message FIN pour dire que c'est la fin
          int s = sendto(a, "FIN", 4, MSG_CONFIRM, (const struct sockaddr*)&client_addr, sockaddr_length);
          if(s<0){perror(""); exit(0);}
          printf("FIN sent\n");

          /******************* FIN DE L'ENVOI DU FICHIER ************************/

            struct timeval end;
            gettimeofday(&end,0);
            struct timeval total_time;
            timersub(&end, &start, &total_time);
            printf("total time: %ld' %ld''\n", total_time.tv_sec, total_time.tv_usec);
            if(total_size_file != 0){
              double debit = (double)(total_size_file) / (double)((total_time.tv_sec * 1000000 + total_time.tv_usec));
              printf("débit : %f\n", debit);
            }
            printf("nombre de retransmissions: %d\n", nb_retransmissions);

            printf("pid %d is over\n", pid);
            exit(0);
        }

        else if(pid>0){  // processus père
          close(a);
        }

      }

      printf("En attente de connexion d'un nouveau client\n");

    }

    close(serverSocket);

    return 0;
  }


  
}
