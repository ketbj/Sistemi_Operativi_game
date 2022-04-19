#include <GL/glut.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>  
#include <netinet/in.h> 
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>

#include "utils.h"
#include "image.h"
#include "surface.h"
#include "world.h"
#include "vehicle.h"
#include "world_viewer.h"
#include "so_game_protocol.h"
#include "user_list.h"

typedef struct {
    struct sockaddr_in server_addr;
    int id;
} udp_args_t;

typedef struct {
  volatile int run;
  World* world;
} UpdaterArgs;

int window;
WorldViewer viewer;
World world;
Vehicle* vehicle;

pthread_mutex_t mutex;

char* server_address = SERVER_ADDRESS;

int run;
int udp_socket, tcp_socket;
pthread_t TCP_connection, UDP_sender, UDP_receiver, runner_thread;

int my_id;
Image* map_elevation;
Image* map_texture;
Image* my_texture_from_server;


//Libera la memoria
void freeMemory(void) {
  int ret;
  printf("Free Memory...\n");
  run = 0;
  ret = close(tcp_socket);
  ERROR_HELPER(ret, "Errore chiusura TCP socket");
  ret = close(udp_socket);
  ERROR_HELPER(ret, "Errore chiusura UDP socket");
  ret = pthread_cancel(UDP_sender);
  ERROR_HELPER(ret, "Errore cancellazione sender thread UDP");
  ret = pthread_cancel(UDP_receiver);
  ERROR_HELPER(ret, "Errore cancellazione receiver thread UDP");
  ret = pthread_cancel(runner_thread);
  ERROR_HELPER(ret, "Errore cancellazione runner thread");
  ret = pthread_cancel(TCP_connection);
  ERROR_HELPER(ret, "Errore cancellazione thread TCP");

  World_destroy(&world);
  Image_free(map_elevation);
  Image_free(map_texture);
  Image_free(my_texture_from_server);

  printf("Memoria liberata\n");
  //ERROR_HELPER(-1, "Stop\n");
  exit(1);
  return;
}

// Gestione segnali
void signalHandler(int signal){
  switch (signal) {
	case SIGHUP:
	  printf("\nSegnale SIGHUP verificato\n");	
	  break;
	case SIGINT:
	  printf("\nSegnale SIGINT verificato\n");
	  break;
	case SIGTERM:
      printf("\nSegnale SIGTERM verificato\n");
      break;
    case SIGQUIT:
      printf("\nSegnale SIGQUIT verificato\n");
      break;
	default:
	  printf("Segnale inaspettato: %d\n", signal);
	  return;
  }
  freeMemory();
}

/*---UDP---*/

// Mando la mia posizione tramite UDP
void* UDP_Sender(void* args){
  int ret;
  char buffer[BUFFER_SIZE];

  printf("%sSender thread avviato\n",UDP);

  udp_args_t* udp_args = (udp_args_t*) args; 
  int id = udp_args->id;                            //il mio ID

  struct sockaddr_in server_addr = udp_args->server_addr;    //indirizzo del server
  int sockaddr_len = sizeof(struct sockaddr_in);

  while(run) {
    // Il mio aggiornamento al server
    VehicleUpdatePacket* vehicle_packet = (VehicleUpdatePacket*) malloc(sizeof(VehicleUpdatePacket));

    PacketHeader header;                //creo un pacchetto di aggiornamento del mio veicolo
    header.type = VehicleUpdate;
    vehicle_packet->header = header;
    vehicle_packet->id = id;

    vehicle_packet->rotational_force = vehicle->rotational_force_update;         //inserisco i dati: theta(rotazione) e x,y(traslazione)
    vehicle_packet->translational_force = vehicle->translational_force_update;

    int buffer_size = Packet_serialize(buffer, &vehicle_packet->header);   //impacchetto il pacchetto nel buffer

    //Invio pacchetto
    ret = sendto(udp_socket, buffer, buffer_size , 0, (struct sockaddr*) &server_addr, (socklen_t) sockaddr_len);  
    ERROR_HELPER(ret,"Errore nell'invio degli aggiornamenti");

    usleep(35000);
  }

  pthread_exit(0);
}

