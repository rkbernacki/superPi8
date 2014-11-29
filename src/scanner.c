#include <newt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <wiringPi.h>
#include <semaphore.h>
#include "utils.c"
#include "config.c"
#include "capture.h"


typedef struct Option {
    char* name;
    char* startby;
    char* time;
    char* length;
    int type;
} Option;


typedef void (*functiontype)();
functiontype edge_falling_pos_func = NULL;
functiontype edge_falling_watch_func = NULL;

struct timespec mode_step_sem_ts;
sem_t mode_step_sem;
pthread_mutex_t edge_falling_pos_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t edge_falling_watch_mutex = PTHREAD_MUTEX_INITIALIZER;

double mode_move_finish;
pthread_t mode_move_thread = NULL;
pthread_mutex_t mode_move_mutex = PTHREAD_MUTEX_INITIALIZER;

sem_t scan_watch_sem;
pthread_mutex_t scan_watch_mutex = PTHREAD_MUTEX_INITIALIZER;

void mode_scan(){
    newtComponent form,
                  name_label, name_entry,
                  startby_label, startby_entry, 
                  time_label, time_entry,  
                  length_label, length_entry,
                  type_entry[2],
                  button_run, button_cancel;
    
    int rows, cols;
    int haserror = 0;
    Option option;

    while(1){
        newtCls();
        newtInit();
    
        if (haserror)
            newtPushHelpLine("Option are invalid");

        newtGetScreenSize(&cols, &rows);
        newtOpenWindow((cols - 72)/2, 5, 72, 20, "Preparation to scan a super 8mm movie");

        name_label = newtLabel(1, 1, "Name of the Movie");
        name_entry = newtEntry(1, 2, "sample", 70, &option.name, NEWT_FLAG_SCROLL | NEWT_FLAG_RETURNEXIT);

        startby_label = newtLabel(1, 4, "Start movie by picture [default 0]");
        startby_entry = newtEntry(1, 5, "0", 70, &option.startby, NEWT_FLAG_SCROLL | NEWT_FLAG_RETURNEXIT);

        time_label = newtLabel(1, 7, "Start the scanner at e.g. \"8:20\" [default now]");
        time_entry = newtEntry(1, 8, "now", 70, &option.time, NEWT_FLAG_SCROLL | NEWT_FLAG_RETURNEXIT);

        length_label = newtLabel(1, 10, "Approximately length in meters");
        length_entry = newtEntry(1, 11, "60", 70, &option.length, NEWT_FLAG_SCROLL | NEWT_FLAG_RETURNEXIT);

        type_entry[0] = newtRadiobutton(1, 13, "8mm", 1, NULL);
        type_entry[1] = newtRadiobutton(10, 13, "Super 8mm", 0, type_entry[0]);

        button_cancel = newtButton(19, 15, "Cancel");
        button_run = newtButton(44, 15, "Run");
        form = newtForm(NULL, NULL, 0);
        newtFormAddComponents(form, name_label, name_entry, startby_label, startby_entry,
                              time_label, time_entry, length_label, length_entry, NULL);

        int i;
        for (i = 0; i < 2; i++)
            newtFormAddComponent(form, type_entry[i]);

        newtFormAddComponents(form, button_cancel, button_run, NULL);

        struct newtExitStruct es; 
        newtFormRun(form, &es);
        newtFinished();
        
        for (i = 0; i < 2; i++)
            if (newtRadioGetCurrent(type_entry[0]) == type_entry[i])
                option.type = i;
        if (es.u.co == button_cancel){
            newtFormDestroy(form);
            return;
        }

        if (validate_user_input(option)){
            start_scanner(option);
            newtFormDestroy(form);
            return;
        }
        
        haserror = 1;
    }
}

