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

// Struttura per gli argomenti dei threads in TCP
typedef struct {
  int client_desc;
  struct sockaddr_in client_addr;
  Image* surface_texture;
  Image* surface_elevation;
} tcp_args_t;

//Uso il mutex per evitare eventuali race condition
pthread_mutex_t mutex;

World world;
UserHead* users;

int run;

//Risorse per la connessione client/server
int tcp_socket, udp_socket;
pthread_t TCP_connection, UDP_sender_thread, UDP_receiver_thread;

//Info della mappa
Image* surface_elevation;  //verticale
Image* surface_texture;    //orizzontale

//Libera la memoria
void freeMemory(void) {
  int ret;
  printf("Free Memory...\n");
  run = 0;
  ret = close(tcp_socket);
  ERROR_HELPER(ret, "Errore chiusura TCP socket");
  ret = close(udp_socket);
  ERROR_HELPER(ret, "Errore chiusura UDP socket");
  ret = pthread_cancel(UDP_sender_thread);
  ERROR_HELPER(ret, "Errore nella cancellazione del sender thread UDP");
  ret = pthread_cancel(UDP_receiver_thread);
  ERROR_HELPER(ret, "Errore nella cancellazione del reicever thread UDP");
  ret = pthread_cancel(TCP_connection);
  ERROR_HELPER(ret, "Errore nella cancellazione thread TCP");

  World_destroy(&world);
  Image_free(surface_elevation);
  Image_free(surface_texture);

  printf("Memoria liberata\n");
  //ERROR_HELPER(-1, "Stop\n");
  exit(1);
  return;
}
    
//Gestione segnali
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
	  printf("\nSegnale inaspettato: %d\n", signal);
	  return;
  }
  freeMemory();
}

/*---TCP---*/

void tcp_initialization(void){
  
  int ret;  
  tcp_socket = socket(AF_INET , SOCK_STREAM , 0);
  ERROR_HELPER(tcp_socket, "[TCP] Errore nella creazione del TCP socket");

  struct sockaddr_in tcp_server_addr = {0};
  int sockaddr_len = sizeof(struct sockaddr_in);
  tcp_server_addr.sin_addr.s_addr = INADDR_ANY;
  tcp_server_addr.sin_family      = AF_INET;
  tcp_server_addr.sin_port        = htons(TCP_PORT);
  
  //Ottimizzazione del Socket per un determinato protocollo (TCP in questo caso)  //SO_REUSEADDR per restart dopo un crash
  int reuseaddr_opt_tcp = 1;
  ret = setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_opt_tcp, sizeof(reuseaddr_opt_tcp));
  ERROR_HELPER(ret, "Cannot set SO_REUSEADDR option");
  
  ret = bind(tcp_socket, (struct sockaddr*) &tcp_server_addr, sockaddr_len);
  ERROR_HELPER(ret, "[TCP] Errore nella bind sul TCP server socket");

  ret = listen(tcp_socket, 3);                                            //massimo numero in attesa
  ERROR_HELPER(ret, "[TCP] Errore nella listen sul TCP server socket");
  
  if (ret>=0) 
    printf("%sServer in ascolto sulla porta %d...\n", TCP, TCP_PORT);
}