//Ricezione degli aggiornamenti world
void* UDP_Receiver(void* args){
  int ret;
  int buffer_size = 0;           
  char buffer[BUFFER_SIZE];
                                            //devo ricevere le info degli altri clients
  printf("%sRicezione aggiornamenti\n",UDP);

  udp_args_t* udp_args = (udp_args_t*) args;

  struct sockaddr_in server_addr = udp_args->server_addr;
  socklen_t addrlen = sizeof(struct sockaddr_in);

  //Ricezione
  while ( (ret = recvfrom(udp_socket, buffer, BUFFER_SIZE, 0, (struct sockaddr*) &server_addr, &addrlen)) > 0){   
  
    buffer_size += ret;

    WorldUpdatePacket* world_update = (WorldUpdatePacket*) Packet_deserialize(buffer, buffer_size);
                                                                     //è un pacchetto che aggiorna il mio mondo
    // Aggiorno i movimenti dei veicoli
    for(int i=0; i < world_update->num_vehicles; i++) {           //per ogni veicolo devo aggiornare le sue posizioni
      ClientUpdate* client = &(world_update->updates[i]);

      Vehicle* client_vehicle = World_getVehicle(&world, client->id);

      if (client_vehicle == 0) continue;

      client_vehicle = World_getVehicle(&world, client->id);
      client_vehicle->x = client->x;
      client_vehicle->y = client->y;
      client_vehicle->theta = client->theta;
    }
  }
  printf("Server ucciso\n");
  pthread_exit(0);
}

// this is the updater threas that takes care of refreshing the agent position
// in your client it is not needed, but you will
// have to copy the x,y,theta fields from the world update packet
// to the vehicles having the corrsponding id
void* updater_thread(void* args_){
  UpdaterArgs* args=(UpdaterArgs*)args_;
  while(args->run){
    usleep(35000);
  }
  return 0;
}
/*---TCP---*/
void tcp_initialization(void){
    
    int ret;
    struct sockaddr_in server_addr = {0};

    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_HELPER(tcp_socket, "Errore creazione socket [TCP]\n");

    server_addr.sin_addr.s_addr = inet_addr(server_address);
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(TCP_PORT);

    ret = connect(tcp_socket, (struct sockaddr*) &server_addr, sizeof(struct sockaddr_in)); 
    ERROR_HELPER(ret, "Errore nella creazione della connessione [TCP]\n"); 
    printf("%sConnessione avvenuta col server [TCP]\n\n",TCP);
    return;
}
 

//Ricezione ID
void recv_ID(int* my_id) {
  int ret;
  char BUFFER[BUFFER_SIZE];

  printf("%sRichiesta di ID\n", TCP);
  
  PacketHeader header;                                      //creo pacchetto, ne alloco la memoria necessaria,  assegnandogli un header e 
  header.type = GetId;                                      //un Id momentaneo(-1) per comunicare se vuole un Id
  IdPacket* packet = (IdPacket*)malloc(sizeof(IdPacket));
  packet->header = header;
  packet->id = -1;

  int size = Packet_serialize(BUFFER, &packet->header);     //converto il GetId, cioè la richiesta al server di un Id,
                                                            //in un buffer e ne ottengo la lunghezza(size)
  //Invio richiesta tramite TCP
  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0 ) {               //invio l'intero buffer al server
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "Impossibile richiedere ID dal server");
  }

  //Ricevo il pacchetto_ID dal server
  while ( (size = recv(tcp_socket, BUFFER, BUFFER_SIZE, 0)) < 0 ) {         //ricevo ID dal server attraverso il buffer
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "Impossibile ricevere l'ID dal server");
  }

  IdPacket* id_recv = (IdPacket*) Packet_deserialize(BUFFER,size);       //spacchetto l' Id ricevuto inserendolo in un pacchetto
  *my_id = id_recv->id;                                                  //inserisco il nuovo Id alla locazione my_id

  printf("%sClient: %d\n", TCP, *my_id);
  printf("\n");
}

