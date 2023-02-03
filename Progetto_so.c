#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/sem.h>

#define SIZE sizeof(my_msg)-sizeof(long)
#define SIM_TIME 3
#define POP_SIZE 7000
#define NUM_SEMS 3

#define TEST_ERROR    if (errno) {dprintf(STDERR_FILENO,		\
					  "%s:%d: PID=%5d: Error %d (%s)\n", \
					  __FILE__,			\
					  __LINE__,			\
					  getpid(),			\
					  errno,			\
					  strerror(errno));}
//----------------------------------------------------------------------------------------------------------------------------------------------------------

struct std_utility{
	int turno; 
	int nof_invites;
	int max_rejects;
	int msg_sino;
};

struct shared_data{
	int matricola[POP_SIZE];
	int voto_ade[POP_SIZE];
	int voto_so[POP_SIZE];
	int nof_elems[POP_SIZE];
	int n_elem_grp[POP_SIZE];
	int id_gruppo[POP_SIZE];
	int gruppo_chiuso[POP_SIZE];
	int msgq_ids[POP_SIZE];
	int msgcoda_gruppos[POP_SIZE];
	int time_out;
};

struct message {
	long mtype;
	int voto_AdE;
	int nof_elems;
	int n_elem_grp;
	int queue_id;			//coda personale studente
	int grp_q_id;			//coda gruppo
	int accetta_rifiuta;	//1 == accetta, inizializzato a 0
};

int turno1_queue_id, turno2_queue_id, sem_id, shm_id, parent_pid;	//id strutture ipc globali per deallocazione
int randomNofElem();
struct std_utility dati_stud(struct shared_data * my_student, int i , struct std_utility utility);
void handle_signal(int signal);
int calcola_voto_SO(struct shared_data * my_student);
void stampa_stats_AdE(struct shared_data * my_student);
void stampa_stats_SO(struct shared_data * my_student);
int accetta(struct shared_data * my_student, struct message my_msg,int coda_gruppo, int i);
struct std_utility invia_messaggio(struct shared_data * my_student, struct std_utility utility, int i, int coda_turno, int coda_gruppo, int student_queue);
void chiudi_gruppo(struct shared_data * my_student, int i, int coda_gruppo);
struct std_utility rifiuta(struct message my_msg,struct std_utility utility);
void print_stats(struct shared_data * my_student, int i);
//----------------------------------------------------------------------------------------------------------------------------------------------------------