//Gestisco il pacchetto
int TCP_packet (int client_tcp_socket, int id, char* buffer, Image* surface_elevation, Image* surface_texture, int len, User* user) {
  PacketHeader* header = (PacketHeader*) buffer;  // Pacchetto per controllo del tipo di richiesta

  if (header->type == NewClient){
  	printf("%sClient è stato avvertito\n",TCP);
  	return 1;
  }
	
  //Client richiede ID
  if (header->type == GetId) {

    printf("%sRichiesta dell'ID:%d\n",TCP, id);

    // Creo il pacchetto ID da mandare
    IdPacket* id_to_send = (IdPacket*) malloc(sizeof(IdPacket));

    PacketHeader header_send;
    header_send.type = GetId;
    
    id_to_send->header = header_send;
    id_to_send->id = id;  // Lo stesso id fornito da TCP_handler

    char buffer_send[BUFFER_SIZE];
    int pckt_length = Packet_serialize(buffer_send, &(id_to_send->header));    //impacchetto per inviare il nuovo id

    // Invio messaggio
    int bytes_sent = 0;
    int ret;
    while(bytes_sent < pckt_length){
      ret = send(client_tcp_socket, buffer_send + bytes_sent, pckt_length - bytes_sent, 0);
      if (ret==-1 && errno==EINTR) continue;
      ERROR_HELPER(ret, "Errore invio assegnazione ID");
      if (ret==0) break;
      bytes_sent += ret;
    }

    printf("%sID assegnato %d\n",TCP, id);

    return 1;
  }

  // Client richiede la surface_texture del mondo
  else if (header->type == GetTexture) {

    printf("%sUtente %d richiede la surface_texture\n",TCP, id);

    //Converto il pacchetto ricevuto in un ImagePacket
    ImagePacket* texture_request = (ImagePacket*) buffer;
    int id_request = texture_request->id;
    
    PacketHeader header_send;
    header_send.type = PostTexture;
 
	//Creo l' ImagePacket da mandare
    ImagePacket* texture_to_send = (ImagePacket*) malloc(sizeof(ImagePacket));
    texture_to_send->header = header_send;
    texture_to_send->id = id_request;
    texture_to_send->image = surface_texture;

    char buffer_send[BUFFER_SIZE];
    int pckt_length = Packet_serialize(buffer_send, &(texture_to_send->header));

    // Invio pacchetto
    int bytes_sent = 0;
    int ret;
    while(bytes_sent < pckt_length){
      ret = send(client_tcp_socket, buffer_send + bytes_sent, pckt_length - bytes_sent,0);
      if (ret==-1 && errno==EINTR) continue;
      ERROR_HELPER(ret, "Errore invio surface_texture");
      if (ret==0) break;
      bytes_sent += ret;
    }

    printf("%sSurface_texture inviata con successo %d\n", TCP, id);

    return 1;
  }

  // Client richiede surface_elevation
  else if (header->type == GetElevation) {

    printf("%sClient %d ha richiesto la surface_elevation\n",TCP, id);

    ImagePacket* elevation_request = (ImagePacket*) buffer;
    int id_request = elevation_request->id;
    
    PacketHeader header_send;
    header_send.type = PostElevation;
    
    ImagePacket* elevation_to_send = (ImagePacket*) malloc(sizeof(ImagePacket));
    elevation_to_send->header = header_send;
    elevation_to_send->id = id_request;
    elevation_to_send->image = surface_elevation;

    char buffer_send[BUFFER_SIZE];
    int pckt_length = Packet_serialize(buffer_send, &(elevation_to_send->header));

    // invio 
    int bytes_sent = 0;
    int ret;
    while(bytes_sent < pckt_length){
      ret = send(client_tcp_socket, buffer_send + bytes_sent, pckt_length - bytes_sent,0);
      if (ret==-1 && errno==EINTR) continue;
      ERROR_HELPER(ret, "Errore invio surface_elevation");
      if (ret==0) break;
      bytes_sent += ret;
    }

    printf("%sSurface_elevation inviata con successo %d\n",TCP, id);

    return 1;
  }

  // Ricevo la texture dal nuovo client
  else if (header->type == PostTexture) {
    int ret;
 
 	// Pacchetto ricevuto non è completo
    if (len < header->size) return -1;

    // Spacchettamento
    PacketHeader* received_header = Packet_deserialize(buffer, header->size);
    ImagePacket* received_texture = (ImagePacket*) received_header;

    printf("%sHo ricevuto la texture del veicolo, ID: %d\n",TCP, id);
    
    // Aggiungo il veicolo al mondo
    //MUTEX
    pthread_mutex_lock(&(mutex));
    
    //sezione critica
    Vehicle* new_vehicle = malloc(sizeof(Vehicle));
    Vehicle_init(new_vehicle, &world, id, received_texture->image);
    World_addVehicle(&world, new_vehicle);

    // Aggiungo l'utente alla mia lista utenti
    User_insert_last(users, user);
    
    //fine sezione critica
    pthread_mutex_unlock(&(mutex));
    
    // Devo notificare gli altri utenti della nuova connessione
    PacketHeader header_connect;
    header_connect.type = NewConnection;

    ImagePacket* user_connected = (ImagePacket*) malloc(sizeof(ImagePacket));
    user_connected->header = header_connect;
   	user_connected->id = user->id;
   	user_connected->image = received_texture->image;
    
    User* user_aux = users->first;
    
    char buffer_connection[BUFFER_SIZE];
    int packet_connection = Packet_serialize(buffer_connection, &(user_connected->header));
    
    int msg_length = 0;

    // Invio il pacchetto riguardante la nuova connessione
    while (user_aux != NULL) {
    	if (user_aux->id != user->id) {            
		    msg_length = 0;
		    while(msg_length < packet_connection){
		      ret = send(user_aux->id, buffer_connection + msg_length, packet_connection - msg_length,0);
		      if (ret==-1 && errno==EINTR) continue;
		      ERROR_HELPER(ret, "Errore nel notificare gli altri utenti della nuova connessione");
		      if (ret==0) break;
		      msg_length += ret;
		    }
	    }

	    user_aux = user_aux->next;
  	}

  	// Comunico al nuovo client le texture di tutti gli altri
  	user_aux = users->first;

  	while (user_aux != NULL) {
    	if (user_aux->id != user->id) {
    		// Invia al nuovo utente la connessione degli utenti già online
    		char buffer_connection_new[BUFFER_SIZE];
		    
		    PacketHeader header_new;
		    header_new.type = NewConnection;

		    ImagePacket* existing_vehicle = (ImagePacket*) malloc(sizeof(ImagePacket));
		    existing_vehicle->header = header_new;
		   	existing_vehicle->id = user_aux->id;
		   	existing_vehicle->image = World_getVehicle(&world, user_aux->id)->texture;

		    packet_connection = Packet_serialize(buffer_connection_new, &(existing_vehicle->header));

		    // Invio
		    msg_length = 0;
		    while(msg_length < packet_connection){
		      ret = send(user->id, buffer_connection_new + msg_length, packet_connection - msg_length,0);
		      if (ret==-1 && errno==EINTR) continue;
		      ERROR_HELPER(ret, "Errore invio info degli altri utenti al nuovo utente");
		      if (ret==0) break;
		      msg_length += ret;
		    }

		    // Attende la conferma del client
			while( (ret = recv(user->id, buffer_connection_new, 12, 0)) < 0){
				if (ret==-1 && errno == EINTR) continue;
				ERROR_HELPER(ret, "Errore ricezione conferma del nuovo client");
			}

	    }

	    user_aux = user_aux->next;
  	}

    return 1;
  }

  //Pacchetto sconosciuto
  else {
    printf("Pacchetto sconosciuto ricevuto da %d!!!\n", id);
  }

  return -1;
}

