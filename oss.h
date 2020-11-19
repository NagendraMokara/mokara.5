#define FTOK_PATH "/root"
#define FTOK_KEY 3450
#define FTOK_MSG_KEY 3451

//maximum running processes
#define SMAX 18
//resource count
#define RMAX 20
//resource value maximum
#define RVMAX 10

//maximum value for bound B, in ms
#define BVMAX 100

enum op { REQUEST=0, RELEASE=1, BLOCK=2, CANCEL=3, TERMINATING=4};

struct request_message {
	long mtype;

	int user;	//user index
	int rd;		//resource descriptor
	int val;	//value
	int op;		//operation - request or release
};

#define REQUEST_MSG_SIZE sizeof(int)*4

struct resource_descriptor {
	int val;	//resource current value
	int max;	//if max == 1, resource is not shared
};

struct oss_user {
	int pid, id;

	//user resource descriptors
	struct resource_descriptor res[RMAX];
};

struct oss {
	sem_t sem;
	struct timeval shared_clock;

	//tell user to exit, if oss is stopped (due to signal or timeout)
	int stop_flag;

	//system resource descriptors
	struct resource_descriptor res[RMAX];

	//process control table
	struct oss_user user_proc[SMAX];
};