int main(){
	int i, status, student_queue, inc_voto;
	int delta_voti, lonewolfs, coda_turno , coda_gruppo;
	unsigned int my_pid;
	sigset_t  my_mask;
	struct std_utility utility;
	struct sembuf sops;
	struct message my_msg;
	struct sigaction sa;
	struct shared_data * my_student;
	
	//creazione chiavi
	//----------------------
	sem_id = semget(IPC_PRIVATE, NUM_SEMS, 0600);								// Chiave Semaforo
	turno2_queue_id = msgget(IPC_PRIVATE, IPC_CREAT | IPC_EXCL | 0600);			// Chiave MSGQ turno 2
	turno1_queue_id = msgget(IPC_PRIVATE, IPC_CREAT | IPC_EXCL | 0600);			// Chiave MSGQ turno 1
	shm_id = shmget(IPC_PRIVATE, sizeof(*my_student), 0600); 					// crea memoria condivisa 
	TEST_ERROR;
	//----------------------
	my_student = shmat(shm_id, NULL, 0); 		//attach della mem.condivisa al puntatore 
	//----------------------
	parent_pid = getpid();
	
	
	sa.sa_handler = handle_signal;
	sa.sa_flags = 0; 						//No special behaviour 
	// Create an empty mask
	sigemptyset(&my_mask);        			// Do not mask any signal
	sa.sa_mask = my_mask;
	//insatallazzione signal handler
	if (sigaction(SIGINT, &sa, NULL) == -1)
			fprintf(stderr, "Cannot set a user-defined handler for Signal #%d: %s\n",i, strsignal(i));
	if (sigaction(SIGALRM, &sa, NULL) == -1)
			fprintf(stderr, "Cannot set a user-defined handler for Signal #%d: %s\n",i, strsignal(i));
	if (sigaction(SIGUSR1, &sa, NULL) == -1)
			fprintf(stderr, "Cannot set a user-defined handler for Signal #%d: %s\n",i, strsignal(i));

	/*creazione semafori*/
	semctl(sem_id, 0, SETVAL, 0);
	semctl(sem_id, 1, SETVAL, 0);
	semctl(sem_id, 2, SETVAL, 0);

	sops.sem_num = 0;
	sops.sem_flg = 0;
	sops.sem_op = POP_SIZE;
    semop(sem_id, &sops, 1);
	
	sops.sem_num = 1;
	sops.sem_flg = 0;
	sops.sem_op = POP_SIZE+1;
    semop(sem_id, &sops, 1);
	
	sops.sem_num = 2;
	sops.sem_flg = 0;
	sops.sem_op = POP_SIZE+1;
    semop(sem_id, &sops, 1);
	
	
	my_student->time_out = 1;
	inc_voto= 11;
	//----------------------
	//fork creazione figli
	for(i = 0; i<POP_SIZE; i++){
		switch(fork()){
			case -1:
				// Handle error 
				fprintf(stderr,"Error #%03d: %s\n", errno, strerror(errno));
				break;
//---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
			//codice processi student
			case 0:
				fprintf(stderr,"CHILD PID: %d INITIALIZED\n",getpid());
				//----------------------
				// MSGQ Student
				student_queue = msgget(IPC_PRIVATE, IPC_CREAT | IPC_EXCL | 0600);	
				coda_gruppo = msgget(IPC_PRIVATE, IPC_CREAT | IPC_EXCL | 0600);
				//creazione dati student
				
				my_student->msgq_ids[i] = student_queue;
				my_student->msgcoda_gruppos[i] = coda_gruppo;
				
				utility = dati_stud(my_student, i, utility);
				
				//mi aggancio alla coda del mio turno 
				if(utility.turno == 1){coda_turno = turno1_queue_id;}
				else{coda_turno = turno2_queue_id;}
				//----------------------
				//Semaforo wait for zero
				//Aspetto che tutti i dati di tutti i processi siano caricati
				sops.sem_num = 0;
				sops.sem_op = -1;
				semop(sem_id, &sops, 1);
				sops.sem_op = 0;
				semop(sem_id, &sops, 1);
				//simulation start
			//---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
				//ciclo inviti
				while(my_student->time_out == 1){
					/*
					*	ricevo messaggi dalla coda del turno dello studente, condivisa da tutti i processi studente dello stesso turno.
					*	posso ricevere qualunque mtype di messaggio escluso mtype == mio pid per evetare di invitare me stesso.
					*	metto la flag IPC_NOWAIT perchè se non ci sono inviti nella coda la msgrcv fallisce con errno == ENOMSG, 
					*	il che mi permette di uscire e quindi posso procedere a invitare.
					*	se ricevo un messaggio procedo a verificare se ho un incremento di voto desiderato.
					*	l'incremento di voto diminuisce ad ogni rifiuto, e accetto solo se max rejects == 0 oppure la differenza tra il 
					*	mio voto e quello dell'invitante è maggiore dell'incremento voto desiderato.
					*/
					if(msgrcv(coda_turno, &my_msg, SIZE, getpid(), MSG_EXCEPT|IPC_NOWAIT) < 0){
						if(errno == ENOMSG){}
						else{
							TEST_ERROR;
							break;
						}
					}else{
						//----------------------
						//codice ricezione messaggio
						//printf("%ld INVITA %d\n", my_msg.mtype, getpid());
						if(my_student->id_gruppo[i] != -1 || utility.msg_sino == 0){
							utility = rifiuta(my_msg, utility);
						}else{
							delta_voti = ((my_msg.voto_AdE) - (my_student->voto_ade[i]));
							if((my_msg.nof_elems != my_student->nof_elems[i])){delta_voti = delta_voti - 3;}
							if(delta_voti>inc_voto||utility.max_rejects == 0){
								coda_gruppo = accetta(my_student, my_msg, coda_gruppo, i);
								break;
							}else{
								if(inc_voto > 0){inc_voto--;}
								else{
									if((my_msg.voto_AdE) >= 18){
										coda_gruppo = accetta(my_student, my_msg, coda_gruppo, i);
										break;
									}
								}
								utility =rifiuta(my_msg, utility);
							}
						}
					}
					//fine ricezione invito
			//---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
					// invita
					if(utility.nof_invites > 0 && utility.msg_sino == 1){utility = invia_messaggio(my_student, utility, i , coda_turno, coda_gruppo, student_queue);}
					
					//se sono il leader del gruppo e ho finito gli inviti chiudo il gruppo
					if(utility.nof_invites == 0 && my_student->id_gruppo[i] == getpid() && utility.msg_sino == 1){
						chiudi_gruppo(my_student,i, coda_gruppo );
						break;
					}
					
			//---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
					
					/*
					*	se ho mandato un invito nella coda del turno aspetto la risposta dall'altro studente nella msgrcv
					*	se ricevo un accettazione aumento il numero di elementi e controllo che l'obiettivo nof elem sia stato raggiunto
					*	se è stato raggiunto chiudo il gruppo e mando un messaggio agli altri studenti sulla coda del gruppo contenente n_elem ed esco.
					*	se riceve un rifiuto o non ha raggiunto nof elem sblocca msg_sino e gli permette di inviare altri messaggi.
					*	se scatta sim time mentre il processo è in attesa della risposta, il processo riceve un segnale SIGUSR1 e la msgrcv
					*	fallisce ritornando -1 e settando errno == EINTR permettendo al processo di sbloccarsi e uscire
					*/
					if(utility.msg_sino == 0){
						if(msgrcv(student_queue, &my_msg, SIZE, 0, 0) < 0){ 
							if(errno == EINTR){}
							else{
								TEST_ERROR;
								break;
							}
						}else{
							if(my_msg.accetta_rifiuta == 1){
								//printf("%ld ACCETTA L'INVITO DI %d\n", my_msg.mtype, getpid());
								my_student->id_gruppo[i] = getpid();
								my_student->n_elem_grp[i]++;
								if(my_student->n_elem_grp[i] == my_student->nof_elems[i]){
									chiudi_gruppo(my_student,i, coda_gruppo );
									break;
								}else{utility.msg_sino = 1;}
							}else{
								//printf("%ld RIFIUTA L'INVITO DI %d\n", my_msg.mtype, getpid());
								utility.msg_sino = 1;
							}
						}
					}
					//----------------------
					//fine ciclo infinito 1
				}

			//---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
				//ricezione messaggio chiusura gruppo
				/*
				*	aspetto il messaggio di chiusura gruppo dal leader sulla coda del gruppo
				*	se scatta sim time mentre il processo è in attesa della risposta, il processo riceve un segnale SIGUSR1 e la msgrcv
				*	fallisce ritornando -1 e settando errno == EINTR permettendo al processo di sbloccarsi e uscire
				*/
				if(my_student->gruppo_chiuso[i] == 0){
					while(my_student->time_out == 1){
						if(msgrcv(coda_gruppo, &my_msg, SIZE, 0, 0) < 0){
							if(errno == EINTR){}
							else{
								TEST_ERROR;
								break;
							}
						}else{
								printf("PID: %d CHIUSURA GRUPPO %d \n",getpid(), my_student->id_gruppo[i]);
								my_student->gruppo_chiuso[i] = 1;
								my_student->n_elem_grp[i] = my_msg.n_elem_grp;
								break;
						}
					}
				}
				//fine scambio messaggi
				//arrivo qui a sim time scaduto o gruppo formato e chiuso.
		//---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
				//attendo che tutti i processi arrivino qui
				sops.sem_num = 1;
				sops.sem_op = -1;
				semop(sem_id, &sops, 1);
				sops.sem_op = 0;
				semop(sem_id, &sops, 1);
				// aspetto che il padre calcoli i voti di SO
				sops.sem_num = 2;
				sops.sem_op = -1;
				semop(sem_id, &sops, 1);
				sops.sem_op = 0;
				semop(sem_id, &sops, 1);
				//stampo i dati dello studente
				print_stats(my_student, i);
				
				//printf("PID: %d TERMINATED \n", getpid());
				//disalloco le code dello studente
				msgctl(student_queue, IPC_RMID, NULL);
				msgctl(coda_gruppo, IPC_RMID, NULL);
				//esco
				exit(0);
//---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
				//fine codice student

			default:
				break;
		}//FINE switch figli
	}//FINE Fork figli
	//----------------------
	

	//------------------------
	//Sblocco semaforo wait for zero
	//aspetto che tutti i processi siano pronti
	sops.sem_num = 0;
	sops.sem_op = 0;
	semop(sem_id, &sops, 1);
	//inizio simulazione
	//parte sim time
	alarm(SIM_TIME);


	//fine creazione gruppi
	sops.sem_num = 1;
	sops.sem_op = -1;
	semop(sem_id, &sops, 1);
	sops.sem_op = 0;
	semop(sem_id, &sops, 1);
	//calcolo i voti di SO
	lonewolfs = calcola_voto_SO(my_student);
	
	sops.sem_num = 2;
	sops.sem_op = -1;
	semop(sem_id, &sops, 1);
	sops.sem_op = 0;
	semop(sem_id, &sops, 1);
	
	
	
	//aspetto che tutti i processi siano usciti
	while ((my_pid = wait(&status)) != -1) {}
	//stampo le statistiche di SO e AdE
	stampa_stats_AdE(my_student);
	stampa_stats_SO(my_student);
	
	//disalloco le strutture del padre
	
	while (shmctl(shm_id, IPC_RMID, NULL)){}
	msgctl(turno1_queue_id, IPC_RMID, NULL);
	msgctl(turno2_queue_id, IPC_RMID, NULL);
	semctl(sem_id, 0, IPC_RMID);
	
	printf("PARENT TERMINATED \n");
	exit(EXIT_SUCCESS);

}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
// crea dati studenti - ritorn my_std
//prendo nof_invites e max_rejects dalle ultime due righe di opt.conf
struct std_utility dati_stud(struct shared_data * my_student, int i ,struct std_utility utility){
	FILE *conf;
	int j;
	int arrayFile[4];
	conf = fopen("opt.conf","r");
	for(j = 0; j<4; j++){fscanf(conf, "%d", &arrayFile[j]);}
	fclose(conf);
	srand(getpid());
	my_student->matricola[i] = getpid();
	my_student->voto_ade[i] = 18 + rand()%13;
	my_student->nof_elems[i] = randomNofElem();
	my_student->id_gruppo[i] = -1;
	my_student->voto_so[i] = 0;
	my_student->gruppo_chiuso[i] = 0;
	my_student->n_elem_grp[i] = 1;
	utility.nof_invites = arrayFile[2];
	utility.max_rejects = arrayFile[3];
	utility.msg_sino = 1;
	if(getpid()%2 == 0){utility.turno = 2;}
	else{utility.turno = 1;}
	return utility;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
//genera nof elem random tra 2 3 e 4
int randomNofElem(){
	int arrayRandom[2],random;
	double fpercent,spercent,val;
	FILE *conf;
	conf = fopen("opt.conf","r");
	for(int i = 0; i<2; i++)
		fscanf(conf, "%d", &arrayRandom[i]);

	fpercent = arrayRandom[0]/100.0;
	spercent = arrayRandom[1]/100.0;
	val = (double)rand() / RAND_MAX;

	if (val < fpercent) {random = 2;}
	else if (val < spercent+fpercent){random = 3;}
	else{random = 4;}
	fclose(conf);

	return random;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
/*
 *	gestisce i segnali SIGALARM, SIGUSR1, SIGINT.
 *	SIGALARM imposta a zero time_out e manda un segnale SIGUSR1 a tutti i processi studenti
 *	SIGINT disalloca le strutture IPC
*/
void handle_signal(int segnale) {
	int i, j;
	struct shared_data * my_student;
	my_student = shmat(shm_id, NULL, 0);
	switch(segnale){

		case SIGALRM:
		my_student->time_out =0;
		for(i = 0; i<POP_SIZE; i++){
			if(kill(my_student->matricola[i], SIGUSR1)<0){TEST_ERROR;}
		}
			break;

		case SIGUSR1:
			break;
			
		case SIGINT:
			if(getpid() == parent_pid){
				for(i = 0; i<POP_SIZE;i++){
					msgctl(my_student->msgq_ids[i], IPC_RMID, NULL);
					msgctl(my_student->msgcoda_gruppos[i], IPC_RMID, NULL);
				}
				while (shmctl(shm_id, IPC_RMID, NULL)){ TEST_ERROR; }
				msgctl(turno1_queue_id, IPC_RMID, NULL);
				msgctl(turno2_queue_id, IPC_RMID, NULL);
				semctl(sem_id, 0, IPC_RMID);
			}
			exit(0);
	}
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
/*
*	calcola i voti di SO.
*	assegna voto 0 per chi non è in un gruppo o non è in un gruppo chiuso.
*	cerca all'interno dell'array il leader di un gruppo che non è ancora salvato in tmp, lo salva
*	e inizia le operazioni per assegnare il voto.
*	ritorna il numero di studenti che non sono in un gruppo o un in gruppo chiuso.
*/
 int calcola_voto_SO(struct shared_data * my_student){
	int i;
	int j,k,h,x,y,n_stud,lonewolfs;
	int boolean;
	int tmp[POP_SIZE];
	int tmpvoto;
	lonewolfs=0;
	for (i=0 ; i< POP_SIZE ; i++){
		tmpvoto = 0;
		if(my_student->id_gruppo[i] == -1 || my_student->gruppo_chiuso[i] == 0){
			my_student->voto_so[i] = 0;
			lonewolfs++;
		}else{
			boolean = 0;
			k = 0;
			while(k<POP_SIZE&&boolean == 0){
				if (tmp[k] == my_student->matricola[i]){boolean = 1;}
				k++;
			}
			if (boolean == 0){
				n_stud = 0;
				if (my_student->id_gruppo[i] == my_student->matricola[i]){
					tmp[k-1] = my_student->matricola[i];
					tmpvoto = my_student->voto_ade[i];
					j = 0;
					x = 0;
					while(j<POP_SIZE&& x<my_student->n_elem_grp[i]){
						if (my_student->matricola[i] == my_student->id_gruppo[j]){
							if(my_student->voto_ade[j]>=tmpvoto){tmpvoto = my_student->voto_ade[j];}
							x++;
						}
						j++;
					}
					y = 0;
					h = 0;
					while(y<POP_SIZE && h<my_student->n_elem_grp[i]){
							if (my_student->matricola[i] == my_student->id_gruppo[y]){
								if(my_student->nof_elems[y]== my_student->n_elem_grp[y]){my_student->voto_so[y] = tmpvoto;}
								else{my_student->voto_so[y] = tmpvoto-3;}
								h++;
								//print_stats(my_student, y);
							}
						y++;
					}
				}
			}
		}
	}
	return lonewolfs;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
// stampa il numero di studenti che hanno preso voti da 18 a 30 in AdE
void stampa_stats_AdE(struct shared_data * my_student){
	int media, tmp,i,voto;
	media = 0;
	printf("voti architettura\n");
	for(voto = 18; voto<=30; voto++){
		tmp = 0;
		for(i = 0; i<POP_SIZE; i++){
			if (my_student->voto_ade[i] == voto){tmp++;}
		}
		printf("numero voti %d : %d\n", voto, tmp);
	}
	for (i = 0; i<POP_SIZE; i++){media += my_student->voto_ade[i];}
	printf("media : %d\n", media/POP_SIZE);
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
//stampa il numero di studenti che hanno preso voti da 18 a 30 in SO e il numero di bocciati
void stampa_stats_SO(struct shared_data * my_student){
	int elem;
	int media, tmp,j,voto,i;
	voto = 0;
	media = 0;
	elem = POP_SIZE;
	printf("voti SO\n");
	for (i=0; i<POP_SIZE;i++){
			if (my_student->voto_so[i]<18){
					voto++;
			}
	}
	printf("numero bocciati : %d\n", voto);
	for(voto = 18; voto<=30; voto++){
			tmp = 0;
			for(i = 0; i<POP_SIZE; i++){
				if (my_student->voto_so[i] == voto){
					tmp++;
				}

			}
			printf("numero voti %d : %d\n", voto, tmp);
	}
	for (i = 0; i<POP_SIZE; i++){
			media += my_student->voto_so[i];
	}
	printf("media : %d\n", media/POP_SIZE);
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
//crea un messaggio, disalloca la coda gruppo dello studente e la sostituisce con quella del leader del gruppo, e invia il messaggio
//ritorna la nuova coda gruppo
int accetta(struct shared_data * my_student, struct message my_msg,int coda_gruppo, int i){
	int sender_queue;
	my_student->id_gruppo[i] = my_msg.mtype;
	msgctl(coda_gruppo, IPC_RMID, NULL);
	coda_gruppo = my_msg.grp_q_id;
	sender_queue = my_msg.queue_id;
	my_msg.accetta_rifiuta = 1;
	my_msg.mtype = getpid();
	if(msgsnd(sender_queue, &my_msg, SIZE,0)<0){TEST_ERROR;}
	return coda_gruppo;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
//crea e invia un messaggio sulla coda del turno
//ritorna la struct std_utility modificata
struct std_utility invia_messaggio(struct shared_data * my_student, struct std_utility utility, int i, int coda_turno, int coda_gruppo, int student_queue){
	struct message my_msg;
	my_msg.mtype = getpid();
	my_msg.voto_AdE = my_student->voto_ade[i];
	my_msg.nof_elems = my_student->nof_elems[i];
	my_msg.grp_q_id = coda_gruppo;
	my_msg.queue_id = student_queue;
	if(msgsnd(coda_turno, &my_msg, SIZE,0) < 0){TEST_ERROR;}
	utility.nof_invites--;
	utility.msg_sino = 0;
	return utility;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
//crea il messaggio di chiusura e ne invia n_elem_grp nella coda del gruppo
//setta il gruppo come chiuso
void chiudi_gruppo(struct shared_data * my_student, int i, int coda_gruppo){
	struct message my_msg;
	int j;
	printf("PID: %d CHIUDE IL GRUPPO %d\n", getpid(), my_student->id_gruppo[i]);
	my_msg.mtype = getpid();
	my_msg.n_elem_grp = my_student->n_elem_grp[i];
	for(j = 0; j<my_student->n_elem_grp[i]; j++){
		if(msgsnd(coda_gruppo, &my_msg, SIZE,0)<0){TEST_ERROR;}
	}
	my_student->gruppo_chiuso[i] = 1;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
//crea un messaggio e lo invia nella coda dello studente che ha mandato l'invito
//ritorna la struct std_utility modificata
struct std_utility rifiuta(struct message my_msg,struct std_utility utility){
	int sender_queue;
	utility.max_rejects--;
	sender_queue = my_msg.queue_id;
	my_msg.mtype = getpid();
	my_msg.accetta_rifiuta = -1;
	if(msgsnd(sender_queue, &my_msg, SIZE,0)<0){TEST_ERROR;}
	return utility;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------
//stampa i dati dello studente
void print_stats(struct shared_data * my_student, int i){
	fprintf(stdout, "matricola: %d || voto_AdE: %d || voto_SO:%d || nof_elems: %d || n_elem_grp: %d || id_grp: %d || grp chiuso: %d\n", \
			my_student->matricola[i],\
			my_student->voto_ade[i],\
			my_student->voto_so[i],\
			my_student->nof_elems[i]\
			,my_student->nof_elems[i]\
			,my_student->id_gruppo[i]\
			, my_student->gruppo_chiuso[i]);
}

