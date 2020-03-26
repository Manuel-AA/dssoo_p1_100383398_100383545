#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
#include "my_io.h"

//#include "mythread.h"
#include "interrupt.h"

#include "queue.h"

TCB* scheduler();
void activator();
void timer_interrupt(int sig);
void disk_interrupt(int sig);
long ticks = 0;
struct queue *listosAlta;
struct queue *listosBaja;
struct queue *bloqueados;

/* Array of state thread control blocks: the process allows a maximum of N threads */
static TCB t_state[N]; 

/* Current running thread */
static TCB* running;
static int current = 0;
static TCB* oldRunning;

/* Variable indicating if the library is initialized (init == 1) or not (init == 0) */
static int init=0;

/* Thread control block for the idle thread */
static TCB idle;

static void idle_function()
{
  while(1);
  
}

void function_thread(int sec)
{
    //time_t end = time(NULL) + sec;
    while(running->remaining_ticks);
    mythread_exit();
}


/* Initialize the thread library */
void init_mythreadlib() 
{
  int i;

  /* Create context for the idle thread */
  if(getcontext(&idle.run_env) == -1)
  {
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(-1);
  }

  idle.state = IDLE;
  idle.priority = SYSTEM;
  idle.function = idle_function;
  idle.run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));
  idle.tid = -1;

  if(idle.run_env.uc_stack.ss_sp == NULL)
  {
    printf("*** ERROR: thread failed to get stack space\n");
    exit(-1);
  }

  idle.run_env.uc_stack.ss_size = STACKSIZE;
  idle.run_env.uc_stack.ss_flags = 0;
  idle.ticks = QUANTUM_TICKS;
  makecontext(&idle.run_env, idle_function, 1); 

  t_state[0].state = INIT;
  t_state[0].priority = LOW_PRIORITY;
  t_state[0].ticks = QUANTUM_TICKS;

  if(getcontext(&t_state[0].run_env) == -1)
  {
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(5);
  }	

  for(i=1; i<N; i++)
  {
    t_state[i].state = FREE;
  }

  t_state[0].tid = 0;
  running = &t_state[0];
  listosAlta = queue_new();
  listosBaja = queue_new();
  bloqueados = queue_new();

  /* Initialize disk and clock interrupts */
  init_disk_interrupt();
  init_interrupt();
}


/* Create and intialize a new thread with body fun_addr and one integer argument */ 
int mythread_create (void (*fun_addr)(),int priority,int seconds)
{
  int i;
  
  if (!init) { init_mythreadlib(); init=1;}

  for (i=0; i<N; i++)
    if (t_state[i].state == FREE) break;

  if (i == N) return(-1);

  if(getcontext(&t_state[i].run_env) == -1)
  {
    perror("*** ERROR: getcontext in my_thread_create");
    exit(-1);
  }

  t_state[i].state = INIT;
  t_state[i].priority = priority;
  t_state[i].function = fun_addr;
  if (t_state[i].priority == LOW_PRIORITY){
    t_state[i].ticks = QUANTUM_TICKS;
  }
  t_state[i].execution_total_ticks = seconds_to_ticks(seconds);
  t_state[i].remaining_ticks = t_state[i].execution_total_ticks;
  t_state[i].run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));
  
  if(t_state[i].run_env.uc_stack.ss_sp == NULL)
  {
    printf("*** ERROR: thread failed to get stack space\n");
    exit(-1);
  }

  t_state[i].tid = i;
  t_state[i].run_env.uc_stack.ss_size = STACKSIZE;
  t_state[i].run_env.uc_stack.ss_flags = 0;

  makecontext(&t_state[i].run_env, fun_addr,2,seconds);
  if (t_state[i].priority == HIGH_PRIORITY && running->priority == LOW_PRIORITY){
    running->state = INIT;
    running->ticks = QUANTUM_TICKS;
    disable_interrupt();
    disable_disk_interrupt();
    enqueue(listosBaja, (void*)running);   
    enable_disk_interrupt();
    enable_interrupt();
    oldRunning = running;
    running = &t_state[i];
    current = t_state[i].tid;
    printf("*** THREAD %i PREEMTED: SETCONTEXT OF %i\n", oldRunning->tid, running->tid);
    activator(running);
  }
  else{
    if (t_state[i].priority == HIGH_PRIORITY && running->priority == HIGH_PRIORITY && t_state[i].execution_total_ticks < running->remaining_ticks){
    running->state = INIT;
    disable_interrupt();
    disable_disk_interrupt();
    sorted_enqueue(listosAlta, running, running->remaining_ticks);  
    enable_disk_interrupt();
    enable_interrupt();
    oldRunning = running;
    running = &t_state[i];
    current = t_state[i].tid;
    printf("*** SWAPCONTEXT FROM %i TO %i\n", oldRunning->tid, running->tid);
    activator(running);
    }
    else{
      disable_interrupt();
      disable_disk_interrupt();
      if (t_state[i].priority == HIGH_PRIORITY){
        sorted_enqueue(listosAlta, (void*)&(t_state[i]), t_state[i].remaining_ticks);
      }
      else{
        enqueue(listosBaja, (void*)&(t_state[i]));
      }
      enable_disk_interrupt();
      enable_interrupt();
    }
  }
  return i;
} 
/****** End my_thread_create() ******/