int validate_user_input(Option option){
    if (strlen(option.name) < 1 || strlen(option.name) > 100)
        return 0;
    
    if (strlen(option.startby) < 1 || !is_integer(option.startby) || atoi(option.startby) < 0)
        return 0;

    if (strlen(option.length) < 1 || !is_integer(option.length) || atoi(option.length) < 1 || atoi(option.length) > 100)
        return 0;

    int time_result[3];
    split_time(option.time, time_result);

    if (time_result[0] == 1)
        return 0;

    return 1;
}


struct start_runner_args {
    newtComponent form;
    newtComponent scale_entry;
    newtComponent uptime_entry;
    newtComponent diskleft_entry;
    newtComponent button_wait;
    int current_image_pos;
    int start_image_pos;
    int stop_next_possible;
};


void start_scan_runner(void *arguments){
    struct start_runner_args *args = (struct start_runner_args*)arguments;
    char path[320];
    int i = 0;    
    while(++i){
        if (args->stop_next_possible)
            break;
        sem_wait(&scan_watch_sem);
        pthread_mutex_lock(&scan_watch_mutex);
        mode_step();
        snprintf(path, 320, "%s/%05d.raw", IMAGE_PATH, i);
        FILE *fd = fopen(path,  "wa");
        capture_image(1, fd);
        fclose(fd);
        sem_post(&scan_watch_sem);
        pthread_mutex_unlock(&scan_watch_mutex);
    }
}



void start_display_runner(void *arguments) {    
    capture_open();
    struct start_runner_args *args = (struct start_runner_args*)arguments;
    time_t uptime = time(NULL);
    pthread_t start_scan_runner_thread;
    pthread_create(&start_scan_runner_thread, NULL, &start_scan_runner, args);
    int i;
    while(1){
        newtScaleSet(args->scale_entry, i);
        char settext[120];
        char output[50];
        time_t uptime_now = time(NULL);
        time_formatting((double)(uptime_now - uptime), output);
        snprintf(settext, 120, "Uptime: %s", output);
        newtTextboxSetText(args->uptime_entry, settext);
        snprintf(settext, 120, "Disk left: %s", disk_left(output));
        newtTextboxSetText(args->diskleft_entry, settext);
        newtRefresh();
        if(pthread_kill(start_scan_runner_thread, 0))
            break;
    }
    capture_close();
}

void start_scanner_watch(){
    pthread_mutex_lock(&scan_watch_mutex);
    int v;
    sem_getvalue(&scan_watch_sem, &v);
    if (v > 0)
        sem_wait(&scan_watch_sem);
    pthread_mutex_unlock(&scan_watch_mutex);
}


void start_scanner(Option option){

    int cols, rows;

    newtCls();
    newtInit();

    newtGetScreenSize(&cols, &rows);
    char title[120];
    strcpy(title, "Scanning: ");
    strcat(title, option.name);
    newtOpenWindow((cols - 72)/2, 5, 72, 20, title);

    newtComponent form = newtForm(NULL, NULL, 0);
    newtComponent scale_entry = newtScale(1, 13, 70, 10);

    newtComponent uptime_entry = newtTextbox(1, 1, 70, 10, NEWT_FLAG_WRAP);
    newtComponent diskleft_entry = newtTextbox(1, 2, 70, 10, NEWT_FLAG_WRAP);
    
    newtFormAddComponents(form, scale_entry, uptime_entry, diskleft_entry, NULL);

    newtComponent button_cancel = newtButton(19, 15, "Cancel");
    newtComponent button_wait = newtButton(44, 15, "Wait/Continue");
    newtFormAddComponents(form, button_cancel, button_wait, NULL);

    edge_falling_watch_func = &start_scanner_watch;
    struct start_runner_args args;
    args.form = form;
    args.scale_entry = scale_entry;
    args.uptime_entry = uptime_entry;
    args.diskleft_entry = diskleft_entry;
    args.stop_next_possible = 0;
    args.button_wait = button_wait;
    sem_init(&scan_watch_sem, 0, 1);
    newtDrawForm(form);
    newtRefresh();
    pthread_t start_display_runner_thread;
    pthread_create(&start_display_runner_thread, NULL, &start_display_runner, &args);

    while(1){
        struct newtExitStruct es;
        newtFormRun(form, &es);
        if (es.u.co == button_cancel){
            pthread_mutex_lock(&scan_watch_mutex);
            sem_post(&scan_watch_sem);
            args.stop_next_possible = 1;
            pthread_mutex_unlock(&scan_watch_mutex);
            pthread_join(start_display_runner_thread, NULL);
            newtFormDestroy(form);
            newtFinished();
            return;
        }
        if (es.u.co == button_wait) {
            pthread_mutex_lock(&scan_watch_mutex);
            int v;
            sem_getvalue(&scan_watch_sem, &v);
            if (v > 0)
                sem_wait(&scan_watch_sem);
            else
                sem_post(&scan_watch_sem);
            pthread_mutex_unlock(&scan_watch_mutex);
        }
    }
}