// Gestione client thread per aggiungere utenti alla lista 
// e gestione pacchetti tramite TCP_packet

void* TCP_client_thread (void* args){
  tcp_args_t* tcp_args = (tcp_args_t*) args;

  printf("%sGestione client.. %d\n",TCP, tcp_args->client_desc);

  int client_tcp_socket = tcp_args->client_desc;
  int msg_length = 0;
  int ret;
  char buffer_recv[BUFFER_SIZE];

  // Nuovo utente da aggiungere alla lista
  User* user = (User*) malloc(sizeof(User));
  
  user->id = client_tcp_socket;
  user->user_addr_tcp = tcp_args->client_addr;
  user->x = 0;
  user->y = 0;
  user->theta = 0;
  user->translational_force = 0;
  user->rotational_force = 0;

  // Ricevo e gestico il pacchetto (richiesta dell'id o della texture)
  int packet_length = BUFFER_SIZE;
  while(run) {
    while( (ret = recv(client_tcp_socket, buffer_recv + msg_length, packet_length - msg_length, 0)) < 0){
    	if (ret==-1 && errno == EINTR) continue;
    	ERROR_HELPER(ret, "Errore nel ricevere il pacchetto");
        usleep(200);
    }
    // Utente disconnesso
    if (ret == 0) {
      printf("%s----Client disconnesso, ID: %d ----\n", TCP, user->id);
      
      //MUTEX
      pthread_mutex_lock(&(mutex));
      
      //Sezione critica
      User_detach(users, user->id);
      Vehicle* deleted = World_getVehicle(&world, user->id);
      Vehicle* aux = World_detachVehicle(&world, deleted);
      Vehicle_destroy(aux);
      //fine sezione critica
      
      pthread_mutex_unlock(&(mutex));  

      // Comunico agli utenti che uno di essi si è disconnesso
      PacketHeader header_aux;
      header_aux.type = NewDisconnection;

      IdPacket* user_disconnected = (IdPacket*) malloc(sizeof(IdPacket));
      user_disconnected->header = header_aux;
      user_disconnected->id = client_tcp_socket;

      User* user_aux = users->first;

      while (user_aux != NULL) {              
        char buffer_disconnection[BUFFER_SIZE];
      	int packet_disconnection = Packet_serialize(buffer_disconnection, &(user_disconnected->header));

	    // Invio
	    msg_length = 0;
	    while(msg_length < packet_disconnection){
	      ret = send(user_aux->id, buffer_disconnection + msg_length, packet_disconnection - msg_length,0);
	      if (ret==-1 && errno==EINTR) continue;
	      ERROR_HELPER(ret, "Errore nell'invio della disconnessione agli altri client");
	      if (ret==0) break;
	      msg_length += ret;
	    }

        user_aux = user_aux->next;
      }

      break; //ricevuto e gestito disconnessione quindi non faccio più nulla
    }
    //Non sono avvenute disconnessioni e ho ricevuto i pacchetti
    msg_length += ret;

    //Avvio gestione pacchetti (per capire quale tipo ho ricevuto)
    ret = TCP_packet(client_tcp_socket, tcp_args->client_desc, buffer_recv, tcp_args->surface_elevation, tcp_args->surface_texture, msg_length, user);

    if (ret == 1)
      msg_length = 0;
  }

  // Chiusura thread
  pthread_exit(0);
}


