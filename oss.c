#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <sys/shm.h>
#include <time.h>
#include <semaphore.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#include "oss.h"

struct oss_stat {
  int requests;

  int grants;
  int blocks;
  int releases;

  int unblocks;
  int cancelled;

  int deadlocks;
  int deadlock_checks;
};

static int n = 0, nmax = 40;  //processes
static int s = 0, smax = SMAX;  //running processes
static int e = 0, emax = 0;   //exited processes
static int        tmax = 5;   //runtime
static int v = 0; //verbose

static struct oss_stat ostat;

static int memid = -1;  //shared memory
static int msgid = -1;  //message queue

static struct oss * ptr = NULL;

//this is our queue of blocked message requests
static struct request_message bmsg[SMAX];  //blocked messages
static unsigned int bmsg_len = 0;

static int loop_flag = 1;

static void wait_children(){
  pid_t pid;
  int status;

  while((pid = waitpid(-1, &status, WNOHANG)) > 0){

    //clear from pids[]
    int i;
    for(i=0; i < SMAX; i++){
      if(ptr->user_proc[i].pid == pid){
        ptr->user_proc[i].pid = 0;
      }
    }

    --s;
    if(++e >= emax){  //if all exited
      loop_flag = 0;  //stop the loop
    }

    //just in case, if a process is waiting on the timer
    /*sem_wait(&ptr->sem);
    ptr->shared_clock.tv_sec++;
    sem_post(&ptr->sem);*/
  }
}

static int term_children(){
  int i;
  struct request_message msg;


  sem_wait(&ptr->sem);
  ptr->stop_flag = 1;
  sem_post(&ptr->sem);

  for(i=0; i < s; ++i){
    /*if(ptr->user_proc[i].pid > 0){
      kill(ptr->user_proc[i].pid, SIGTERM);
    }*/
    msg.mtype = ptr->user_proc[i].pid;
    msg.op = CANCEL;
    msgsnd(msgid, (void*)&msg, REQUEST_MSG_SIZE, 0);
  }

  while(s > 0){
    wait_children();
  }

  return 0;
}

static void sig_handler(const int sig){

  switch(sig){
    case SIGINT:
    case SIGALRM:
      loop_flag = 0;

      sem_wait(&ptr->sem);
      ptr->stop_flag = 1;
      sem_post(&ptr->sem);
      break;

    case SIGCHLD:
      wait_children();
      break;

    default:
      break;
  }
}

static void clean_shm(){

  sem_destroy(&ptr->sem);
  msgctl(msgid, IPC_RMID, NULL);
  shmctl(memid, IPC_RMID, NULL);
  shmdt(ptr);
}

static int find_free_oss_user(){
  int i;
  for(i=0; i < SMAX; i++){
    if(ptr->user_proc[i].pid == 0){
      return i;
    }
  }
  return -1;
}