//Richiesta map_texture
void recv_Texture(Image** map_texture) {
  int ret;
  char BUFFER[BUFFER_SIZE];

  printf("%sRichiesta di map_texture\n", TCP);

  PacketHeader header;
  header.type = GetTexture;
  ImagePacket* packet = (ImagePacket*) malloc(sizeof(ImagePacket));       //creo il pacchetto con la richiesta di Texture
  packet->image = NULL;
  packet->header = header;

  int size = Packet_serialize(BUFFER, &packet->header);

  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0 ) {                 //invio la il pacchetto tramite buffer al server
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "Impossibile richiedere map_texture dal server");
  }

  int whole_packet_size = 0;           
  size = 0;

  while(1) {                                                                             //mi serve per controllare che tutto il pacchetto è
    while ( (size += recv(tcp_socket, BUFFER + size, BUFFER_SIZE - size, 0)) < 0 ) {     //ricevuto intero      
      if (errno == EINTR) continue;
      ERROR_HELPER(-1, "Impossibile ricevere map_texture dal server");
    }

    PacketHeader* aux = (PacketHeader*) BUFFER;            //prelevo la grandezza effettiva del pacchetto convertendo il buffer scritto
    whole_packet_size = aux->size;
    
    //Mancata ricezione intero pacchetto, allora ripeto
    if (size < whole_packet_size) continue;
    else break;
  }

  ImagePacket* received_packet = (ImagePacket*) Packet_deserialize(BUFFER,size);     //se ho ricevuto tutto il pacchetto, lo spacchetto nel formato di ricevitore texture
  *map_texture = received_packet->image;                                             //e lo inserisco in map_texture

  printf("%smap_texture ricevuta\n\n",TCP);
}

//Ricezione map_elevation
void recv_Elevation(Image** map_elevation) {
  int ret;                                         //è la texture dei muri 
  char BUFFER[BUFFER_SIZE];

  printf("%sRichiesta map_elevation\n",TCP);

  PacketHeader header;
  header.type = GetElevation;
  ImagePacket* packet = (ImagePacket*) malloc(sizeof(ImagePacket));
  packet->image = NULL;
  packet->header = header;

  int size = Packet_serialize(BUFFER, &packet->header);

  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0 ) {
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "Impossibile richiedere map_elevation al server");
  }

  int whole_packet_size = 0;
  size = 0;
  
  while(1) {

    while ( (size += recv(tcp_socket, BUFFER + size, BUFFER_SIZE - size, 0)) < 0 ) {
      if (errno == EINTR) continue;
      ERROR_HELPER(-1, "Impossibile ricevere map_elevation dal server");
    }

    // Dimensione totale del pacchetto da ricevere
    PacketHeader* aux = (PacketHeader*) BUFFER;
    whole_packet_size = aux->size;

    // Se la dimensione del pacchetto ricevuto è ancora minore della dimensione del pacchetto totale aspetta le altre parti
    if (size < whole_packet_size) continue;
    else break;
  }

  ImagePacket* elevation_packet = (ImagePacket*) Packet_deserialize(BUFFER, size);
  *map_elevation = elevation_packet->image;

  printf("%smap_elevation ricevuta\n\n",TCP);
}

//Invio della mia texture
void send_Texture(Image** my_texture, Image** my_texture_from_server) {
  int ret;
  char BUFFER[BUFFER_SIZE];

  printf("%sInvio della mia texture al server\n",TCP);

  PacketHeader header;
  header.type = PostTexture;
  ImagePacket* packet = (ImagePacket*)malloc(sizeof(ImagePacket));
  packet->image = *my_texture;                                          //questa volta invio la mia texture, non richiedo nulla
  packet->header = header; 

  int size = Packet_serialize(BUFFER, &packet->header);
  
  // Invia la texture del veicolo
  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0) {
    if (errno == EINTR) continue;
    ERROR_HELPER(ret, "Impossibile inviare la mia texture al server");
  }

  printf("%sTexture del veicolo inviata\n\n",TCP);

  *my_texture_from_server = packet->image;          
}