// Gestione connessione TCP con il client (thread-part)
//In questo modo accetto connessioni multiple (multithreading)
void* TCP_handler(void* args){
	
  int ret;
  
  int tcp_client_desc;

  tcp_args_t* tcp_args = (tcp_args_t*) args;

  printf("%s Attendo connessione dai client\n",TCP);

  int sockaddr_len = sizeof(struct sockaddr_in);
  struct sockaddr_in client_addr;

  while( (tcp_client_desc = accept(tcp_socket, (struct sockaddr*)&client_addr, (socklen_t*) &sockaddr_len)) > 0) { //aspetta una connessione
    printf( " ----Nuovo client connesso----\n");                                            //sul tcp_socket
    printf("%sConnessione stabilita con %d...\n",TCP, tcp_client_desc);

    pthread_t client_thread;

    // argomenti client thread
    tcp_args_t tcp_args_aux;
    tcp_args_aux.client_desc = tcp_client_desc;
    tcp_args_aux.surface_texture = tcp_args->surface_texture;
    tcp_args_aux.surface_elevation = tcp_args->surface_elevation;
    tcp_args_aux.client_addr = client_addr;

    // Creo un nuovo thread per ogni client connesso
    ret = pthread_create(&client_thread, NULL, TCP_client_thread, &tcp_args_aux);
    PTHREAD_ERROR_HELPER(ret, "Errore creazione thread del TCP client");
  }
  ERROR_HELPER(tcp_client_desc, "Errore nell'accept della connessione TCP del client");

  printf("%s Non accetto piu connessioni\n",TCP);

  // Chiusura thread
  pthread_exit(0);
}