static int exec_user(){
  char xx[20];
  const int user_index = find_free_oss_user();
  if(user_index < 0){
    //this should not happen, since we exec only when s < smax
    fprintf(stderr, "OSS: user_index == -1\n");
    return -1;
  }

  const pid_t pid = fork();
  switch(pid){
    case -1:
      perror("fork");
      break;

    case 0:
      snprintf(xx, 20, "%i", user_index);

      setpgid(getpid(), getppid());
      execl("user_proc", "user_proc", xx, NULL);

      perror("execl");
      exit(EXIT_FAILURE);
      break;

    default:
      //save process pid
      printf("OSS: Started P%d at time %ld:%ld\n",
        n, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      ptr->user_proc[user_index].pid = pid;
      ptr->user_proc[user_index].id  = n;
      break;
  }

	return pid;
}

static int setup_args(const int argc, char * const argv[]){

  int option;
	while((option = getopt(argc, argv, "n:s:t:h:v")) != -1){
		switch(option){
			case 'h':
        fprintf(stderr, "Usage: master [-n x] [-s x] [-t x] infile.txt\n");
        fprintf(stderr, "-n 40 Processes to start\n");
        fprintf(stderr, "-s 18 Processes to rununing\n");
        fprintf(stderr, "-t 5 Runtime\n");
        fprintf(stderr, "-v   Verbose\n");
        fprintf(stderr, "-h Show this message\n");
				return -1;

      case 'n':  nmax	= atoi(optarg); break;
			case 's':  smax	= atoi(optarg); break;
      case 't':  tmax	= atoi(optarg); break;
      case 'v':  v = 1;               break;

      default:
				fprintf(stderr, "OSS: Error: Invalid option %c\n", option);
				return -1;
		}
	}

  if( (smax <= 0) || (smax > 18)   ){
    fprintf(stderr, "Error: -s invalid\n");
    return -1;
  }
  emax = nmax;

  return 0;
}

static struct oss * setup_shm(){

	key_t key = ftok(FTOK_PATH, FTOK_KEY);
	if(key == -1){
		perror("ftok");
		return NULL;
	}

	memid = shmget(key, sizeof(struct oss), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
  if(memid == -1){
  	perror("shmget");
  	return NULL;
  }

	struct oss * addr = (struct oss *) shmat(memid, NULL, 0);
	if(addr == NULL){
		perror("shmat");
		return NULL;
	}

  bzero(addr, sizeof(struct oss));

  if(sem_init(&addr->sem, 1, 1) == -1){
    perror("sem_init");
    return NULL;
  }

	return addr;
}

static int setup_msg(){

	key_t key = ftok(FTOK_PATH, FTOK_MSG_KEY);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	msgid = msgget(key, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
  if(msgid == -1){
  	perror("shmget");
  	return -1;
  }
  return msgid;
}

static int shared_clock_update(){
  struct timeval timestep, temp;

  timestep.tv_sec = 0;
  timestep.tv_usec = 1000;

  sem_wait(&ptr->sem);
  timeradd(&ptr->shared_clock, &timestep, &temp);
  ptr->shared_clock = temp;
  sem_post(&ptr->sem);

  return 0;
}

//Setup the system resource descriptors
static void setup_res(){
  int i;

  //20% of the resources are
  int about20 = 20 + (((rand() % 100) > 50) ? -5 : 5);
  int shared_descriptors = (RMAX / (100 / about20)) + 1;

  printf("OSS: %d descriptors will be shared\n", shared_descriptors);

  for(i=0; i <= shared_descriptors; i++){
    ptr->res[i].val =
    ptr->res[i].max = 1 + (rand() % (RVMAX-1));

    printf("R%d=%d max=%d, shared\n", i, ptr->res[i].val, ptr->res[i].max);
  }

  //rest of the resource descriptors are not shared, and have maximum value of 1
  //so only 1 process can take them == not shared
  for(i=shared_descriptors+1; i < RMAX; i++){
    ptr->res[i].val =
    ptr->res[i].max = 1;
    printf("R%d=%d max=%d, static\n", i, ptr->res[i].val, ptr->res[i].max);
  }

}

static void list_resources(){
  int i,j;

  printf("Current System Resources at time %ld:%ld\n",
    ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

  for(i=0; i < RMAX; i++){
    printf("R%02d ", i);
  }
  printf("\n");

  for(i=0; i < SMAX; i++){
    struct resource_descriptor * r = ptr->user_proc[i].res;

    int alloc = 0;
    for(j=0; j < RMAX; j++){
      if(r[j].val > 0){
        alloc = 1;  //we have allocations
        break;
      }
    }

    if(alloc){
      printf("P%02d ", ptr->user_proc[i].id);
      for(j=0; j < RMAX; j++){
        printf("% 3d ", r[j].val);
      }
      printf("\n");
    }
  }
}

static int gather_requests(struct request_message rmsg[SMAX]){
  int i = 0;

  //until we have space for another message
  while(i < SMAX){
    //try to receive a message without blocking
    if(msgrcv(msgid, (void*)&rmsg[i], sizeof(struct request_message), getpid(), IPC_NOWAIT) < 0){
      //there is no message now
      break;//stop loop
    }
    ++i;
  }

  return i; //return number of messages in buffer
}

static int request_block(struct request_message *m){
  if(bmsg_len < SMAX){  //if we have space in blocked queue
    memcpy(&bmsg[bmsg_len++], m, sizeof(struct request_message));
    return 0;
  }else{
    //this shouldn't happen, but return error
    return -1;
  }
}

static int request_deadlock_check(struct request_message *r,
                                  struct request_message a[SMAX], const int alen,
                                  struct request_message b[SMAX], const int blen){
  int i, j, avail[RMAX], safe[SMAX];
  struct request_message c[SMAX];
  const int clen = alen + blen;

  if(clen > SMAX){
    fprintf(stderr, "OSS: Error alen + blen > SMAX\n");
    return -1;
  }

  if(clen <= 0){
    return 0;
  }

  //group current requests and blocked toghether
  if(alen > 0){
    memcpy(c, a, sizeof(struct request_message)*alen);
  }
  if(blen > 0){
    memcpy(&c[alen], b, sizeof(struct request_message)*blen);
  }

  bzero(safe, sizeof(int)*SMAX);  //nobody is safe

  //copy available resources at current time
  for(i=0; i < RMAX; i++){
    avail[i] = ptr->res[i].val;
  }

  //remove request from available
  avail[r->rd] -= r->val;

  //run the deadlock algorithm
  i = 0;
  while(i != clen){

    for(i = 0; i < clen; i++){

      if(safe[i] == 0){  //if user process is not safe

        //check if the request can finish with the available resource
        struct request_message * m = &c[i];
        struct oss_user * usr = &ptr->user_proc[m->user];

        //calculate the need
        int enough = 1;
        for(j=0; j < RMAX; j++){
          const int need = usr->res[j].max - usr->res[j].val;
          //check if need can be satisfied with available
          if(need > avail[j]){
            enough = 0;
            break;
          }
        }


        if(enough){ //if we have enough

          //merge allocated to availabel, since process can finish
          for(j=0; j < RMAX; j++){
            avail[j] += usr->res[j].val; //return request resource to current resource available
          }
          safe[i] = 1;  //this request is safe

          break;  //restart check from beginning, since available has changed
        }
      }
    }
  }

  //check if some of the requests is not safe
  int not_safe = 0;
  for(i=0; i < clen; i++){
    if(safe[i] == 0){
      not_safe = 1;
      break;
    }
  }

  if(not_safe){ //if we are not safe
    printf("Processes ");
    for(i=0; i < clen; i++){
      if(safe[i] == 0){
        printf("P%d ", c[i].user);
      }
    }
    printf("could deadlock in this scenario\n");
    ostat.deadlocks++;
  }
  ostat.deadlock_checks++;

  return not_safe;
}

static int unblock_requests(struct request_message * rmsg, const int nreq){
  int i, unblocked = 0;

  //for each request in blocked queue
  for(i=0; i < bmsg_len; i++){

    struct request_message * m = &bmsg[i];
    struct oss_user * usr = &ptr->user_proc[m->user];

    //if we have enough resource and no deadlock
    if( (ptr->res[m->rd].val >= m->val) &&
        (request_deadlock_check(&bmsg[i], rmsg, nreq, &bmsg[i+1], bmsg_len - i - 1) == 0) ){

      //update system resource descriptor table
      ptr->res[m->rd].val -= m->val;

      ostat.unblocks++;
      printf("OSS granting P%d blocked request R%d at time %ld:%ld\n",
        usr->id, m->rd, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      //unblock the waiting user
      m->mtype = usr->pid;
      m->op = REQUEST;
      msgsnd(msgid, (void*)m, REQUEST_MSG_SIZE, 0);

      ++unblocked;

      //remove message from queue
      if(i != (bmsg_len-1)){  //if we are not at last message
        //replace it with last message
        memcpy(&bmsg[i], &bmsg[bmsg_len-1], sizeof(struct request_message));
        --i;
      }
      --bmsg_len;
    }
  }
  //bmsg_len -= unblocked;

  return unblocked;
}

static int process_requests(struct request_message rmsg[SMAX], const int nreq){
  int i;

  for(i=0; i < nreq; i++){
    struct request_message * m = &rmsg[i];
    struct oss_user * usr = &ptr->user_proc[m->user];

    int respond = 0;
    if(m->op == REQUEST){
      ostat.requests++;
      printf("OSS has detected Process P%d requesting R%d at time %ld:%ld\n",
        usr->id, m->rd, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      //check if we have enough resource
      int block = 0;
      if(ptr->res[m->rd].val < m->val){
        block = 1;
      }

      //check for deadlock
      if(nreq > 1){
        printf("OSS running deadlock avoidance at time %ld:%ld\n",
          ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

        //run deddlock check on the requests left
        if(request_deadlock_check(&rmsg[i], &rmsg[i+1], (nreq - i - 1), bmsg, bmsg_len)){
          printf("\tUnsafe state after granting request; request not granted\n");
          block = 1;
        }else{
          printf("\tSafe state after granting request found\n");
        }
      }

      if(block){
        if(request_block(m) < 0){
          m->op = CANCEL;
          respond = 1;
          printf("\tCancelled P%d request R%d\n", usr->id, m->rd);
          ostat.cancelled++;
        }else{
          m->op = BLOCK;
          ostat.blocks++;
          printf("\tP%d added to wait queue, waiting on R%d\n", usr->id, m->rd);
        }
      }else{
        //update system resource descriptor table
        ptr->res[m->rd].val -= m->val;
        printf("OSS granting P%d request R%d at time %ld:%ld\n",
          usr->id, m->rd, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

        ostat.grants++;
        respond = 1;

        //show table of current resource allocations
        if(v){
          if((ostat.grants % 20) == 0){ //on each 20 granted requests
            list_resources();
          }
        }
      }

    }else if(m->op == RELEASE){

      ostat.releases++;
      printf("OSS has acknowledged Process P%d releasing R%d=%d at time %ld:%ld\n",
        usr->id, m->rd, m->val, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      //update system resource descriptor table
      ptr->res[m->rd].val += m->val;

      //on every release we ty to unblock a request from queue
      unblock_requests(&rmsg[i], (nreq - i));

      respond = 1;  //send reply to user proc
    }else if(m->op == TERMINATING){

      printf("OSS has acknowledged Process P%d is terminating at time %ld:%ld\n",
        usr->id, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      printf("\tReleasing ");
      int j;
      for(j=0; j < RMAX; j++){
        if(usr->res[j].val > 0){
          printf("R%d=%d ", j, usr->res[j].val);
          ptr->res[j].val += usr->res[j].val;
        }
      }
      printf("\n");

      wait_children();
      respond = 0;

    }else{

      printf("OSS has detected invalid message from Process P%d at time %ld:%ld\n",
        usr->id, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

      m->op = CANCEL;
      respond = 1;  //send reply to user proc
    }

    if(respond){
      m->mtype = usr->pid;
      msgsnd(msgid, (void*)m, REQUEST_MSG_SIZE, 0);
    }
  }
  return 0;
}

static int cancel_blocked(){
  if(bmsg_len == 0){
    return -1;
  }

  //drop last blocked request
  struct request_message * m = &bmsg[--bmsg_len];
  struct oss_user * usr = &ptr->user_proc[m->user];

  ostat.cancelled++;
  printf("OSS cancelled P%d blocked request R%d at time %ld:%ld\n",
    usr->id, m->rd, ptr->shared_clock.tv_sec, ptr->shared_clock.tv_usec);

  //unblock the waiting user
  m->mtype = usr->pid;
  m->op = CANCEL;
  msgsnd(msgid, (void*)m, REQUEST_MSG_SIZE, 0);

  return 0;
}

static void list_ostat(){
  printf("Requests: %d\n", ostat.requests);
  printf("\n");
  printf("Grants: %d\n", ostat.grants);
  printf("Blocks: %d\n", ostat.blocks);
  printf("Unblocks: %d\n", ostat.unblocks);
  printf("Cancelled: %d\n", ostat.cancelled);
  printf("\n");
  printf("Deadlocks: %d\n", ostat.deadlocks);
  printf("Deadlocks Checks: %d\n", ostat.deadlock_checks);
}

int main(const int argc, char * const argv[]){

  struct request_message rmsg[SMAX];  //request messages
  int nreq; //number of request messages

  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, sig_handler);
  signal(SIGCHLD, sig_handler);
  signal(SIGALRM, sig_handler);

  ptr = setup_shm();
  if(ptr == NULL){
    clean_shm();
    return EXIT_FAILURE;
  }

  if(setup_msg() == -1){
    clean_shm();
    return EXIT_FAILURE;
  }

  if(setup_args(argc, argv) == -1){
    clean_shm();
    return EXIT_FAILURE;
  }

  setup_res();
  ptr->stop_flag = 0;

  //clear the statistics
  bzero(&ostat, sizeof(struct oss_stat));

  //alarm(tmax);  /* setup alarm after arguments are processes*/

	while(loop_flag && (e < emax)){

    //if we can, we start a new process
    if( (n < nmax) && (s < smax)  ){
      const pid_t user_pid = exec_user();  //TODO: setup process index
      if(user_pid > 0){
        ++n; ++s;  /* increase count of processes started */
      }
    }

    nreq = gather_requests(rmsg);
    process_requests(rmsg, nreq);
    if(bmsg_len == s){  //if all running are blocked
      if(unblock_requests(NULL, 0) == 0){
        //if blocked queue has a deadlock, we drop last request
        cancel_blocked();
      }
    }

    shared_clock_update();
	}

  //show the results
  list_ostat();

  term_children();
	clean_shm();
  return EXIT_SUCCESS;
}