/* Read disk syscall */
int read_disk()
{
  if (data_in_page_cache() != 0){
    running->state = WAITING;
    if (running->priority == LOW_PRIORITY){
      running->ticks = QUANTUM_TICKS;
    }
    disable_interrupt();
    disable_disk_interrupt();
    enqueue(bloqueados, (void*)(running));     
    enable_disk_interrupt();
    enable_interrupt();
    oldRunning = running;
    running = scheduler();
    printf("*** THREAD %i READ FROM DISK\n", oldRunning->tid);
    activator(running);
  }
  return 1;
}

/* Disk interrupt  */
void disk_interrupt(int sig)
{
  TCB* nuevo;
  if (!queue_empty(bloqueados)){
    disable_interrupt();
    disable_disk_interrupt();
    nuevo = dequeue(bloqueados);
    if (nuevo->priority == LOW_PRIORITY){
      enqueue(listosBaja, (void*)(nuevo));
    }
    else{
      sorted_enqueue(listosAlta, (void*)(nuevo), nuevo->remaining_ticks);
    }
    enable_disk_interrupt();
    enable_interrupt();
    printf("*** THREAD %i READY\n", nuevo->tid);
  }

}


/* Free terminated thread and exits */
void mythread_exit() {
  int tid = mythread_gettid();	

  printf("*** THREAD %d FINISHED\n", tid);	
  t_state[tid].state = FREE;
  free(t_state[tid].run_env.uc_stack.ss_sp); 
  oldRunning = running;
  running = scheduler();
  printf("*** THREAD %i TERMINATED: SETCONTEXT OF %i\n", oldRunning->tid, running->tid);
  activator(running);
}


void mythread_timeout(int tid) {
  printf("*** THREAD %d EJECTED\n", tid);
  t_state[tid].state = FREE;
  free(t_state[tid].run_env.uc_stack.ss_sp);

  oldRunning = running;
  running = scheduler();
  activator(running);
}


/* Sets the priority of the calling thread */
void mythread_setpriority(int priority) 
{
  int tid = mythread_gettid();	
  t_state[tid].priority = priority;
  t_state[tid].remaining_ticks = 195;
  
}

/* Returns the priority of the calling thread */
int mythread_getpriority(int priority) 
{
  int tid = mythread_gettid();	
  return t_state[tid].priority;
}


/* Get the current thread id.  */
int mythread_gettid(){
  if (!init) { init_mythreadlib(); init=1;}
  return current;
}


/* RR */

TCB* scheduler()
{
  TCB* nuevo;
  if (!queue_empty(listosAlta)){
    disable_interrupt();
    disable_disk_interrupt();
    nuevo = dequeue(listosAlta);
    enable_disk_interrupt();
    enable_interrupt();
    current = nuevo->tid;
    return nuevo;
  }
  if (!queue_empty(listosBaja)){
    disable_interrupt();
    disable_disk_interrupt();
    nuevo = dequeue(listosBaja);
    enable_disk_interrupt();
    enable_interrupt();
    current = nuevo->tid;
    return nuevo;
  }
  if (queue_empty(listosAlta) && queue_empty(listosBaja) && !queue_empty(bloqueados)){
    nuevo = &idle;
    current = nuevo->tid;
    return nuevo;
  }
  printf("*** FINISH\n");	
  exit(1);
  
}


/* Timer interrupt */
void timer_interrupt(int sig){
  ticks++;
  running->remaining_ticks--;
  if (running->remaining_ticks < 0){
    mythread_timeout(running->tid);
  }
  if (running->priority == LOW_PRIORITY){
    running->ticks--;
    if (running->ticks == 0){
      running->ticks = QUANTUM_TICKS;
      running->state = INIT;
      disable_interrupt();
      disable_disk_interrupt();
      enqueue(listosBaja, (void*)(running));     
      enable_disk_interrupt();
      enable_interrupt();
      oldRunning = running;
      running = scheduler();
      if (running != oldRunning){
        printf("*** SWAPCONTEXT FROM %i TO %i\n", oldRunning->tid, running->tid);
        activator(running);
      }
    }
  }
  if (running->priority == SYSTEM){
    oldRunning = running;
    running = scheduler();
    if (running != oldRunning){
      printf("*** THREAD READY: SET CONTEXT TO %d\n", running->tid);
      activator(running);
    }
  }
} 

/* Activator */
void activator(TCB* next){
  if (oldRunning->state == FREE){
    setcontext (&(next->run_env));
    printf("mythread_free: After setcontext, should never get here!!...\n");
  }
  else{
    swapcontext(&(oldRunning->run_env), &(next->run_env));
  }	
}