/*---UDP---*/

void udp_initialization(void){
  
  int ret;
  
  udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  ERROR_HELPER(udp_socket, "Errore nella creazione dell UDP socket");

  struct sockaddr_in udp_server_addr = {0};
  udp_server_addr.sin_addr.s_addr = INADDR_ANY;
  udp_server_addr.sin_family      = AF_INET;
  udp_server_addr.sin_port        = htons(UDP_PORT);
  
  //Ottimizzazione della socket per protocollo UDP  //SO_REUSEADDR per restart veloce dopo un crash
  int reuseaddr_opt_udp = 1;
  ret = setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_opt_udp, sizeof(reuseaddr_opt_udp));
  ERROR_HELPER(ret, "Cannot set SO_REUSEADDR option");

  ret = bind(udp_socket, (struct sockaddr*) &udp_server_addr, sizeof(udp_server_addr));
  ERROR_HELPER(ret, "Errore nella bind sulla socket UDP del server");

  printf("%sServer UDP partito\n", UDP);
}

// Gestione ricezione delle info dal client tramite UDP
void* UDP_receiver(void* args) {
  int ret;
  char buffer_recv[BUFFER_SIZE];
  
  struct sockaddr_in client_addr = {0};
  socklen_t addrlen = sizeof(struct sockaddr_in);
  
  printf("%sPronto a ricevere aggiornamenti\n",UDP);
  while (1) {
    if((ret = recvfrom(udp_socket, buffer_recv, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &addrlen)) > 0) {}
    ERROR_HELPER(ret, "Errore nella ricezione UDP dei pacchetti");

    PacketHeader* header = (PacketHeader*) buffer_recv;

    VehicleUpdatePacket* packet = (VehicleUpdatePacket*)Packet_deserialize(buffer_recv, header->size);
    User* user = User_find_id(users, packet->id);
    user->user_addr_udp = client_addr;

    if(!user) {
      printf("Impossibile trovare utente: %d\n", packet->id);
      pthread_exit(0);
    }
    
    // Aggiorno la posizione dell'utente nel mondo
    Vehicle* vehicle_aux = World_getVehicle(&world, user->id);

    vehicle_aux->translational_force_update = packet->translational_force;    //aggiorno dopo aver ricevuto tutte le info sul veicolo
    vehicle_aux->rotational_force_update = packet->rotational_force;

    //mutex per l'aggiornamento del mondo
    pthread_mutex_lock(&(mutex));
    World_update(&world);
    pthread_mutex_unlock(&(mutex));
  }

  pthread_exit(0);
}

// Gestione invio delle info al client tramite UDP
void* UDP_sender(void* args) {
  char buffer_send[BUFFER_SIZE];

  printf("%sPronto per l'invio degli aggiornamenti\n",UDP);
  while(run) {
    int size = users->size;

    // Se c'è almeno un utente connesso
    if (size > 0) {
        
      // Preparazione all'aggiornamento del mondo
      PacketHeader header;
      header.type = WorldUpdate;

      WorldUpdatePacket* world_update = (WorldUpdatePacket*) malloc(sizeof(WorldUpdatePacket));
      world_update->header = header;
      world_update->updates = (ClientUpdate*) malloc(sizeof(ClientUpdate) * size);
      world_update->num_vehicles = users->size;
      
      // mutex
      pthread_mutex_lock(&(mutex));
      
      //Sezione critica
      User* user = users->first;
      for (int i=0; i<size; i++) {
      	// Creo l'aggiornamento per l'i-esimo utente
        ClientUpdate* client = &(world_update->updates[i]);

        Vehicle* user_vehicle = World_getVehicle(&world, user->id);
        client->id = user->id;
        client->x = user_vehicle->x;
        client->y = user_vehicle->y;
        client->theta = user_vehicle->theta;

        user = user->next;
      }

      int size = Packet_serialize(buffer_send, &world_update->header);
      
	  //Fine sezione critica
      pthread_mutex_unlock(&(mutex));
      
      user = users->first;

      // Ogni utente connesso riceve queste info
      while (user != NULL) {
        if(user->user_addr_udp.sin_addr.s_addr != 0) {
          int ret = sendto(udp_socket, buffer_send, size, 0, (struct sockaddr*) &user->user_addr_udp, (socklen_t)sizeof(user->user_addr_udp));
          ERROR_HELPER(ret, "Errore nell'invio degli aggiornamenti");
        }
        user = user->next;
      }
    }
    usleep(12000);
  }

  pthread_exit(0);
}


