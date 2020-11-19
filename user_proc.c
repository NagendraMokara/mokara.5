#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/time.h>
#include <semaphore.h>
#include <sys/msg.h>

#include "oss.h"

struct oss *ptr = NULL;
struct oss_user *usr = NULL;

static struct oss* setup_shm(){
	const key_t key = ftok(FTOK_PATH, FTOK_KEY);
	if(key == -1){
		perror("ftok");
		return NULL;
	}

	const int memid = shmget(key, sizeof(struct oss), 0);
  if(memid == -1){
  	perror("semget");
  	return NULL;
  }

	struct oss * addr = (struct oss *) shmat(memid, NULL, 0);
	if(addr == NULL){
		perror("shmat");
		return NULL;
	}

	return addr;
}

static int setup_msg(){
	const key_t key = ftok(FTOK_PATH, FTOK_MSG_KEY);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	int msgid = msgget(key, 0);
  if(msgid == -1){
  	perror("msgget");
  	return -1;
  }
	return msgid;
}

static int get_xx(const int argc, char * const argv[]){

	if(argc != 2){
		fprintf(stderr, "Usage: user xx\n");
		return -1;
	}

	int xx = atoi(argv[1]);
	if((xx < 0) | (xx >= SMAX)){
		fprintf(stderr, "Error: xx > 0!\n");
		return -1;
	}

	return xx;
}

//Setup our maximum resource descriptors need
static void setup_res(){
  int i;
  for(i=0; i < RMAX; i++){
    usr->res[i].val = 0;
		//generate max, up to the system max
    usr->res[i].max = rand() % ptr->res[i].max;
  }
}

static int group_val(struct resource_descriptor r[RMAX], int buf[RMAX]){
	int i, b=0;
	for(i=0; i < RMAX; i++){
		if(r[i].val < r[i].max){	//if we still don't have the whole resource
			buf[b++] = i;
		}
	}
	return b;
}

static int group_max(struct resource_descriptor r[RMAX], int buf[RMAX]){
	int i, b=0;
	for(i=0; i < RMAX; i++){
		if((r[i].val > 0) && (r[i].val == r[i].max) ){	//if we have the whole resourec we need
			buf[b++] = i;	//we can release it
		}
	}
	return b;
}

static int create_msg(struct request_message *msg){
	int b, buf[RMAX];

	//decide if we release or request
	msg->op = ((rand() % 100) < 70) ? REQUEST : RELEASE;

	if(msg->op == REQUEST){
		b = group_val(usr->res, buf);
		if(b == 0){
			return -1;	//nothing more to request, stop master loop
		}
		msg->rd  = buf[rand() % b];
		msg->val = 1;	//we request 1 unit at a time

	}else{
		b = group_max(usr->res, buf);
		if(b == 0){
			return 0;	//nothing more to release
		}
		msg->rd  = buf[rand() % b];
		msg->val = usr->res[msg->rd].val;	//we request 1 unit at a time
	}

	return 1;	//we have a request or release op
}

static int process_msg(struct request_message *msg){

	switch(msg->op){
		case REQUEST:
			usr->res[msg->rd].val += msg->val;
			break;

		case RELEASE:
			usr->res[msg->rd].val = 0;	//we have release everything
			usr->res[msg->rd].max = 0;	//nothing more to request
			break;

		case CANCEL:
			//do nothing
			break;

		case BLOCK:
			//shouldn't happen
			fprintf(stderr, "Error: Processing blocked message\n");
			break;

		default:
			//shouldn't happen
			fprintf(stderr, "Error: Processing unknown message\n");
			break;
	}
	return 0;
}

int main(const int argc, char * const argv[]){
	int term_flag = 0;
	struct timeval B, T, tv;	//B is for request, T is for terminate
	struct request_message msg;

	const int xx = get_xx(argc, argv);

	if(xx == -1){
		return EXIT_FAILURE;
	}

	ptr = setup_shm();
	if(ptr == NULL){
		return EXIT_FAILURE;
	}
	usr = &ptr->user_proc[xx];

	const int msgid = setup_msg();
  if(msgid == -1){
    return EXIT_FAILURE;
  }

	srand(getpid());


	//generate random increment for B
	timerclear(&B);
	tv.tv_sec = 0;
	tv.tv_usec = rand() % BVMAX;

	sem_wait(&ptr->sem);

	setup_res();

	timeradd(&ptr->shared_clock, &tv, &B);

	tv.tv_sec = 0;
	tv.tv_usec = 250;
	timeradd(&ptr->shared_clock, &tv, &T);

	while((ptr->stop_flag == 0) && (term_flag == 0)){

		//check if its time to do crate request
		if(timercmp(&ptr->shared_clock, &B, >=)){
			sem_post(&ptr->sem);

			//ask/drop a resource
			int err = create_msg(&msg);
			if(err < 0){	//if no more requests

				break;	//stop the loop
			}else if(err > 0){

				//send our request and wait for reply
				msg.mtype = getppid();	//set OSS, as receiver
				msg.user = xx;	//set our id, as sender
				msgsnd(msgid, (void*)&msg, sizeof(struct request_message), 0);
				msgrcv(msgid, (void*)&msg, sizeof(struct request_message), getpid(), 0);

				process_msg(&msg);
			}

			sem_wait(&ptr->sem);
			//set next B value
			tv.tv_sec = 0;
			tv.tv_usec = rand() % BVMAX;
			timeradd(&ptr->shared_clock, &tv, &B);
		}
		sem_post(&ptr->sem);


		sem_wait(&ptr->sem);
		//check if we should terminate
		if(timercmp(&ptr->shared_clock, &T, >=)){
			//15 % change to terminate
			term_flag = ((rand() % 100) < 15) ? 1 : 0;

			//update time for next term check
			tv.tv_sec = 0;
			tv.tv_usec = 250;
			timeradd(&ptr->shared_clock, &tv, &T);
		}
	}
	sem_post(&ptr->sem);

	//send final message
	msg.mtype = getppid();	//set OSS, as receiver
	msg.user = xx;	//set our id, as sender
	msg.rd  = 0;
	msg.val = 0;
	msg.op  = TERMINATING;
	msgsnd(msgid, (void*)&msg, REQUEST_MSG_SIZE, 0);

	shmdt(ptr);

	return EXIT_SUCCESS;
}