void* TCP_thread_routine(void* args) {

	printf("%sTCP_THREAD AVVIATO\n",TCP);

	while(run) {
		char BUFFER[BUFFER_SIZE];
		int actual_size = 0;
		int size = 0;
		int ret = 0;

		while(1) {     //sta in ascolto dal server 
            while ( (size += recv(tcp_socket, BUFFER + size, BUFFER_SIZE - size, 0)) < 0 ) { //ciò che ricevo da server lo metto nel BUFFER
                if (errno == EINTR) continue;
                ERROR_HELPER(-1, "Errore nella lettura socket dalla tcp_routine");
            }
            
            if(size==0){      //se il server si è disconnesso, stacco anche il client
               printf("\n!!! SERVER INTERROTTO INASPETTATAMENTE !!!\n");
               freeMemory();
               //ERROR_HELPER(-1, "Stop\n");
               
            }
            
            PacketHeader* aux = (PacketHeader*) BUFFER;      //casto solo per controllare che ho ricevuto corretto
            actual_size = aux->size;
          
            // Non ricevo tutto il pacchetto
            if (size < actual_size) continue;       //ripeto l'ascolto

            PacketHeader* head = (PacketHeader*) Packet_deserialize(BUFFER, actual_size);  //spacchetto per controllare cosa ho ricevuto
              
            if(head->type == NewConnection) {   //si è connesso un nuovo client
				
                //Un nuovo client si è connesso (server ha mandato una texture)
                ImagePacket* texture_back = (ImagePacket*) Packet_deserialize(BUFFER, actual_size);    
                Image* new_texture_user = texture_back->image;        //spacchetto nel formato di texture, per ottenere la texture del nuovo
                
                pthread_mutex_lock(&(mutex));     //utilizzo un semaforo per la sezione critica di creazione nuovo veicolo
                Vehicle* v = (Vehicle*) malloc(sizeof(Vehicle));
                Vehicle_init(v, &world, texture_back->id, new_texture_user);
                World_addVehicle(&world, v);
                pthread_mutex_unlock(&(mutex));
                
                printf("%sUtente %d è entrato in gioco...\n",TCP, texture_back->id);

				//Comunico al server la avvenuta ricezione
				PacketHeader* pack = (PacketHeader*)malloc(sizeof(PacketHeader));
                pack->type = NewClient;                     //notifica al server che ha aggiunto il nuovo client

                actual_size = Packet_serialize(BUFFER, pack);

                while ( (ret = send(tcp_socket, BUFFER,actual_size , 0)) < 0) {
                  if (errno == EINTR) continue;
                  ERROR_HELPER(ret, "Impossibile aggiungere client dal server\n");
                }
					
               //Esco dal ciclo cosi da rientrare nel while di run per continuare il prossimo ascolto
                break;
            }

              // Disconnessione utente
            else if(head->type == NewDisconnection) {
                IdPacket* id_disconnected = (IdPacket*) Packet_deserialize(BUFFER, size);
                
                // Elimino veicolo
                Vehicle* deleting_user = World_getVehicle(&world, id_disconnected->id);
                if(deleting_user) { //controllo se è nel mio mondo            //controllo se l'avevo precedentemente aggiunto, e lo elimino
                    pthread_mutex_lock(&(mutex));                              //tramite semaphoro
                    World_detachVehicle(&world, deleting_user);
                    Vehicle_destroy(deleting_user);
                    pthread_mutex_unlock(&(mutex));
                }

                printf("%sUtente %d disconnesso\n",TCP, id_disconnected->id);
				//Esco dal ciclo cosi da rientrare nel while di run per continuare il prossimo ascolto
                break;
            }
            //In teoria non dovrebbe essere eseguita questa linea di codice
            else {
                printf("%sErrore nel riconoscimento del pacchetto: %d...\n",TCP, head->type);
                continue;
              }
              size=0;
		}
	}
	printf("%s Tcp routine chiusa\n", TCP);

  pthread_exit(0);
}


void helloServer (int* my_id, Image** my_texture, Image** map_elevation, Image** map_texture, Image** my_texture_from_server){
  recv_ID(my_id);
  recv_Texture(map_texture);
  recv_Elevation(map_elevation);
  send_Texture(my_texture, my_texture_from_server);

  return;
}

