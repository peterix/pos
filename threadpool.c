#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED 1 /* XPG 4.2 - needed for WCOREDUMP() */
#define _POSIX_C_SOURCE 199506L
#define _REENTRANT

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

pthread_mutex_t ticket_mutex;
pthread_mutex_t csection_mutex;
pthread_cond_t csection_cond;
pthread_condattr_t cattr;
pthread_mutexattr_t mattr;
int next_ticket_assign = 0;
int next_ticket = 0;

int init_tickets()
{
	int all_ok = 0;
	all_ok |= pthread_mutexattr_init(&mattr);
	all_ok |= pthread_condattr_init(&cattr);
	all_ok |= pthread_mutex_init(&ticket_mutex, &mattr);
	all_ok |= pthread_mutex_init(&csection_mutex, &mattr);
	all_ok |= pthread_cond_init(&csection_cond, &cattr);
	return all_ok == 0;
}

int numthreads, numtickets;
int getticket(void)
{
	int next;
	pthread_mutex_lock(&ticket_mutex);
	next = next_ticket_assign;
	next_ticket_assign++;
	pthread_mutex_unlock(&ticket_mutex);
	return next;
}

/* wait until its our turn */
void await(int aenter)
{
	pthread_mutex_lock(&csection_mutex);
	while (next_ticket != aenter)
	{
		pthread_cond_wait(&csection_cond, &csection_mutex);
	}
}

/* wake up the others */
void advance(void)
{
	next_ticket++;
	pthread_cond_broadcast(&csection_cond);
	pthread_mutex_unlock(&csection_mutex);
}

void finish_tickets()
{
	pthread_cond_destroy(&csection_cond);
	pthread_mutex_destroy(&csection_mutex);
	pthread_mutex_destroy(&ticket_mutex);
	
	pthread_mutexattr_destroy(&mattr);
	pthread_condattr_destroy(&cattr);
}

void wait_about_half_second(unsigned int *seed)
{
	struct timespec how_long;
	int length = rand_r(seed) % 501; /* 0..500 */
	how_long.tv_sec = 0;
	how_long.tv_nsec = length * 1000000;
	/*
	 * We really don't care if this gets interrupted by signals. Or do we?
	 * ... we could, maybe. I decided not to.
	 */
	nanosleep(&how_long, NULL);
}

void *thread_func(void *threadid)
{
	int ticket;
	long tid = (long)threadid;
	unsigned int seed = tid;
	while ((ticket = getticket()) < numtickets)
	{
		wait_about_half_second(&seed);
		/* critical section */
		await(ticket);
		{
			printf("%d\t(%ld)\n", ticket, tid);
		}
		advance();
		wait_about_half_second(&seed);
	}
	pthread_exit(NULL);
}


const char * helptext =
"Ticket algorithm usage:\n\
%s THREADS TICKETS\n\
 where THREADS is the number of threads\n\
 and TICKETS is the number of critical section tickets handed out\n";

int main(int argc, char **argv)
{
	long i;
	int rc;
	pthread_attr_t attr;
	pthread_t *threads;
	void *retval;
	
	if(argc != 3)
	{
		fprintf(stderr, helptext, argv[0]);
		return EXIT_FAILURE;
	}
	sscanf(argv[1],"%d", &numthreads);
	sscanf(argv[2],"%d", &numtickets);
	
	if(numthreads < 0 || numtickets < 0)
	{
		fprintf(stderr, "Both thread and ticket numbers have to be positive.\n");
		return EXIT_FAILURE;
	}
	/* init the pthread constructs */
	pthread_attr_init(&attr);
	init_tickets();
	
	/* allocate thread structs */
	threads = malloc(sizeof(pthread_t) * numthreads);
	if(!threads)
	{
		fprintf(stderr, "ERROR; out of memory\n");
		return EXIT_FAILURE;
	}
	
	/* create threads */
	for(i = 0; i<numthreads; i++)
	{
		rc = pthread_create(&threads[i], &attr, thread_func, (void *)i);
		if (rc != 0)
		{
			fprintf(stderr, "ERROR; return code from pthread_create() is %d\n", rc);
			return EXIT_FAILURE;
		}
	}
	
	/* stuff happens over in the threads */
	
	/* then wait for them all to finish */
	for(i = 0; i<numthreads; i++)
	{
		rc = pthread_join(threads[i], &retval);
		if (rc != 0)
		{
			printf("ERROR; return code from pthread_join() is %d\n", rc);
			return EXIT_FAILURE;
		}
	}
	/* destroy the attribute */
	pthread_attr_destroy(&attr);
	
	/* destroy pthread constructs */
	finish_tickets();
	
	/* free thread structs */
	free(threads);
	
	return EXIT_SUCCESS;
}