void mode_move_stopping(){
    int delay = 0;
    do {
        usleep(delay);
        pthread_mutex_lock(&mode_move_mutex);
        delay = (int)((mode_move_finish - timestamp_mili())*1000000);
        pthread_mutex_unlock(&mode_move_mutex);
    } while(delay > 0);
    pthread_mutex_lock(&mode_move_mutex);
    mode_move_thread = NULL;
    digitalWrite(GPIO_MOTOR, 0);
    pthread_mutex_unlock(&mode_move_mutex);
}


void mode_move(){
    while(1){
        char key = getchar();
        // ESC
        if (key == 27){
            return;
        }
        // spacebar    
        if (key == 32){
            pthread_mutex_lock(&mode_move_mutex);
            mode_move_finish = timestamp_mili() + 0.4;
            if (!mode_move_thread){
                digitalWrite(GPIO_MOTOR, 1);
                pthread_create(&mode_move_thread, NULL, &mode_move_stopping, NULL);
            }
            pthread_mutex_unlock(&mode_move_mutex);
        }
    }
}

void mode_step_stop(){
    digitalWrite(GPIO_MOTOR, 0);
    sem_post(&mode_step_sem);
}

void mode_step(){
    edge_falling_pos_func = &mode_step_stop;
    digitalWrite(GPIO_MOTOR, 1);
    struct timeb tmb;
    ftime(&tmb);
    mode_step_sem_ts.tv_sec = MAX_MOTOR_SPAN + tmb.time;
    sem_timedwait(&mode_step_sem, &mode_step_sem_ts);
    if (digitalRead(GPIO_MOTOR)){
        digitalWrite(GPIO_MOTOR, 0);
        printf("Waiting too long of position signal");
        exit(1);
    }
}

void edge_falling_handler_pos(){
    pthread_mutex_lock(&edge_falling_pos_mutex);
    if(edge_falling_pos_func)
        edge_falling_pos_func();
    edge_falling_pos_func = NULL;
    pthread_mutex_unlock(&edge_falling_pos_mutex);
}

void edge_falling_handler_watch(){
    pthread_mutex_lock(&edge_falling_watch_mutex);
    if(edge_falling_watch_func)
        edge_falling_watch_func();
    edge_falling_watch_func = NULL;
    pthread_mutex_unlock(&edge_falling_watch_mutex);
}


int scanner_init(){
    sem_init(&mode_step_sem, 0, 0);
    return 0;
}


int gpio_init(){
    if(wiringPiSetup() == -1){
        printf("Error could not initalize wiringPi\n");
        return 1;
    }

    pinMode(GPIO_MOTOR, OUTPUT);
	pinMode(GPIO_POS, INPUT);
    pinMode(GPIO_WATCH, INPUT);
    pullUpDnControl(GPIO_POS, PUD_DOWN);
    pullUpDnControl(GPIO_WATCH, PUD_DOWN);
    wiringPiISR(GPIO_POS, INT_EDGE_FALLING, &edge_falling_handler_pos);
    wiringPiISR(GPIO_WATCH, INT_EDGE_FALLING, &edge_falling_handler_watch);
    return 0;
}