/*---MAIN---*/
int main(int argc, char **argv) {
    printf("\n");
    printf("------[CLIENT]------\n");
    printf("Per muoverti nel mondo usa le frecce e per zoomare premi '+/-'\n");
    printf("Per uscire premi ESC\n");
    
  run = 0;

  if (argc<2) {
    printf("usage: %s <player texture>\n", argv[1]);
    exit(-1);
  }
  int ret;

  // Inizializzazione del signal handler
  struct sigaction signal_action;
  signal_action.sa_handler = signalHandler;
  signal_action.sa_flags = SA_RESTART;

  sigfillset(&signal_action.sa_mask);
  ret = sigaction(SIGHUP, &signal_action, NULL);
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGHUP");
  ret = sigaction(SIGINT, &signal_action, NULL);
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGINT!!!");
  ret = sigaction(SIGTERM, &signal_action, NULL);
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGTERM!!!");
  ret = sigaction(SIGQUIT, &signal_action, NULL);
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGQUIT!!!");

//printf("loading texture image from %s ... ", argv[1]);
  Image* my_texture = Image_load(argv[1]);
  if (my_texture) {
//    printf("Done! \n");
  } else {
//    printf("Fail! \n");
  }
  
  //Connessione in locale
  server_address = "127.0.0.1";
  
  // Apertura connessione TCP
  tcp_initialization();

  // Apertura connessione UDP
  
  struct sockaddr_in udp_server = {0};

  udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  ERROR_HELPER(udp_socket, "ERRORE CREAZIONE UDP SOCKET");       //implementare il tutto in una funzione

  udp_server.sin_addr.s_addr = inet_addr(server_address);
  udp_server.sin_family      = AF_INET;
  udp_server.sin_port        = htons(UDP_PORT);


  run = 1;

  // Richiede l'ID, la texture e l'elevation della mappa e invia la propria texture al server che la rimanda indietro
  helloServer(&my_id, &my_texture, &map_elevation, &map_texture, &my_texture_from_server);

  // Carica il mondo con le texture e elevation ricevute
  World_init(&world, map_elevation, map_texture, 0.5, 0.5, 0.5);

  // Aggiunge il nostro veicolo al mondo
  vehicle=(Vehicle*) malloc(sizeof(Vehicle));
  Vehicle_init(vehicle, &world, my_id, my_texture);
  World_addVehicle(&world, vehicle);
 
    
  //Struct contentente il pacchetto delle informazioni per l'UDP
  udp_args_t udp_args;
  udp_args.server_addr = udp_server;
  udp_args.id = my_id;

  //Preparo il thread di runner
  pthread_attr_t runner_attrs;
  UpdaterArgs runner_args={
    .run=1,
    .world=&world
  };
  
  pthread_attr_init(&runner_attrs); 
  runner_args.run=0;
  void* retval;
  
  
  //Creo i threads che gestiscono le 3 connessioni e il runner
  ret = pthread_create(&TCP_connection, NULL, TCP_thread_routine, NULL); 
  PTHREAD_ERROR_HELPER(ret, "Errore nella creazione del thread per la connessione TCP (client)");

  ret = pthread_create(&UDP_sender, NULL, UDP_Sender, &udp_args);
  PTHREAD_ERROR_HELPER(ret, "Errore nella creazione del sender_thread per la connessione UDP (client)");

  ret = pthread_create(&UDP_receiver, NULL, UDP_Receiver, &udp_args);
  PTHREAD_ERROR_HELPER(ret, "Errore nella creazione del receiver_thread per la connessione UDP (client)");

  ret = pthread_create(&runner_thread, &runner_attrs, updater_thread, &runner_args);
  PTHREAD_ERROR_HELPER(ret, "Errore nella creazione del thread runner");

  // Apre la schermata di gioco
  WorldViewer_runGlobal(&world, vehicle, &argc, argv);

  ret = pthread_join(TCP_connection, NULL);
  PTHREAD_ERROR_HELPER(ret, "Errore di join del thread per la connessione TCP");

  ret = pthread_join(UDP_sender, NULL);
  PTHREAD_ERROR_HELPER(ret, "Errore di join del sender_thread per la connessione UDP");

  ret = pthread_join(UDP_receiver, NULL);
  PTHREAD_ERROR_HELPER(ret, "Errore di join del receiver_thread per la connessione UDP");

  ret = pthread_join(runner_thread, &retval);
  PTHREAD_ERROR_HELPER(ret, "Impossibile avviare il Mondo\n");

  freeMemory();

  return 0;  
}