/*---MAIN---*/
int main(int argc, char **argv) {
	
  run = 0;

  if (argc<3) {
    printf("usage: %s <elevation_image> <texture_image>\n", argv[1]);
    exit(-1);
  }
  
  printf(" -------Server in esecuzione-------\n");

  int ret;

  // Inizializzazione del signal handler
  struct sigaction signal_action;
  signal_action.sa_handler = signalHandler;
  signal_action.sa_flags = SA_RESTART;
                                                 // sigfillset inizializza i segnali puntati dal parametro
  sigfillset(&signal_action.sa_mask);            //.sa_mask: definisce l'insieme di segnali da bloccare mentre è in funzione il signalHandler
  ret = sigaction(SIGHUP, &signal_action, NULL);      //syscall
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGHUP");
  ret = sigaction(SIGINT, &signal_action, NULL);
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGINT");
  ret = sigaction(SIGTERM, &signal_action, NULL);
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGTERM");
  ret = sigaction(SIGQUIT, &signal_action, NULL);
  ERROR_HELPER(ret,"ERRORE GESTIONE SIGQUIT!!!");

  char* elevation_filename=argv[1];
  char* texture_filename=argv[2];
  
  // load the images
//printf("loading elevation image from %s ... ", elevation_filename);
  surface_elevation = Image_load(elevation_filename);
  if (surface_elevation) {
//   printf("Done! \n");
  } else {
//   printf("Fail! \n");
  }

//printf("loading texture image from %s ... ", texture_filename);
  surface_texture = Image_load(texture_filename);
  if (surface_texture) {
//   printf("Done! \n");
  } else {
//   printf("Fail! \n");
  }

  // Inizializzazione TCP e UDP
  tcp_initialization();
  
  udp_initialization();

  // Creo una nuova lista user
  users = (UserHead*) malloc(sizeof(UserHead));
  Users_init(users);

  // Costruisco il mondo
  World_init(&world, surface_elevation, surface_texture,  0.5, 0.5, 0.5);

  // Thread TCP args
  tcp_args_t tcp_args;
  tcp_args.surface_texture = surface_texture;
  tcp_args.surface_elevation = surface_elevation;

  // Mando in esecuzione
  run = 1;

  // Creazione thread
  ret = pthread_create(&TCP_connection, NULL, TCP_handler, &tcp_args);
  PTHREAD_ERROR_HELPER(ret, "Errore nella creazione del thread per la connessione TCP");

  ret = pthread_create(&UDP_sender_thread, NULL, UDP_sender, NULL);
  PTHREAD_ERROR_HELPER(ret, "Errore nella creazione del sender_thread per la connessione UDP");

  ret = pthread_create(&UDP_receiver_thread, NULL, UDP_receiver, NULL); 
  PTHREAD_ERROR_HELPER(ret, "Errore nella creazione del receiver_thread per la connessione UDP");

  // Chiusura thread
  ret = pthread_join(TCP_connection,NULL);
  ERROR_HELPER(ret,"Errore di join del thread per la connessione TCP");

  ret = pthread_join(UDP_sender_thread,NULL);
  ERROR_HELPER(ret,"Errore di join del sender_thread per la connessione UDP");

  ret = pthread_join(UDP_receiver_thread,NULL);
  ERROR_HELPER(ret,"Errore di join del receiver_thread per la connessione UDP");

  return 0;     
}
