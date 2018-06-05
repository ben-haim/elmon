/* 
 * elmon.c -- Curses based Performance Monitor for Linux
 * License:  GPL version 3
 * Developer: Brian Smith
 * Based on the nmon project by Nigel Griffiths
 */

/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3 as 
    published by the Free Software Foundation

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define RAW(member)      (long)((long)(p->cpuN[i].member)   - (long)(q->cpuN[i].member))
#define RAWTOTAL(member) (long)((long)(p->cpu_total.member) - (long)(q->cpu_total.member)) 

#define VERSION "13b1"
char version[] = VERSION;
static char *SccsId = "elmon " VERSION;

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <ncurses.h>
#include <signal.h>
#include <pwd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define FLIP(variable) if(variable) variable=0; else variable=1;

#ifdef MALLOC_DEBUG
#define MALLOC(argument)        mymalloc(argument,__LINE__)
#define FREE(argument)          myfree(argument,__LINE__)
#define REALLOC(argument1,argument2)    myrealloc(argument1,argument2,__LINE__)
void *mymalloc(int size, int line)
{
void * ptr;
        ptr= malloc(size);
        fprintf(stderr,"0x%x = malloc(%d) at line=%d\n",ptr,size,line);
        return ptr;
}
void myfree(void *ptr,int line)
{
        fprintf(stderr,"free(0x%x) at line=%d\n",ptr,line);
        free(ptr);
}
void *myrealloc(void *oldptr, int size, int line)
{
void * ptr;
        ptr= realloc(oldptr,size);
        fprintf(stderr,"0x%x = realloc(0x%x, %d) at line=%d\n",ptr,oldptr,size,line);
        return ptr;
}
#else
#define MALLOC(argument)        malloc(argument)
#define FREE(argument)          free(argument)
#define REALLOC(argument1,argument2)    realloc(argument1,argument2)
#endif /* MALLOC STUFF */


#define MAX_OPTIONS 40
int enabled_options[MAX_OPTIONS];
int optionCount = 0;
int loop1 = 0;
int loop2 = 0;
int loop_options = 0;
int tempInt = 0;

int enabled_option(int item){
        for(loop1 = 0; loop1 < optionCount; loop1++){
                if(enabled_options[loop1] == item){
                        return 1;
                }
        }
        return 0;
}

void add_option(int item){
        if (enabled_option(item) == 0){
                enabled_options[optionCount] = item;
                optionCount++;
        }
}

void remove_option(int item){
        for(loop1 = 0; loop1 < optionCount; loop1++){
                if(item == enabled_options[loop1]){
                        for (loop2 = loop1; loop2 < optionCount; loop2++){
                                enabled_options[loop2] = enabled_options[loop2+1];
                        }
                        optionCount--;
                        break;
                }
        }
}

void flip(int item){
        if (enabled_option(item)){
                remove_option(item);
        }else{
                add_option(item);
        }
}

/*void move_down_option(int item){
        for(loop1 = 0; loop1 < optionCount - 1; loop1++){
                if (item == enabled_options[loop1]){
                        tempInt = enabled_options[loop1 + 1];
                        enabled_options[loop1 + 1] = enabled_options[loop1];
                        enabled_options[loop1] = tempInt;
                        break;
                }
        }
}*/

/*void move_up_option(int item){
        for(loop1 = 1; loop1 < optionCount; loop1++){
                if (item == enabled_options[loop1]){
                        tempInt = enabled_options[loop1 - 1];
                        enabled_options[loop1 - 1] = enabled_options[loop1];
                        enabled_options[loop1] = tempInt;
                        break;
                }
        }
}*/



#define P_CPUINFO	0
#define P_STAT		1
#define P_VERSION	2
#define P_MEMINFO   	3
#define P_UPTIME   	4
#define P_LOADAVG   	5
#define P_NFS   	6
#define P_NFSD   	7
#define P_NUMBER	8 /* one more than the max */

char *month[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

/* Cut of everything after the first space in callback
 * Delete any '&' just before the space
 */
char *check_call_string (char* callback, const char* name)
{
        char * tmp_ptr = callback;

        if (strlen(callback) > 256) {
                fprintf(stderr,"ERROR elmon: ignoring %s - too long\n", name);
                return (char *) NULL;
        }

        for( ; *tmp_ptr != '\0' && *tmp_ptr != ' ' && *tmp_ptr != '&'; ++tmp_ptr )
                ;

        *tmp_ptr = '\0';

        if( tmp_ptr == callback )
                return (char *)NULL;
        else
                return callback;
}

/* Remove error output to this buffer and display it if NMONDEBUG=1 */
char errorstr[70];
int error_on = 0;
void error(char *err) 
{
	strncpy(errorstr,err,69);
}

/* /proc/cpuinfo can be 512 bytes per CPU and we allow 256 CPUs */
/* and 20 lines per CPU so boost the buffers for this one */
#define PROC_MAXBUF   (1024*4)
#define CPUINFO_MAXBUF (512*256)
#define PROC_MAXLINES (20*256*sizeof(char *))

int reread =0;
struct {
	FILE *fp;
	char *filename;
	int lines;
	char *line[PROC_MAXLINES];
	char *buf;
} proc[P_NUMBER];

void proc_init()
{
int i;
	/* Initialise the file pointers */
	for(i=0;i<P_NUMBER;i++) {
		proc[i].fp = 0;
		if(i == P_CPUINFO)
			proc[i].buf  = (char *)malloc(CPUINFO_MAXBUF);
		else
			proc[i].buf  = (char *)malloc(PROC_MAXBUF);
	}
	proc[P_CPUINFO].filename = "/proc/cpuinfo";
	proc[P_STAT].filename    = "/proc/stat";
	proc[P_VERSION].filename = "/proc/version";
	proc[P_MEMINFO].filename = "/proc/meminfo";
	proc[P_UPTIME].filename  = "/proc/uptime";
	proc[P_LOADAVG].filename = "/proc/loadavg";
	proc[P_NFS].filename     = "/proc/net/rpc/nfs";
	proc[P_NFSD].filename    = "/proc/net/rpc/nfsd";
}

void proc_read(int num)
{
int i;
int size;
int found;
char buf[1024];
int bytes;

	if(proc[num].fp == 0) {
		if( (proc[num].fp = fopen(proc[num].filename,"r")) == NULL) {
			sprintf(buf, "failed to open file %s", proc[num].filename);
			error(buf);
			proc[num].fp = 0;
			return;
		}
	}
	rewind(proc[num].fp);
	if(num == P_CPUINFO)
		bytes = CPUINFO_MAXBUF -1;
	else
		bytes = PROC_MAXBUF -1;
	size = fread(proc[num].buf, 1, bytes, proc[num].fp);
	proc[num].buf[size]=0;
	proc[num].lines=0;
	proc[num].line[0]=&proc[num].buf[0];
	if(num == P_VERSION) {
		found=0;
		for(i=0;i<size;i++) { /* remove some weird stuff */
			if( found== 0 &&
		 	    proc[num].buf[i]   == ')' &&
			    proc[num].buf[i+1] == ' ' &&
			    proc[num].buf[i+2] == '(' ) {
				proc[num].buf[i+1] = '\n';
				found=1;
			} else {
			    if(
		 	    proc[num].buf[i]   == ')' &&
			    proc[num].buf[i+1] == ' ' &&
			    proc[num].buf[i+2] == '#' ) {
				proc[num].buf[i+1] = '\n';
			    }
			    if(
		 	    proc[num].buf[i]   == '#' &&
			    proc[num].buf[i+2] == '1' ) {
				proc[num].buf[i] = '\n';
			    }
			}
		}
	}
	for(i=0;i<size;i++) {
		if(proc[num].buf[i] == '\t') 
			proc[num].buf[i]= ' '; 
		if(proc[num].buf[i] == '\n') {
			proc[num].lines++;
			proc[num].buf[i] = 0;
			proc[num].line[proc[num].lines] = &proc[num].buf[i+1];
		}
		if(proc[num].lines==PROC_MAXLINES-1)
			break;
	}
	if(reread) {
		fclose( proc[num].fp);
		proc[num].fp = 0;
	}
}

#include <dirent.h>

struct procsinfo {
                int pi_pid;
                char pi_comm[64];
                char pi_state;
                int pi_ppid;
                int pi_pgrp;
                int pi_session;
                int pi_tty_nr;
                int pi_tty_pgrp;
                unsigned long pi_flags;
                unsigned long pi_minflt;
                unsigned long pi_cmin_flt;
                unsigned long pi_majflt;
                unsigned long pi_cmaj_flt;
                unsigned long pi_utime;
                unsigned long pi_stime;
                long pi_cutime;
                long pi_cstime;
                long pi_pri;
                long pi_nice;
                long junk /* removed */;
                long pi_it_real_value;
                unsigned long pi_start_time;
                unsigned long pi_vsize;
                long pi_rss; /* - 3 */
                unsigned long pi_rlim_cur;
                unsigned long pi_start_code;
                unsigned long pi_end_code;
                unsigned long pi_start_stack;
                unsigned long pi_esp;
                unsigned long pi_eip;
                /* The signal information here is obsolete. */
                unsigned long pi_pending_signal;
                unsigned long pi_blocked_sig;
                unsigned long pi_sigign;
                unsigned long pi_sigcatch;
                unsigned long pi_wchan;
                unsigned long pi_nswap;
                unsigned long pi_cnswap;
                int pi_exit_signal;
                int pi_cpu;

		unsigned long statm_size;       /* total program size */
                unsigned long statm_resident;   /* resident set size */
                unsigned long statm_share;      /* shared pages */
                unsigned long statm_trs;        /* text (code) */
                unsigned long statm_drs;        /* data/stack */
                unsigned long statm_lrs;        /* library */
                unsigned long statm_dt;         /* dirty pages */
};


#include <mntent.h>
#include <fstab.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <net/if.h>

int debug =0;
time_t  timer;			/* used to work out the hour/min/second */

/* Counts of resources */
int	cpus = 1;  	/* number of CPUs in system (lets hope its more than zero!) */
int	max_cpus = 1;  	/* highest number of CPUs in DLPAR */
int	networks = 0;  	/* number of networks in system  */
int	partitions = 0;  	/* number of partitions in system  */
int	partitions_short = 0;  	/* partitions file data short form (i.e. data missing) */
int	disks    = 0;  	/* number of disks in system  */
int	seconds  = -1; 	/* pause interval */
int	maxloops = -1;  /* stop after this number of updates */
char	hostname[256];
char	run_name[256];
int	run_name_set = 0;
char	fullhostname[256];
int	loop;

#define DPL 150 /* Disks per line for file output to ensure it 
		does not overflow the spreadsheet input line max */

int disks_per_line = DPL;

#define NEWDISKGROUP(disk) ( (disk) % disks_per_line == 0)

int     x_1 = 0;                        /* curses row */
int     y_1 = 0;                        /* curses column */
int     x_2 = 0;
int     x_3 = 0;

int     use_x_1 = 0;
int     use_x_2 = 0;
int     use_x_3 = 0;

int     num_col = 1;
int	num_col_temp = 1;
int     last_num_col = 1;
int	column_check = 0;

int maxcols = 0;

#define SHOW_CPU 0
#define SHOW_SMP 1
#define SHOW_LONGTERM 2
#define SHOW_DISK 3
#define SHOW_MEMORY 5
#define SHOW_MEMORY_GRAPH 6
#define SHOW_LARGE 7
#define SHOW_KERNEL 8
#define SHOW_NFS 9
#define SHOW_NET 10
#define SHOW_NETERROR 11
#define SHOW_PARTITIONS 12
#define SHOW_HELP 13
#define SHOW_TOP 14
#define SHOW_VERBOSE 15
#define SHOW_JFS 16
#define SHOW_LPAR 17
#define SHOW_VM 18
#define SHOW_DGROUP 19
#define SHOW_DISKMAP 20

/* Mode of output variables */
int	show_aaa     = 1;
int	show_para    = 1;
int	show_headings= 1;
int	show_columns = 1;
int	show_disk_mode = 0;
#define SHOW_DISK_NONE  0
#define SHOW_DISK_STATS 1
#define SHOW_DISK_GRAPH 2
int	show_neterror= 0;
int	show_topmode = 1;
#define ARGS_NONE 0
#define ARGS_ONLY 1
int	show_args    = 0;
int	show_all     = 1;	/* 1=all procs& disk 0=only if 1% or more busy */
int	flash_on     = 0;
int	first_time   = 1;
int	first_huge   = 1;
long	huge_peak    = 0;
int	welcome      = 1;
int	dotline      = 0;
int	show_rrd     = 0;
int     dgroup_loaded = 0; /* 0 = no, 1=needed, 2=loaded */
int	show_raw    = 0;
int	menu_sX	    = 1;
int	menu_sY     = 1;

//The keymap array is used by the help menu and contains the keyboard key that is associated with the menu location.  
char keymap[5][9];

#define RRD if(show_rrd)  

double ignore_procdisk_threshold = 0.1;
double ignore_io_threshold      = 0.1;
/* Curses support */
#define CURSE if(cursed)  /* Only use this for single line curses calls */
#define COLOUR if(colour) /* Only use this for single line colour curses calls */
#ifdef NCURSES_MOUSE_VERSION
#define MOUSE if (mouse)  /* Check if ncurses mouse support is available */ 
MEVENT mouse_event;
int	mouse = 0;	/* 1 = ncurses mouse 
			   0 = ncurses doesn't support mouse */
#endif
int	cursed = 1;	/* 1 = using curses and 
			   0 = loging output for a spreadsheet */
int	colour = 1;	/* 1 = using colour curses and 
			   0 = using black and white curses  (see -b flag) */
#define MVPRINTW(row,col,string) {move((row),(col)); \
					attron(A_STANDOUT); \
					printw(string); \
					attroff(A_STANDOUT); }
FILE *fp;	/* filepointer for spreadsheet output */


char *timestamp(int loop, time_t eon)
{
static char string[64];
	if(show_rrd)
		sprintf(string,"%ld",(long)eon);
	else
		sprintf(string,"T%04d",loop);
	return string;
}
#define LOOP timestamp(loop,timer)

char *easy[5] = {"not found",0,0,0,0};
char *lsb_release[5] = {"not found",0,0,0,0};

void find_release()
{
FILE *pop;
int i;
char tmpstr[71];

	pop = popen("cat /etc/*ease 2>/dev/null", "r");
	if(pop != NULL) {
		tmpstr[0]=0;
	    	for(i=0;i<4;i++) {
			if(fgets(tmpstr, 70, pop) == NULL) 
				break;
			tmpstr[strlen(tmpstr)-1]=0; /* remove newline */
			easy[i] = malloc(strlen(tmpstr)+1);
			strcpy(easy[i],tmpstr);
		}
		pclose(pop);
	}
	pop = popen("/usr/bin/lsb_release -idrc 2>/dev/null", "r");
	if(pop != NULL) {
		tmpstr[0]=0;
	    	for(i=0;i<4;i++) {
			if(fgets(tmpstr, 70, pop) == NULL) 
				break;
			tmpstr[strlen(tmpstr)-1]=0; /* remove newline */
			lsb_release[i] = malloc(strlen(tmpstr)+1);
			strcpy(lsb_release[i],tmpstr);
		}
		pclose(pop);
	}
}

void helpmenuitem(WINDOW* pad, char *key, char *description, int x, int y, int enabled, int menu_Y, int menu_X){
	if (!strcmp(key, "space")){
		keymap[menu_X][menu_Y] = ' '; 
	} else{
		keymap[menu_X][menu_Y] = *key; 
	}
	COLOUR wattrset(pad, COLOR_PAIR(2) | A_BOLD); 
	mvwprintw(pad, x, y, "%s:", key);
	if (enabled){
		COLOUR wattrset(pad, COLOR_PAIR(2)); 
	} else{
		COLOUR wattrset(pad, COLOR_PAIR(0)); 
	}
	if (menu_sX == menu_X && menu_sY == menu_Y){
                wattron(pad,A_STANDOUT); 
	}
	mvwprintw(pad, x, y + strlen(key) + 2, "%s", description);
        wattroff(pad,A_STANDOUT); 
	COLOUR wattrset(pad, COLOR_PAIR(0)); 
}

void displayhelpmenu(WINDOW* pad){
			COLOUR wattrset(pad, COLOR_PAIR(0) | A_BOLD); 
                        #ifdef NCURSES_MOUSE_VERSION
                        if (mouse){
                                mvwprintw(pad, 0,0, "  ---Use keys below, arrow keys to select, or mouse to toggle stats on/off---");
                        }else{
                                mvwprintw(pad, 0, 0,  "  --- Use keys below or arrow keys to select to toggle statistics on/off ---");
                        }    
                        #else
                                mvwprintw(pad, 0, 0,  "  --- Use keys below or arrow keys to select to toggle statistics on/off ---");
                        #endif
			COLOUR wattrset(pad, COLOR_PAIR(0)); 

                        helpmenuitem(pad, "h", "Close Help ", 1, 1, 0, 1, 1 ); 
                        helpmenuitem(pad, "l", "Long Term CPU Avg ", 1, 15, enabled_option(SHOW_LONGTERM), 1, 2 ); 
                        helpmenuitem(pad, "m", "Memory/Swap Stats  ", 1, 36, enabled_option(SHOW_MEMORY), 1, 3 ); 
                        helpmenuitem(pad, "M", "Memory/Swap Graph", 1, 58, enabled_option(SHOW_MEMORY_GRAPH), 1, 4 ); 

                        helpmenuitem(pad, "V", "Virt Mem   ", 2, 1, enabled_option(SHOW_VM), 2, 1 ); 
                        helpmenuitem(pad, "n", "Network Stats     ", 2, 15, enabled_option(SHOW_NET), 2, 2 ); 
                        helpmenuitem(pad, "j", "Filesystem Usage   ", 2, 36, enabled_option(SHOW_JFS), 2, 3 ); 
                        helpmenuitem(pad, "k", "Kernel/Load Stats", 2, 58, enabled_option(SHOW_KERNEL), 2, 4 );

                        helpmenuitem(pad, "N", "NFS        ", 3, 1, enabled_option(SHOW_NFS), 3, 1 ); 
                        helpmenuitem(pad, "o", "Disks %Busy Map    ", 3, 15, enabled_option(SHOW_DISKMAP), 3, 2 ); 
                        if (show_disk_mode == SHOW_DISK_GRAPH) { 
                                helpmenuitem(pad, "d", "Disk I/O Graphs     ", 3, 36, 1, 3, 3 ); 
                        }else{
                                helpmenuitem(pad, "d", "Disk I/O Graphs     ", 3, 36, 0, 3, 3 ); 
                        }    
                        if (show_disk_mode == SHOW_DISK_STATS) { 
                                helpmenuitem(pad, "D", "Disk I/O Stats   ", 3, 58, 1, 3, 4 ); 
                        }else{
                                helpmenuitem(pad, "D", "Disk I/O Stats   ", 3, 58, 0, 3, 4 ); 
                        }

                        helpmenuitem(pad, "c", "CPU         ", 4, 1, enabled_option(SHOW_SMP), 4, 1 ); 
                        helpmenuitem(pad, "g", "Disk Groups       ", 4, 15, enabled_option(SHOW_DGROUP), 4, 2 ); 
                        helpmenuitem(pad, "r", "System Info        ", 4, 36, enabled_option(SHOW_CPU), 4, 3 ); 
                        helpmenuitem(pad, "v", "Verbose Display  ", 4, 58, enabled_option(SHOW_VERBOSE), 4, 4 ); 

			COLOUR wattrset(pad, COLOR_PAIR(0) | A_BOLD); 
                        mvwprintw(pad, 5, 0,  "                          --- Top Process Mode ---                                ");
			COLOUR wattrset(pad, COLOR_PAIR(0)); 

                        helpmenuitem(pad, "t", "Top Procs  ", 6, 1, enabled_option(SHOW_TOP), 5, 1 ); 
                        if (show_topmode == 1 && enabled_option(SHOW_TOP)) {
                                helpmenuitem(pad, "1", "Basic Top Mode    ", 6, 15, 1, 5, 2 ); 
                        } else{
                                helpmenuitem(pad, "1", "Basic Top Mode    ", 6, 15, 0, 5, 2 ); 
                        }    
                        if (show_topmode == 3 && enabled_option(SHOW_TOP)) {
                                helpmenuitem(pad, "3", "CPU Top Mode       ", 6, 36, 1, 5, 3 ); 
                        } else{
                                helpmenuitem(pad, "3", "CPU Top Mode       ", 6, 36, 0, 5, 3 ); 
                        }    
                        helpmenuitem(pad, "u", "Show Command Args", 6, 58, show_args, 5, 4 ); 

			COLOUR wattrset(pad, COLOR_PAIR(0) | A_BOLD); 
                        mvwprintw(pad, 7, 0,  "                              --- Controls ---                                ");
			COLOUR wattrset(pad, COLOR_PAIR(0)); 

                        helpmenuitem(pad, "q", "Quit       ", 8, 1, 0, 6, 1 ); 
                        helpmenuitem(pad, "b", "Black/White Mode  ", 8, 15, 0, 6, 2 ); 
                        helpmenuitem(pad, "C", "Single-Column View ", 8, 36, !show_columns, 6, 3 ); 
                        helpmenuitem(pad, "space", "Refresh Now  ", 8, 58, 0, 6, 4 ); 

                        helpmenuitem(pad, ".", "Only show busy disk/processes   ", 9, 1, !show_all, 7, 1 ); 
                        helpmenuitem(pad, "0", "Reset Peak Counts to Zero (peak = '>') ", 9, 36, 0, 7, 2 ); 

                        helpmenuitem(pad, "+", "Double Screen Refresh Time      ", 10, 1, 0, 8, 1 ); 
                        helpmenuitem(pad, "-", "Half Screen Refresh Time               ", 10, 36, 0, 8, 2 ); 
}

void display(WINDOW* pad, int rows) {
	if (show_columns) {
        	if (COLS >= 246){
        	        if (x_1+2+(rows)>LINES-1){
        	                if (x_2+2+(rows)>LINES-1) {
        	                        if (x_3+2+(rows)>LINES-1){
        	                                if (x_1 <= x_2 && x_1 <= x_3 ) {
        	                                        use_x_1++;
        	                                } else if (x_2 <= x_1 && x_2 <= x_3) {
        	                                        use_x_2++;
        	                                }else{
        	                                        use_x_3++;
        	                                }
        	                        }else{
        	                                use_x_3++;
        	                        }
        	                }else{
        	                        use_x_2++;
        	                }
        	        }else {
        	                use_x_1++;
        	        }
        	} else if (COLS >= 164) {
        	        if (x_1+2+(rows)>LINES-1){
        	                if (x_2+2+(rows)>LINES-1){
        	                        if (x_1 <= x_2) {
        	                                use_x_1++;
        	                        } else{
        	                                use_x_2++;
        	                        }
        	                }else{
        	                        use_x_2++;
        	                }
        	        }else{
        	                use_x_1++;
        	        }
        	}else{
        	        use_x_1++;
        	}

	        if (last_num_col == 1){
	                maxcols = COLS;
	        }else if (last_num_col == 2){
	                maxcols = COLS/2;
	        }else {
	                maxcols = COLS/3;
	        }
	}else{
		use_x_1++;
		maxcols = COLS;
	}


        if (use_x_1 > 0){
                if(x_1+2+(rows)>LINES)
                        pnoutrefresh(pad, 0,0,x_1,1,LINES-2,maxcols-2);
                else
                        pnoutrefresh(pad, 0,0,x_1,1,x_1+rows+1,maxcols-2);
                x_1=x_1+(rows);
                use_x_1 = 0;
        }
        if (use_x_2 > 0){
                if(x_2+2+(rows)>LINES) {
                        pnoutrefresh(pad, 0,0,x_2,maxcols,LINES-2,maxcols*2-2);
                }else {
                        pnoutrefresh(pad, 0,0,x_2,maxcols,x_2+rows+1,maxcols*2-2);
                }
                mvvline(1,maxcols-1,0,LINES-2);
                wnoutrefresh(stdscr);
                x_2=x_2+(rows);
                use_x_2 = 0;
        }
        if (use_x_3 > 0){
                if(x_3+2+(rows)>LINES) {
                        pnoutrefresh(pad, 0,0,x_3,maxcols*2,LINES-2,maxcols*3-2);
                }else {
                        pnoutrefresh(pad, 0,0,x_3,maxcols*2,x_3+rows+1,maxcols*3-2);
                }
                mvwvline(stdscr,1,maxcols*2-1,0,LINES-2);
                wnoutrefresh(stdscr);
                x_3=x_3+(rows);
                use_x_3 = 0;
        }

       	if (x_3 > 1 ){
       	        num_col = 3;
       	} else if (x_2 > 1){
       	        num_col = 2;
       	} else{
       	        num_col = 1;
       	}

        if(x_1+2>LINES || x_2+2>LINES || x_3+2>LINES) {
                mvwprintw(stdscr,LINES-1,10,"Warning: Some Statistics may not shown");
        }
}


/* Full Args Mode stuff here */

#define ARGSMAX 1024*8
#define CMDLEN 4096

struct {
	int pid;
	char *args;
} arglist[ARGSMAX];

void args_output(int pid, int loop, char *progname)
{
FILE *pop;
int i;
char tmpstr[CMDLEN];
static int arg_first_time = 1;

	if(pid == 0)
		return; /* ignore init */
	for(i=0;i<ARGSMAX-1;i++ ) {   /* clear data out */
		if(arglist[i].pid == pid){
			return;
		}
		if(arglist[i].pid == 0) /* got to empty slot */
			break;
	}
	sprintf(tmpstr,"ps -p %d -o args 2>/dev/null", pid);
	pop = popen(tmpstr, "r");
	if(pop == NULL) {
		return;
	} else {
		if(fgets(tmpstr, CMDLEN, pop) == NULL) { /* throw away header */
			pclose(pop);
			return;
		}
		tmpstr[0]=0;
		if(fgets(tmpstr, CMDLEN, pop) == NULL) {
			pclose(pop);
			return;
		}
		tmpstr[strlen(tmpstr)-1]=0;
		if(tmpstr[strlen(tmpstr)-1]== ' ')
			tmpstr[strlen(tmpstr)-1]=0;
		arglist[i].pid = pid;
		if(arg_first_time) {
			fprintf(fp,"UARG,+Time,PID,ProgName,FullCommand\n");
			arg_first_time = 0;
		}
		fprintf(fp,"UARG,%s,%07d,%s,%s\n",LOOP,pid,progname,tmpstr);
		pclose(pop);
		return;
	}
}

void args_load()
{
FILE *pop;
int i;
char tmpstr[CMDLEN];

	for(i=0;i<ARGSMAX;i++ ) {   /* clear data out */
		if(arglist[i].pid == -1)
			break;
		if(arglist[i].pid != 0){
			arglist[i].pid = -1;
			free(arglist[i].args);
		}
	}
	pop = popen("ps -eo pid,args 2>/dev/null", "r");
	if(pop == NULL) {
		return;
	} else {
		if(fgets(tmpstr, CMDLEN, pop) == NULL) { /* throw away header */
			pclose(pop);
			return;
		}
		for(i=0;i<ARGSMAX;i++ ) {
			tmpstr[0]=0;
			if(fgets(tmpstr, CMDLEN, pop) == NULL) {
				pclose(pop);
				return;
			}
			tmpstr[strlen(tmpstr)-1]=0;
			if(tmpstr[strlen(tmpstr)-1]== ' ')
				tmpstr[strlen(tmpstr)-1]=0;
			arglist[i].pid = atoi(tmpstr);
			arglist[i].args = malloc(strlen(tmpstr));
			strcpy(arglist[i].args,&tmpstr[6]);
		}
		pclose(pop);
	}
}

char *args_lookup(int pid, char *progname)
{
int i;
	for(i=0;i<ARGSMAX;i++) {
		if(arglist[i].pid == pid)
			return arglist[i].args;
		if(arglist[i].pid == -1)
			return progname;
	}
	return progname;
}
/* end args mode stuff here */

void   linux_bbbp(char *name, char *cmd, char *err)
{
        int   i;
        int   len;
#define STRLEN 4096
        char   str[STRLEN];
        FILE * pop;
        static int   lineno = 0;

        pop = popen(cmd, "r");
        if (pop == NULL) {
                fprintf(fp, "BBBP,%03d,%s failed to run %s\n", lineno++, cmd, err);
        } else {
                fprintf(fp, "BBBP,%03d,%s\n", lineno++, name);
                for (i = 0; i < 2048 && (fgets(str, STRLEN, pop) != NULL); i++) { /* 2048=sanity check only */
                        len = strlen(str);
			if(len>STRLEN) len=STRLEN;
                        if (str[len-1] == '\n') /*strip off the newline */
                                str[len-1] = 0;
                        /* fix lsconf style output so it does not confuse spread sheets */
                        if(str[0] == '+') str[0]='p';
                        if(str[0] == '*') str[0]='m';
                        if(str[0] == '-') str[0]='n';
                        if(str[0] == '/') str[0]='d';
                        if(str[0] == '=') str[0]='e';
                        fprintf(fp, "BBBP,%03d,%s,\"%s\"\n", lineno++, name, str);
                }
                pclose(pop);
        }
}

#define WARNING "needs root permission or file not present"

/* Global name of programme for printing it */
char	*progname;

/* Main data structure for collected stats.
 * Two versions are previous and current data.
 * Often its the difference that is printed.
 * The pointers are swaped i.e. current becomes the previous
 * and the previous over written rather than moving data around.
 */
struct cpu_stat {
	long long user;
	long long sys;
	long long wait; 
	long long idle;
	long long irq;
	long long softirq;
	long long steal;
	long long nice;
	long long intr;
	long long ctxt;
	long long btime;
	long long procs;
	long long running;
	long long blocked;
	float uptime;
	float idletime;
	float mins1;
	float mins5;
	float mins15;
};

#define ulong unsigned long
struct dsk_stat {	
	char	dk_name[32];
	int	dk_major;
	int	dk_minor;
	long	dk_noinfo;
	ulong	dk_reads;
	ulong	dk_rmerge;
	ulong	dk_rmsec;
	ulong	dk_rkb;
	ulong	dk_writes;
	ulong	dk_wmerge;
	ulong	dk_wmsec;
	ulong	dk_wkb;
	ulong	dk_xfers;
	ulong	dk_bsize;
	ulong	dk_time;
	ulong	dk_inflight;
	ulong	dk_11;
	ulong	dk_partition;
	ulong	dk_blocks; /* in /proc/partitions only */
	ulong	dk_use;
	ulong	dk_aveq;
};

struct mem_stat {
	long memtotal;
	long memfree;
	long memshared;
	long buffers;
	long cached;
	long swapcached;
	long active;
	long inactive;
	long hightotal;
	long highfree;
	long lowtotal;
	long lowfree;
	long swaptotal;
	long swapfree;
#ifdef LARGEMEM
	long dirty;
	long writeback;
	long mapped;
	long slab;
	long committed_as;
	long pagetables;
	long hugetotal;
	long hugefree;
	long hugesize;
#else
	long bigfree;
#endif /*LARGEMEM*/
};

struct vm_stat {
long long nr_dirty;
long long nr_writeback;
long long nr_unstable;
long long nr_page_table_pages;
long long nr_mapped;
long long nr_slab;
long long pgpgin;
long long pgpgout;
long long pswpin;
long long pswpout;
long long pgalloc_high;
long long pgalloc_normal;
long long pgalloc_dma;
long long pgfree;
long long pgactivate;
long long pgdeactivate;
long long pgfault;
long long pgmajfault;
long long pgrefill_high;
long long pgrefill_normal;
long long pgrefill_dma;
long long pgsteal_high;
long long pgsteal_normal;
long long pgsteal_dma;
long long pgscan_kswapd_high;
long long pgscan_kswapd_normal;
long long pgscan_kswapd_dma;
long long pgscan_direct_high;
long long pgscan_direct_normal;
long long pgscan_direct_dma;
long long pginodesteal;
long long slabs_scanned;
long long kswapd_steal;
long long kswapd_inodesteal;
long long pageoutrun;
long long allocstall;
long long pgrotated;
};



char *nfs_v2_names[18] = {
	"null", "getattr", "setattr", "root", "lookup", "readlink",
	"read", "wrcache", "write", "create", "remove", "rename",
	"link", "symlink", "mkdir", "rmdir", "readdir", "fsstat"};

char *nfs_v3_names[22] ={
	 "null", "getattr", "setattr", "lookup", "access", "readlink",
	 "read", "write", "create", "mkdir", "symlink", "mknod", 
	 "remove", "rmdir", "rename", "link", "readdir", "readdirplus",
	 "fsstat", "fsinfo", "pathconf", "commit"};

struct nfs_stat {
	long v2c[18];	/* verison2 client */
	long v3c[22];	/* verison3 client */
	long v2s[18];	/* verison2 server */
	long v3s[22];	/* verison3 server */
};

#define NETMAX 32
struct net_stat {
	unsigned long if_name[17];
	unsigned long long if_ibytes;
	unsigned long long if_obytes;
	unsigned long long if_ipackets;
	unsigned long long if_opackets;
	unsigned long if_ierrs;
	unsigned long if_oerrs;
	unsigned long if_idrop;   
	unsigned long if_ififo;   
	unsigned long if_iframe;   
	unsigned long if_odrop;   
	unsigned long if_ofifo;   
	unsigned long if_ocarrier;   
	unsigned long if_ocolls;   
} ;
#ifdef PARTITIONS
#define PARTMAX 256
struct part_stat {
	int part_major;
	int part_minor;
	unsigned long part_blocks;
	char part_name[16];
	unsigned long part_rio;
	unsigned long part_rmerge;
	unsigned long part_rsect;
	unsigned long part_ruse;
	unsigned long part_wio;
	unsigned long part_wmerge;
	unsigned long part_wsect;
	unsigned long part_wuse;
	unsigned long part_run;
	unsigned long part_use;
	unsigned long part_aveq;
};
#endif /*PARTITIONS*/


#ifdef POWER

int lparcfg_reread=1;

struct {
char version_string[16];		/*lparcfg 1.3 */
int version;
char serial_number[16];			/*HAL,0210033EA*/
char system_type[16];			/*HAL,9124-720*/
int  partition_id;			/*11*/
/* 
R4=0x14
R5=0x0
R6=0x800b0000
R7=0x1000000040004
*/
int BoundThrds;				/*=1*/
int CapInc;				/*=1*/
long long DisWheRotPer;			/*=2070000*/
int MinEntCap;				/*=10*/
int MinEntCapPerVP;			/*=10*/
int MinMem;				/*=2048*/
int DesMem;				/*=4096*/
int MinProcs;				/*=1*/
int partition_max_entitled_capacity;	/*=400*/
int system_potential_processors;	/*=4*/
		/**/
int partition_entitled_capacity;	/*=20*/
int system_active_processors;		/*=4*/
int pool_capacity;			/*=4*/
int unallocated_capacity_weight;	/*=0*/
int capacity_weight;			/*=0*/
int capped;				/*=1*/
int unallocated_capacity;		/*=0*/
long long pool_idle_time;		/*=0*/
long long pool_idle_saved;
long long pool_idle_diff;
int pool_num_procs;			/*=0*/
long long purr;				/*=0*/
long long purr_saved;
long long purr_diff;
long long timebase;
int partition_active_processors;	/*=1*/
int partition_potential_processors;	/*=40*/
int shared_processor_mode;		/*=1*/
} lparcfg;

int lpar_count=0;

#define LPAR_LINE_MAX   50
#define LPAR_LINE_WIDTH 80
char lpar_buffer[LPAR_LINE_MAX][LPAR_LINE_WIDTH];

int lpar_sanity=55;

char *locate(char *s)
{
int i;
int len;
	len=strlen(s);
	for(i=0;i<lpar_count;i++)
		if( !strncmp(s,lpar_buffer[i],len))
			return lpar_buffer[i];
	return "";
}

#define NUMBER_NOT_VALID -999

long long read_longlong(char *s)
{
long long x;
int ret;
int len;
int i;
char *str;
	str = locate(s);
	len=strlen(str);
	if(len == 0) {
		return NUMBER_NOT_VALID;
	}
	for(i=0;i<len;i++) {
		if(str[i] == '=') {
			ret = sscanf(&str[i+1], "%lld", &x);
			if(ret != 1) {
				fprintf(stderr,"sscanf for %s failed returned = %d line=%s\n", s, ret, str);
				return -1;
			}
			return x;
		}
	}
	fprintf(stderr,"read_long_long failed returned line=%s\n", str);
	return -2;
}


int proc_lparcfg()
{
static FILE *fp = (FILE *)-1;
char *str;
	if( fp == (FILE *)-1) {
           if( (fp = fopen("/proc/ppc64/lparcfg","r")) == NULL) {
		error("failed to open - /proc/ppc64/lparcfg");
		fp = (FILE *)-1;
		return 0;
	   }
	}

	for(lpar_count=0;lpar_count<LPAR_LINE_MAX-1;lpar_count++) {
		if(fgets(lpar_buffer[lpar_count],LPAR_LINE_WIDTH-1,fp) == NULL)
			break; 
	}
	if(lparcfg_reread) {
		fclose(fp);
		fp = (FILE *)-1;
	} else rewind(fp);

	str=locate("lparcfg");  	sscanf(str, "lparcfg %s", lparcfg.version_string);
	str=locate("serial_number");	sscanf(str, "serial_number=%s", lparcfg.serial_number);
	str=locate("system_type");	sscanf(str, "system_type=%s", lparcfg.system_type);

#define GETDATA(variable) lparcfg.variable = read_longlong( __STRING(variable) );

	GETDATA(partition_id);
	GETDATA(BoundThrds);
	GETDATA(CapInc);
	GETDATA(DisWheRotPer);
	GETDATA(MinEntCap);
	GETDATA(MinEntCapPerVP);
	GETDATA(MinMem);
	GETDATA(DesMem);
	GETDATA(MinProcs);
	GETDATA(partition_max_entitled_capacity);
	GETDATA(system_potential_processors);
	GETDATA(partition_entitled_capacity);
	GETDATA(system_active_processors);
	GETDATA(pool_capacity);
	GETDATA(unallocated_capacity_weight);
	GETDATA(capacity_weight);
	GETDATA(capped);
	GETDATA(unallocated_capacity);
	lparcfg.pool_idle_saved = lparcfg.pool_idle_time;
	GETDATA(pool_idle_time);
	lparcfg.pool_idle_diff = lparcfg.pool_idle_time - lparcfg.pool_idle_saved;
	GETDATA(pool_num_procs);
	lparcfg.purr_saved = lparcfg.purr;
	GETDATA(purr);
	lparcfg.purr_diff = lparcfg.purr - lparcfg.purr_saved;
	GETDATA(partition_active_processors);
	GETDATA(partition_potential_processors);
	GETDATA(shared_processor_mode);
	return 1;
}
#endif /*POWER*/


#define DISKMIN 256
#define DISKMAX diskmax
int diskmax = DISKMIN;

#define CPUMAX 128

struct data {
	struct dsk_stat *dk;
	struct cpu_stat cpu_total;
	struct cpu_stat cpuN[CPUMAX];
	struct mem_stat mem;
	struct vm_stat vm;
	struct nfs_stat nfs;
	struct net_stat ifnets[NETMAX];
#ifdef PARTITIONS
	struct part_stat parts[PARTMAX];
#endif /*PARTITIONS*/

	struct timeval tv;
	double time;
	struct procsinfo *procs;

	int    nprocs;
} database[2], *p, *q;


long long read_vmline(FILE *fp, char  *s)
{
char buffer[4096];
int len;
 int ret;
long long var;

	if(fgets(buffer,4096-1,fp) == NULL)
		return -1;
	len = strlen(s) +1;
	var = -1;
	ret = sscanf(&buffer[len],"%lld", &var);
	if(ret == 1)
		return var;
	else
		return -1;
}

#define GETVM(variable) p->vm.variable = read_vmline(fp, __STRING(variable) );

int read_vmstat()
{
static FILE *fp = (FILE *)-1;

	if( fp == (FILE *)-1) {
           if( (fp = fopen("/proc/vmstat","r")) == NULL) {
		error("failed to open - /proc/vmstat");
		fp = (FILE *)-1;
		return -1;
	   }
	}
	GETVM(nr_dirty);
	GETVM(nr_writeback);
	GETVM(nr_unstable);
	GETVM(nr_page_table_pages);
	GETVM(nr_mapped);
	GETVM(nr_slab);
	GETVM(pgpgin);
	GETVM(pgpgout);
	GETVM(pswpin);
	GETVM(pswpout);
	GETVM(pgalloc_high);
	GETVM(pgalloc_normal);
	GETVM(pgalloc_dma);
	GETVM(pgfree);
	GETVM(pgactivate);
	GETVM(pgdeactivate);
	GETVM(pgfault);
	GETVM(pgmajfault);
	GETVM(pgrefill_high);
	GETVM(pgrefill_normal);
	GETVM(pgrefill_dma);
	GETVM(pgsteal_high);
	GETVM(pgsteal_normal);
	GETVM(pgsteal_dma);
	GETVM(pgscan_kswapd_high);
	GETVM(pgscan_kswapd_normal);
	GETVM(pgscan_kswapd_dma);
	GETVM(pgscan_direct_high);
	GETVM(pgscan_direct_normal);
	GETVM(pgscan_direct_dma);
	GETVM(pginodesteal);
	GETVM(slabs_scanned);
	GETVM(kswapd_steal);
	GETVM(kswapd_inodesteal);
	GETVM(pageoutrun);
	GETVM(allocstall);
	GETVM(pgrotated);
	fclose(fp);
	fp=(FILE *)-1;
/*
	rewind(fp);
*/
	return 1;
}


/* These macro simplify the access to the Main data structure */
#define DKDELTA(member) ( (q->dk[i].member > p->dk[i].member) ? 0 : (p->dk[i].member - q->dk[i].member))
#define SIDELTA(member) ( (q->si.member > p->si.member)       ? 0 : (p->si.member - q->si.member))

#define IFNAME 64

#define TIMEDELTA(member,index1,index2) ((p->procs[index1].member) - (q->procs[index2].member))
#define COUNTDELTA(member) ( (q->procs[topper[j].other].member > p->procs[i].member) ? 0 : (p->procs[i].member  - q->procs[topper[j].other].member) )

#define TIMED(member) ((double)(p->procs[i].member.tv_sec)) 

double mem_peak = 0;
double swap_peak = 0;
double *cpu_peak; /* ptr to array  - 1 for each cpu - 0 = average for machine */
double *disk_busy_peak;
double *disk_rate_peak;
double net_read_peak[NETMAX];
double net_write_peak[NETMAX];
int aiorunning;
int aiorunning_max = 0;
int aiocount;
int aiocount_max = 0;
float aiotime;
float aiotime_max =0.0;

char *dskgrp(int i)
{
static char error_string[] = { "Too-Many-Disks" };
static char *string[16] = {"",   "1",  "2",  "3", 
			   "4",  "5",  "6",  "7", 
			   "8",  "9",  "10", "11", 
			   "12", "13", "14", "15"};

	i = (int)((float)i/(float)disks_per_line);
	if(0 <= i && i <= 15 ) 
		return string[i];
	return error_string;
}

/* command checking against a list */

#define CMDMAX 64

char *cmdlist[CMDMAX];
int cmdfound = 0;

int cmdcheck(char *cmd)
{
	int i;
#ifdef CMDDEBUG
	fprintf(stderr,"cmdfound=%d\n",cmdfound);
	for(i=0;i<cmdfound;i++)
		fprintf(stderr,"cmdlist[%d]=\"%s\"\n",i,cmdlist[i]);
#endif /* CMDDEBUG */
	for(i=0;i<cmdfound;i++) {
		if(strlen(cmdlist[i]) == 0)
			continue;
		if( !strncmp(cmdlist[i],cmd,strlen(cmdlist[i])) )
			return 1;
	}
	return 0;
}

/* Convert secs + micro secs to a double */
double	doubletime(void)
{

	gettimeofday(&p->tv, 0);
	return((double)p->tv.tv_sec + p->tv.tv_usec * 1.0e-6);
}

int stat8 = 0; /* used to determine the number of variables on a line */

void proc_cpu()
{
int i;
static int intr_line = 0;
static int ctxt_line = 0;
static int btime_line= 0;
static int proc_line = 0;
static int run_line  = 0;
static int block_line= 0;
static int proc_cpu_first_time = 1;
long long user;
long long nice;
long long sys;
long long idle;
long long iowait;
long long hardirq;
long long softirq;
long long steal;

	if(proc_cpu_first_time) {
		stat8 = sscanf(&proc[P_STAT].line[0][5], "%lld %lld %lld %lld %lld %lld %lld %lld", 
			&user,
			&nice,
			&sys,
			&idle,
			&iowait,
			&hardirq,
			&softirq,
			&steal);
		proc_cpu_first_time = 0;
	}
	user = nice = sys = idle = iowait = hardirq = softirq = steal = 0;
	if(stat8 == 8) {
		sscanf(&proc[P_STAT].line[0][5], "%lld %lld %lld %lld %lld %lld %lld %lld", 
			&user,
			&nice,
			&sys,
			&idle,
			&iowait,
			&hardirq,
			&softirq,
			&steal);
	} else { /* stat 4 variables here as older Linux proc */
		sscanf(&proc[P_STAT].line[0][5], "%lld %lld %lld %lld", 
			&user,
			&nice,
			&sys,
			&idle);
	}
	p->cpu_total.user = user + nice;
	p->cpu_total.wait = iowait; /* in the case of 4 variables = 0 */
	p->cpu_total.sys  = sys;
	/* p->cpu_total.sys  = sys + hardirq + softirq + steal;*/
	p->cpu_total.idle = idle;
	
	p->cpu_total.irq     = hardirq;
	p->cpu_total.softirq = softirq;
	p->cpu_total.steal   = steal;
	p->cpu_total.nice    = nice;
#ifdef DEBUG
	if(debug)fprintf(stderr,"XX user=%lld wait=%lld sys=%lld idle=%lld\n",
			p->cpu_total.user,
			p->cpu_total.wait,
			p->cpu_total.sys,
			p->cpu_total.idle);
#endif /*DEBUG*/

	for(i=0;i<cpus;i++ ) {
	    user = nice = sys = idle = iowait = hardirq = softirq = steal = 0;
	    if(stat8 == 8) {
		sscanf(&proc[P_STAT].line[i+1][5], 
			"%lld %lld %lld %lld %lld %lld %lld %lld", 
		&user,
		&nice,
		&sys,
		&idle,
		&iowait,
		&hardirq,
		&softirq,
		&steal);
	    } else {
		sscanf(&proc[P_STAT].line[i+1][5], "%lld %lld %lld %lld", 
		&user,
		&nice,
		&sys,
		&idle);
	    }
		p->cpuN[i].user = user + nice;
		p->cpuN[i].wait = iowait;
		p->cpuN[i].sys  = sys;
		/*p->cpuN[i].sys  = sys + hardirq + softirq + steal;*/
		p->cpuN[i].idle = idle;

		p->cpuN[i].irq     = hardirq;
		p->cpuN[i].softirq = softirq;
		p->cpuN[i].steal   = steal;
		p->cpuN[i].nice    = nice;
	}

	if(intr_line == 0) {
		if(proc[P_STAT].line[i+1][0] == 'p' &&
		   proc[P_STAT].line[i+1][1] == 'a' &&
		   proc[P_STAT].line[i+1][2] == 'g' &&
		   proc[P_STAT].line[i+1][3] == 'e' ) {
			/* 2.4 kernel */
			intr_line = i+3;
			ctxt_line = i+5;
			btime_line= i+6;
			proc_line = i+7;
			run_line  = i+8;
			block_line= i+9;
		}else {
			/* 2.6 kernel */
			intr_line = i+1;
			ctxt_line = i+2;
			btime_line= i+3;
			proc_line = i+4;
			run_line  = i+5;
			block_line= i+6;
		}
	}
	p->cpu_total.intr = -1;
	p->cpu_total.ctxt = -1;
	p->cpu_total.btime = -1;
	p->cpu_total.procs = -1;
	p->cpu_total.running = -1;
	p->cpu_total.blocked = -1;
	if(proc[P_STAT].lines >= intr_line)
	sscanf(&proc[P_STAT].line[intr_line][0], "intr %lld", &p->cpu_total.intr);
	if(proc[P_STAT].lines >= ctxt_line)
	sscanf(&proc[P_STAT].line[ctxt_line][0], "ctxt %lld", &p->cpu_total.ctxt);
	if(proc[P_STAT].lines >= btime_line)
	sscanf(&proc[P_STAT].line[btime_line][0], "btime %lld", &p->cpu_total.btime);
	if(proc[P_STAT].lines >= proc_line)
	sscanf(&proc[P_STAT].line[proc_line][0], "processes %lld", &p->cpu_total.procs);
	if(proc[P_STAT].lines >= run_line)
	sscanf(&proc[P_STAT].line[run_line][0], "procs_running %lld", &p->cpu_total.running);
	if(proc[P_STAT].lines >= block_line)
	sscanf(&proc[P_STAT].line[block_line][0], "procs_blocked %lld", &p->cpu_total.blocked);
}

void proc_nfs()
{
int i;
int j;
    if(proc[P_NFS].fp != 0) {
	/* line readers "proc2 18 num num etc" */
	for(j=0,i=8;i<strlen(proc[P_NFS].line[2]);i++) {
		if(proc[P_NFS].line[2][i] == ' ') {
			p->nfs.v2c[j] =atol(&proc[P_NFS].line[2][i+1]);
			j++;
		}
	}
	/* line readers "proc3 22 num num etc" */
	for(j=0,i=8;i<strlen(proc[P_NFS].line[3]);i++) {
		if(proc[P_NFS].line[3][i] == ' ') {
			p->nfs.v3c[j] =atol(&proc[P_NFS].line[3][i+1]);
			j++;
		}
	}
    }
	/* line readers "proc2 18 num num etc" */
    if(proc[P_NFSD].fp != 0) {
	for(j=0,i=8;i<strlen(proc[P_NFSD].line[7]);i++) {
		if(proc[P_NFSD].line[2][i] == ' ') {
			p->nfs.v2s[j] =atol(&proc[P_NFSD].line[2][i+1]);
			j++;
		}
	}
	/* line readers "proc3 22 num num etc" */
	for(j=0,i=8;i<strlen(proc[P_NFSD].line[8]);i++) {
		if(proc[P_NFS].line[3][i] == ' ') {
			p->nfs.v3s[j] =atol(&proc[P_NFSD].line[3][i+1]);
			j++;
		}
	}
    }
}

void proc_kernel()
{
int i;
	p->cpu_total.uptime=0.0;
	p->cpu_total.idletime=0.0;
	p->cpu_total.uptime=atof(proc[P_UPTIME].line[0]);
	for(i=0;i<strlen(proc[P_UPTIME].line[0]);i++) {
		if(proc[P_UPTIME].line[0][i] == ' ') {
			p->cpu_total.idletime=atof(&proc[P_UPTIME].line[0][i+1]);
			break;
		}
	}

        sscanf(&proc[P_LOADAVG].line[0][0], "%f %f %f", 
		&p->cpu_total.mins1,
		&p->cpu_total.mins5,
		&p->cpu_total.mins15);

}

char *proc_find_sb(char * p)
{
	for(; *p != 0;p++)
		if(*p == ' ' && *(p+1) == '(')
			return p;
	return 0;
}

#define DISK_MODE_IO 1
#define DISK_MODE_DISKSTATS 2
#define DISK_MODE_PARTITIONS 3

int disk_mode = 0;

void proc_disk_io(double elapsed)
{
int diskline;
int i;
int ret;
char *str;
int fudged_busy;

	disks = 0;
	for(diskline=0;diskline<proc[P_STAT].lines;diskline++) {
		if(strncmp("disk_io", proc[P_STAT].line[diskline],7) == 0) 
			break;
	}
	for(i=8;i<strlen(proc[P_STAT].line[diskline]);i++) {
		if( proc[P_STAT].line[diskline][i] == ':')
			disks++;
	}

	str=&proc[P_STAT].line[diskline][0];
	for(i=0;i<disks;i++) {
		str=proc_find_sb(str);
		if(str == 0)
			break;
		ret = sscanf(str, " (%d,%d):(%ld,%ld,%ld,%ld,%ld", 
			&p->dk[i].dk_major,
			&p->dk[i].dk_minor,
			&p->dk[i].dk_noinfo,
			&p->dk[i].dk_reads,
			&p->dk[i].dk_rkb,
			&p->dk[i].dk_writes,
			&p->dk[i].dk_wkb);
		if(ret != 7)
			exit(7);
		p->dk[i].dk_xfers = p->dk[i].dk_noinfo;
		/* blocks  are 512 bytes*/
		p->dk[i].dk_rkb = p->dk[i].dk_rkb/2;
		p->dk[i].dk_wkb = p->dk[i].dk_wkb/2;

		p->dk[i].dk_bsize = (p->dk[i].dk_rkb+p->dk[i].dk_wkb)/p->dk[i].dk_xfers*1024;

		/* assume a disk does 200 op per second */
		fudged_busy = (p->dk[i].dk_reads + p->dk[i].dk_writes)/2;
		if(fudged_busy > 100*elapsed)
			p->dk[i].dk_time += 100*elapsed;
		p->dk[i].dk_time = fudged_busy;

		sprintf(p->dk[i].dk_name,"dev-%d-%d",p->dk[i].dk_major,p->dk[i].dk_minor);
/*	fprintf(stderr,"disk=%d name=\"%s\" major=%d minor=%d\n", i,p->dk[i].dk_name, p->dk[i].dk_major,p->dk[i].dk_minor); */
		str++;
	}
}

void proc_diskstats(double elapsed)
{
static FILE *fp = (FILE *)-1;
char buf[1024];
int i;
int ret;

	if( fp == (FILE *)-1) {
           if( (fp = fopen("/proc/diskstats","r")) == NULL) {
		error("failed to open - /proc/diskstats");
		disks=0;
		return;
	   }
	}
/*
   2    0 fd0 1 0 2 13491 0 0 0 0 0 13491 13491
   3    0 hda 41159 53633 1102978 620181 39342 67538 857108 4042631 0 289150 4668250
   3    1 hda1 58209 58218 0 0
   3    2 hda2 148 4794 10 20
   3    3 hda3 65 520 0 0
   3    4 hda4 35943 1036092 107136 857088
  22    0 hdc 167 5394 22308 32250 0 0 0 0 0 22671 32250 <-- USB !!
   8    0 sda 990 2325 4764 6860 9 3 12 417 0 6003 7277
   8    1 sda1 3264 4356 12 12
*/
	for(i=0;i<DISKMAX;) {
		if(fgets(buf,1024,fp) == NULL)
			break;
		/* zero the data ready for reading */
		p->dk[i].dk_major = 
		p->dk[i].dk_minor =
		p->dk[i].dk_name[0] =
		p->dk[i].dk_reads =
		p->dk[i].dk_rmerge =
		p->dk[i].dk_rkb =
		p->dk[i].dk_rmsec =
		p->dk[i].dk_writes =
		p->dk[i].dk_wmerge =
		p->dk[i].dk_wkb =
		p->dk[i].dk_wmsec =
		p->dk[i].dk_inflight =
		p->dk[i].dk_time =
		p->dk[i].dk_11 =0;

		ret = sscanf(&buf[0], "%d %d %s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
			&p->dk[i].dk_major,
			&p->dk[i].dk_minor,
			&p->dk[i].dk_name[0],
			&p->dk[i].dk_reads,
			&p->dk[i].dk_rmerge,
			&p->dk[i].dk_rkb,
			&p->dk[i].dk_rmsec,
			&p->dk[i].dk_writes,
			&p->dk[i].dk_wmerge,
			&p->dk[i].dk_wkb,
			&p->dk[i].dk_wmsec,
			&p->dk[i].dk_inflight,
			&p->dk[i].dk_time,
			&p->dk[i].dk_11 );
		if(ret == 7) { /* suffle the data around due to missing columns for partitions */
			p->dk[i].dk_partition = 1;
			p->dk[i].dk_wkb = p->dk[i].dk_rmsec;
			p->dk[i].dk_writes = p->dk[i].dk_rkb;
			p->dk[i].dk_rkb = p->dk[i].dk_rmerge;
			p->dk[i].dk_rmsec=0;
			p->dk[i].dk_rmerge=0;
	
		}
		else if(ret == 14) p->dk[i].dk_partition = 0;
		else fprintf(stderr,"disk sscanf wanted 14 but returned=%d line=%s\n", 
	 			ret,buf);

		p->dk[i].dk_rkb /= 2; /* sectors = 512 bytes */
		p->dk[i].dk_wkb /= 2;
		/*p->dk[i].dk_xfers = p->dk[i].dk_rkb + p->dk[i].dk_wkb;*/
		p->dk[i].dk_xfers = p->dk[i].dk_reads + p->dk[i].dk_writes;
		if(p->dk[i].dk_xfers == 0)
			p->dk[i].dk_bsize = 0;
		else
			p->dk[i].dk_bsize = (p->dk[i].dk_rkb+p->dk[i].dk_wkb)/p->dk[i].dk_xfers*1024;

		p->dk[i].dk_time /= 10.0; /* in milli-seconds to make it upto 100%, 1000/100 = 10 */
	
		if(p->dk[i].dk_reads != 0 || p->dk[i].dk_writes != 0) 
			i++;	
	}
	if(reread) {
		fclose(fp);
		fp = (FILE *)-1;
	} else rewind(fp);
	disks = i;
}

void strip_spaces(char *s)
{
char *p;
int spaced=1;

	p=s;
	for(p=s;*p!=0;p++) {
		if(*p == ':')
			*p=' ';
		if(*p != ' ') {
			*s=*p;
			s++;
			spaced=0;
		} else if(spaced) {
			/* do no thing as this is second space */
			} else {
				*s=*p;
				s++;
				spaced=1;
			}

	}
	*s = 0;
}

void proc_partitions(double elapsed)
{
static FILE *fp = (FILE *)-1;
char buf[1024];
int i = 0;
int ret;

	if( fp == (FILE *)-1) {
           if( (fp = fopen("/proc/partitions","r")) == NULL) {
		error("failed to open - /proc/partitions");
		partitions=0;
		return;
	   }
	}
	if(fgets(buf,1024,fp) == NULL) goto end; /* throw away the header lines */
	if(fgets(buf,1024,fp) == NULL) goto end;
/*
major minor  #blocks  name     rio rmerge rsect ruse wio wmerge wsect wuse running use aveq

  33     0    1052352 hde 2855 15 2890 4760 0 0 0 0 -4 7902400 11345292
  33     1    1050304 hde1 2850 0 2850 3930 0 0 0 0 0 3930 3930
   3     0   39070080 hda 9287 19942 226517 90620 8434 25707 235554 425790 -12 7954830 33997658
   3     1   31744408 hda1 651 90 5297 2030 0 0 0 0 0 2030 2030
   3     2    6138720 hda2 7808 19561 218922 79430 7299 20529 222872 241980 0 59950 321410
   3     3     771120 hda3 13 41 168 80 0 0 0 0 0 80 80
   3     4          1 hda4 0 0 0 0 0 0 0 0 0 0 0
   3     5     408208 hda5 812 241 2106 9040 1135 5178 12682 183810 0 11230 192850
*/
	for(i=0;i<DISKMAX;i++) {
		if(fgets(buf,1024,fp) == NULL)
			break;
		strip_spaces(buf);
		ret = sscanf(&buf[0], "%d %d %lu %s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
			&p->dk[i].dk_major,
			&p->dk[i].dk_minor,
			&p->dk[i].dk_blocks,
		(char *)&p->dk[i].dk_name,
			&p->dk[i].dk_reads,
			&p->dk[i].dk_rmerge,
			&p->dk[i].dk_rkb,
			&p->dk[i].dk_rmsec,
			&p->dk[i].dk_writes,
			&p->dk[i].dk_wmerge,
			&p->dk[i].dk_wkb,
			&p->dk[i].dk_wmsec,
			&p->dk[i].dk_inflight,
			&p->dk[i].dk_use,
			&p->dk[i].dk_aveq
			);
		p->dk[i].dk_rkb /= 2; /* sectors = 512 bytes */
		p->dk[i].dk_wkb /= 2;
		p->dk[i].dk_xfers = p->dk[i].dk_rkb + p->dk[i].dk_wkb;
		if(p->dk[i].dk_xfers == 0)
			p->dk[i].dk_bsize = 0;
		else
			p->dk[i].dk_bsize = (p->dk[i].dk_rkb+p->dk[i].dk_wkb)/p->dk[i].dk_xfers*1024;

		p->dk[i].dk_time /= 10.0; /* in milli-seconds to make it upto 100%, 1000/100 = 10 */
	
		if(ret != 15) {
#ifdef DEBUG
			if(debug)fprintf(stderr,"sscanf wanted 15 returned = %d line=%s\n", ret,buf);
#endif /*DEBUG*/
			partitions_short = 1;
		} else partitions_short = 0;
	}
	end:
	if(reread) {
		fclose(fp);
		fp = (FILE *)-1;
	} else rewind(fp);
	disks = i;
}

void proc_disk(double elapsed)
{
struct stat buf;
int ret;
	if(disk_mode == 0) {
		ret = stat("/proc/diskstats", &buf);
		if(ret == 0) {
			disk_mode=DISK_MODE_DISKSTATS;
		} else {
	               ret = stat("/proc/partitions", &buf);
			if(ret == 0) {
				disk_mode=DISK_MODE_PARTITIONS;
			} else {
				disk_mode=DISK_MODE_IO;
			}
		}
	}
	switch(disk_mode){
	case DISK_MODE_IO: 		proc_disk_io(elapsed);   break;
	case DISK_MODE_DISKSTATS: 	proc_diskstats(elapsed); break;
	case DISK_MODE_PARTITIONS: 	proc_partitions(elapsed); break;
	}
}
#undef isdigit
#define isdigit(ch) ( ( '0' <= (ch)  &&  (ch) >= '9')? 0: 1 )

long proc_mem_search( char *s)
{
int i;
int j;
int len;
	len=strlen(s);
	for(i=0;i<proc[P_MEMINFO].lines;i++ ) {
		if( !strncmp(s, proc[P_MEMINFO].line[i],len) ) {
			for(j=len;
				!isdigit(proc[P_MEMINFO].line[i][j]) &&
				proc[P_MEMINFO].line[i][j] != 0;
				j++)
				/* do nothing */ ;
			return atol( &proc[P_MEMINFO].line[i][j]);
		}
	}
	return -1;
}

void proc_mem()
{
	p->mem.memtotal   = proc_mem_search("MemTotal");
	p->mem.memfree    = proc_mem_search("MemFree");
	p->mem.memshared  = proc_mem_search("MemShared");
	p->mem.buffers    = proc_mem_search("Buffers");
	p->mem.cached     = proc_mem_search("Cached");
	p->mem.swapcached = proc_mem_search("SwapCached");
	p->mem.active     = proc_mem_search("Active");
	p->mem.inactive   = proc_mem_search("Inactive");
	p->mem.hightotal  = proc_mem_search("HighTotal");
	p->mem.highfree   = proc_mem_search("HighFree");
	p->mem.lowtotal   = proc_mem_search("LowTotal");
	p->mem.lowfree    = proc_mem_search("LowFree");
	p->mem.swaptotal  = proc_mem_search("SwapTotal");
	p->mem.swapfree   = proc_mem_search("SwapFree");
#ifdef LARGEMEM
	p->mem.dirty         = proc_mem_search("Dirty");
	p->mem.writeback     = proc_mem_search("Writeback");
	p->mem.mapped        = proc_mem_search("Mapped");
	p->mem.slab          = proc_mem_search("Slab");
	p->mem.committed_as  = proc_mem_search("Committed_AS");
	p->mem.pagetables    = proc_mem_search("PageTables");
	p->mem.hugetotal     = proc_mem_search("HugePages_Total");
	p->mem.hugefree      = proc_mem_search("HugePages_Free");
	p->mem.hugesize      = proc_mem_search("Hugepagesize");
#else
	p->mem.bigfree       = proc_mem_search("BigFree");
#endif /*LARGEMEM*/
}

int current_snaps;
#define MAX_SNAPS 400
#define MAX_SNAP_ROWS 20
#define SNAP_OFFSET 6


int next_cpu_snap = 0;
int cpu_snap_all = 0;

struct {
	double user;
	double kernel;
	double iowait;
	double idle;
} cpu_snap[MAX_SNAPS];

int snap_average()
{
int i;
int end;
int total = 0;

	if(cpu_snap_all)
		end = current_snaps;
	else
		end = next_cpu_snap;

	for(i=0;i<end;i++) {
		total = total + cpu_snap[i].user + cpu_snap[i].kernel;
	}
	return (total / end) ;
}

void snap_clear()
{
int i;
	for(i=0;i<current_snaps;i++) {
		cpu_snap[i].user = 0;
		cpu_snap[i].kernel = 0;
		cpu_snap[i].iowait = 0;
		cpu_snap[i].idle = 0;
	}
	next_cpu_snap=0;
	cpu_snap_all=0;
}

void plot_snap(WINDOW *pad)
{
int i;
int j;
	if (cursed) {
		mvwprintw(pad,0, 0, " CPU +");
		int counter;
		for (counter=0; counter < current_snaps - 1; counter++){
			wprintw(pad, "-");
		}
		wprintw(pad, "+");
		mvwprintw(pad,1, 0,"100%%-|");
		mvwprintw(pad,2, 1, "95%%-|");
		mvwprintw(pad,3, 1, "90%%-|");
		mvwprintw(pad,4, 1, "85%%-|");
		mvwprintw(pad,5, 1, "80%%-|");
		mvwprintw(pad,6, 1, "75%%-|");
		mvwprintw(pad,7, 1, "70%%-|");
		mvwprintw(pad,8, 1, "65%%-|");
		mvwprintw(pad,9, 1, "60%%-|");
		mvwprintw(pad,10, 1, "55%%-|");
		mvwprintw(pad,11, 1, "50%%-|");
		mvwprintw(pad,12, 1, "45%%-|");
		mvwprintw(pad,13, 1, "40%%-|");
		mvwprintw(pad,14, 1, "35%%-|");
		mvwprintw(pad,15, 1, "30%%-|");
		mvwprintw(pad,16, 1, "25%%-|");
		mvwprintw(pad,17, 1, "20%%-|");
		mvwprintw(pad,18, 1,"15%%-|");
		mvwprintw(pad,19, 1,"10%%-|");
		mvwprintw(pad,20, 1," 5%%-|");

 		if (colour){
 			mvwprintw(pad,21, 4, " +--------------------");
 			COLOUR wattrset(pad, COLOR_PAIR(2));
 			mvwprintw(pad,21, 26, "User%%");
 			COLOUR wattrset(pad, COLOR_PAIR(0));
 			mvwprintw(pad,21, 30, "---------");
 			COLOUR wattrset(pad, COLOR_PAIR(1));
 			mvwprintw(pad,21, 39, "System%%");
 			COLOUR wattrset(pad, COLOR_PAIR(0));
 			mvwprintw(pad,21, 45, "---------");
 			COLOUR wattrset(pad, COLOR_PAIR(4));
 			mvwprintw(pad,21, 54, "Wait%%");
 			COLOUR wattrset(pad, COLOR_PAIR(0));
			for (counter=0; counter < current_snaps - 54; counter++){
				wprintw(pad, "-");
			}
 			wprintw(pad, "+");
		} else {
			mvwprintw(pad,21, 4, " +");
			for (counter=0; counter < current_snaps - 1; counter++){
				wprintw(pad, "-");
			}
			wprintw(pad, "+");
		}

		for (j = 0; j < current_snaps; j++) {
			for (i = 0; i < MAX_SNAP_ROWS; i++) {
				wmove(pad,MAX_SNAP_ROWS-i, j+SNAP_OFFSET);
				if( (cpu_snap[j].user / 100 * MAX_SNAP_ROWS) > i+0.5) {
					COLOUR wattrset(pad,COLOR_PAIR(9));
					wprintw(pad,"U");
					COLOUR wattrset(pad,COLOR_PAIR(0));
				} else if( (cpu_snap[j].user + cpu_snap[j].kernel )/ 100 * MAX_SNAP_ROWS > i+0.5) {
					COLOUR wattrset(pad,COLOR_PAIR(8));
					wprintw(pad,"s");
					COLOUR wattrset(pad,COLOR_PAIR(0));
				} else if( (cpu_snap[j].user + cpu_snap[j].kernel +cpu_snap[j].iowait )/ 100 * MAX_SNAP_ROWS > i+0.5) {
					COLOUR wattrset(pad,COLOR_PAIR(10));
					wprintw(pad,"w");
					COLOUR wattrset(pad,COLOR_PAIR(0));
				} else 
					wprintw(pad," ");
			}
		}
		for (i = 0; i < MAX_SNAP_ROWS; i++) {
			wmove(pad,MAX_SNAP_ROWS-i, next_cpu_snap+SNAP_OFFSET);
			wprintw(pad,"|");
		}
		wmove(pad,MAX_SNAP_ROWS+1 - (snap_average() /5), next_cpu_snap+SNAP_OFFSET);
		wprintw(pad,"+");
		if(dotline) {
		    for (i = 0; i < current_snaps; i++) {
			wmove(pad,MAX_SNAP_ROWS+1-dotline*2, i+SNAP_OFFSET);
			wprintw(pad,"+");
		    }
		    dotline = 0;
		}
	}
}

/* This saves the CPU overall usage for later ploting on the screen */
void plot_save(double user, double kernel, double iowait, double idle)
{
	cpu_snap[next_cpu_snap].user = user;
	cpu_snap[next_cpu_snap].kernel = kernel;
	cpu_snap[next_cpu_snap].iowait = iowait;
	cpu_snap[next_cpu_snap].idle = idle;
	next_cpu_snap++;
	if(next_cpu_snap >= current_snaps) {
		next_cpu_snap=0;
		cpu_snap_all=1;
	}
}

/* This puts the CPU usage on the screen and draws the CPU graphs or outputs to the file */

void save_smp(WINDOW *pad, int cpu_no, int row, long user, long kernel, long iowait, long idle, long nice, long irq, long softirq, long steal)
{
static int firsttime = 1;
	if (cursed) {
		mvwprintw(pad,row,0, "%02d usr=%4ld sys=%4ld wait=%4ld idle=%4ld steal=%2ld nice=%4ld irq=%2ld sirq=%2ld\n", 
		cpu_no, user, kernel, iowait, idle, steal, nice, irq, softirq, steal);
		return;
	}
	if(firsttime) {
		fprintf(fp,"CPUTICKS_ALL,AAA,user,sys,wait,idle,nice,irq,softirq,steal\n");
		fprintf(fp,"CPUTICKS%02d,AAA,user,sys,wait,idle,nice,irq,softirq,steal\n", cpu_no);
		firsttime=0;	
	}
	if(cpu_no==0) {
	fprintf(fp,"CPUTICKS_ALL,%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n", 
	        LOOP, user, kernel, iowait, idle, nice, irq, softirq, steal);
	} else {
	fprintf(fp,"CPUTICKS%02d,%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n", 
	cpu_no, LOOP, user, kernel, iowait, idle, nice, irq, softirq, steal);
	}
}

void plot_smp(WINDOW *pad, int cpu_no, int row, double user, double kernel, double iowait, double idle)
{
	int	i;
	int	peak_col;

	if(show_rrd) return;

	if(cpu_peak[cpu_no] < ((double)((int)user/2 + (int)kernel/2 + (int)iowait/2)*2.0) )
		cpu_peak[cpu_no] = (double)((int)user/2 + (int)kernel/2 + (int)iowait/2)*2.0;

	if (cursed) {
		if(cpu_no == 0)
			mvwprintw(pad,row, 0, "Avg");
		else
			mvwprintw(pad,row, 0, "%2d", cpu_no);
		mvwprintw(pad,row,  3, "%5.1lf", user);
		mvwprintw(pad,row,  9, "%5.1lf", kernel);
		mvwprintw(pad,row, 15, "%5.1lf", iowait);
		mvwprintw(pad,row, 22, "%5.1lf", idle);
		mvwprintw(pad,row, 27, "|                                                  ");
		wmove(pad,row, 28);
		for (i = 0; i < (int)(user   / 2); i++){
			COLOUR wattrset(pad,COLOR_PAIR(9));
			wprintw(pad,"U");
			COLOUR wattrset(pad,COLOR_PAIR(0));
		}
		for (i = 0; i < (int)(kernel / 2); i++){
			COLOUR wattrset(pad,COLOR_PAIR(8));
			wprintw(pad,"s");
			COLOUR wattrset(pad,COLOR_PAIR(0));
		}
		for (i = 0; i < (int)(iowait / 2); i++) {
			COLOUR wattrset(pad,COLOR_PAIR(10));
			wprintw(pad,"W");
			COLOUR wattrset(pad,COLOR_PAIR(0));
		}
		for (i = 0; i < (int)(idle   / 2); i++)
			wprintw(pad," ");
		mvwprintw(pad,row, 77, "|");
		
		peak_col = 28 +(int)(cpu_peak[cpu_no]/2);
		if(peak_col > 77)
			peak_col=77;
		mvwprintw(pad,row, peak_col, ">");
	} else {
	/* Sanity check the numnbers */
		if( user < 0.0 || kernel < 0.0 || iowait < 0.0 || idle < 0.0 || idle >100.0) {
			user = kernel = iowait = idle = 0;
		}
		
		if(cpu_no == 0)
			fprintf(fp,"CPU_ALL,%s,%.1lf,%.1lf,%.1lf,%.1lf,,%d\n", LOOP,
			    user, kernel, iowait, idle,cpus);
		else {
			fprintf(fp,"CPU%02d,%s,%.1lf,%.1lf,%.1lf,%.1lf\n", cpu_no, LOOP,
			    user, kernel, iowait, idle);
		}
	}
}
/* Added variable to remember started children
 * 0 - start
 * 1 - snap
 * 2 - end
*/
#define CHLD_START 0
#define CHLD_SNAP 1
#define CHLD_END 2
int nmon_children[3] = {-1,-1,-1};


/* Signal handler 
 * SIGUSR1 or 2 is used to stop nmon cleanly
 * SIGWINCH is used when the window size is changed
 */
void	interrupt(int signum)
{
int child_pid;
int waitstatus;
	if (signum == SIGCHLD ) {
		while((child_pid = waitpid(0, &waitstatus, 0)) == -1 ) {
			if( errno == EINTR) /* retry */
				continue;
			return; /* ECHLD, EFAULT */
		}
                if(child_pid == nmon_children[CHLD_SNAP]) 
                	nmon_children[CHLD_SNAP] = -1;
		signal(SIGCHLD, interrupt);
		return;
	}
	if (signum == SIGUSR1 || signum == SIGUSR2) {
		maxloops = loop;
		return;
	}
	if (signum == SIGWINCH) {
		CURSE endwin(); /* stop + start curses so it works out the # of row and cols */
		CURSE initscr();
		CURSE nodelay(stdscr, TRUE);
		CURSE noecho();
		CURSE keypad(stdscr,TRUE);
		CURSE cbreak();
		signal(SIGWINCH, interrupt);
		COLOUR colour = has_colors();
        	COLOUR start_color();
                COLOUR init_pair((short)0,(short)7,(short)0); /* White */
                COLOUR init_pair((short)1,(short)1,(short)0); /* Red */
                COLOUR init_pair((short)2,(short)2,(short)0); /* Green */
                COLOUR init_pair((short)3,(short)3,(short)0); /* Yellow */
                COLOUR init_pair((short)4,(short)4,(short)0); /* Blue */
                COLOUR init_pair((short)5,(short)5,(short)0); /* Magenta */
                COLOUR init_pair((short)6,(short)6,(short)0); /* Cyan */
                COLOUR init_pair((short)7,(short)7,(short)0); /* White */
		COLOUR init_pair((short)8,(short)1,(short)1); /* Red background, red text */
		COLOUR init_pair((short)9,(short)2,(short)2); /* Green background, green text */
		COLOUR init_pair((short)10,(short)4,(short)4); /* Blue background, blue text */
		COLOUR init_pair((short)11,(short)3,(short)3); /* Yellow background, yellow text */
		COLOUR init_pair((short)12,(short)6,(short)6); /* Cyan background, cyan text */
		COLOUR init_pair((short)13,(short)5,(short)5); /* Magenta background, Magenta text */
		CURSE clear();
		return;
	}
	CURSE endwin();
	exit(0);
}


/* only place the q=previous and p=currect pointers are modified */
void switcher(void)
{
	static int	which = 1;

	if (which) {
		p = &database[0];
		q = &database[1];
		which = 0;
	} else {
		p = &database[1];
		q = &database[0];
		which = 1;
	}
	if(flash_on)
		flash_on = 0;
	else
		flash_on = 1;
}


/* Lookup the right string */
char	*status(int n)
{
	switch (n) {
	case 0:
		return "Run  ";
	default:
		return "Sleep";
	}
}

/* Lookup the right process state string */
char	*get_state( char n)
{
	static char	duff[64];
	switch (n) {
	case 'R': return "Running  ";
	case 'S': return "Sleeping ";
	case 'D': return "DiskSleep";
	case 'Z': return "Zombie   ";
	case 'T': return "Traced   ";
	case 'W': return "Paging   ";
	default:
		sprintf(duff, "%d", n);
		return duff;
	}
}

#ifdef GETUSER
/* Convert User id (UID) to a name with caching for speed 
 * getpwuid() should be NFS/yellow pages safe
 */
char	*getuser(uid_t uid)
{
#define NAMESIZE 16
	struct user_info {
		uid_t uid;
		char	name[NAMESIZE];
	};
	static struct user_info *u = NULL;
	static int	used = 0;
	int	i;
	struct passwd *pw;

	i = 0;
	if (u != NULL) {
		for (i = 0; i < used; i++) {
			if (u[i].uid == uid) {
				return u[i].name;
			}
		}
		u = (struct user_info *)realloc(u, (sizeof(struct user_info ) * (i + 1)));
	} else
		u = (struct user_info *)malloc(sizeof(struct user_info ));
	used++;

	/* failed to find a match so add it */
	u[i].uid = uid;
	pw = getpwuid(uid);

	if (pw != NULL)
		strncpy(u[i].name, pw->pw_name, NAMESIZE);
	else
		sprintf(u[i].name, "unknown%d",uid);
	return u[i].name;
}
#endif /* GETUSER */

/* User Defined Disk Groups */

char   *save_word(char *in, char *out)
{
        int   len;
        int   i;
        len = strlen(in);
	out[0] = 0;
        for (i = 0; i < len; i++) {
                if ( isalnum(in[i]) || in[i] == '_' || in[i] == '-' || in[i] == '/' ) {
                        out[i] = in[i];
                        out[i+1] = 0;
                } else
                        break;
        }
        for (; i < len; i++)
                if (isalnum(in[i]))
                        return &in[i];
        return &in[i];
} 

#define DGROUPS 64
#define DGROUPITEMS 512

char   *dgroup_filename;
char   *dgroup_name[DGROUPS];
int   *dgroup_data;
int   dgroup_disks[DGROUPS];
int   dgroup_total_disks;
int   dgroup_total_groups;

void load_dgroup(struct dsk_stat *dk)
{
	FILE * gp;
	char   line[4096];
	char   name[1024];
	int   i, j;
	char   *nextp;

	if (dgroup_loaded == 2)
		return;
	dgroup_data = MALLOC(sizeof(int)*DGROUPS * DGROUPITEMS);
	for (i = 0; i < DGROUPS; i++)
		for (j = 0; j < DGROUPITEMS; j++)
			dgroup_data[i*DGROUPS+j] = -1;

	gp = fopen(dgroup_filename, "r");

	if (gp == NULL) {
		perror("opening disk group file");
		fprintf(stderr,"ERROR: failed to open %s\n", dgroup_filename);
		exit(9);
	}

	for (dgroup_total_groups = 0; 
		fgets(line, 4096, gp) != NULL && dgroup_total_groups < DGROUPS; 
		dgroup_total_groups++) {
		/* save the name */
		nextp = save_word(line, name);
		if(strlen(name) == 0) { /* was a blank line */
			fprintf(stderr,"ERROR elmon:ignoring odd line in diskgroup file \"%s\"\n",line);
			/* Decrement dgroup_total_groups by 1 to correct index for next loop */
			--dgroup_total_groups;
			continue;
		}
		/* Added +1 to be able to correctly store the terminating \0 character */
		dgroup_name[dgroup_total_groups] = MALLOC(strlen(name)+1);
		strcpy(dgroup_name[dgroup_total_groups], name);

		/* save the hdisks */
		for (i = 0; i < DGROUPITEMS && *nextp != 0; i++) {
			nextp = save_word(nextp, name);
			for (j = 0; j < disks; j++) {
				if ( !strcmp(dk[j].dk_name, name) ) {
					dgroup_data[dgroup_total_groups*DGROUPS+i] = j;
					dgroup_disks[dgroup_total_groups]++;
					dgroup_total_disks++;
					break;
				}
			}
			if (j == disks)
				fprintf(stderr,"ERROR elmon:diskgroup file - failed to find disk=%s for group=%s disks know=%d\n",
					 name, dgroup_name[dgroup_total_groups],disks);
		}
	}
	fclose(gp);
	dgroup_loaded = 2;
}


void list_dgroup(struct dsk_stat *dk)
{
	int   i, j, k, n;
	int   first = 1;

	for (n = 0, i = 0; i < dgroup_total_groups; i++) {
		if (first) {
			fprintf(fp, "BBBG,%03d,User Defined Disk Groups Name,Disks\n", n++);
			first = 0;
		}
		fprintf(fp, "BBBG,%03d,%s", n++, dgroup_name[i]);
		for (k = 0, j = 0; j < dgroup_disks[i]; j++) {
			if (dgroup_data[i*DGROUPS+j] != -1) {
				fprintf(fp, ",%s", dk[dgroup_data[i*DGROUPS+j]].dk_name);
				k++;
			}
			/* add extra line if we have lots to stop spreadsheet line width problems */
			if (k == 128) {
				fprintf(fp, "\nBBBG,%03d,%s continued", n++, dgroup_name[i]);
			}
		}
		fprintf(fp, "\n");
	}
	fprintf(fp, "DGBUSY,Disk Group Busy %s", hostname);
	for (i = 0; i < DGROUPS; i++) {
		if (dgroup_name[i] != 0)
			fprintf(fp, ",%s", dgroup_name[i]);
	}
	fprintf(fp, "\n");
	fprintf(fp, "DGREAD,Disk Group Read KB/s %s", hostname);
	for (i = 0; i < DGROUPS; i++) {
		if (dgroup_name[i] != 0)
			fprintf(fp, ",%s", dgroup_name[i]);
	}
	fprintf(fp, "\n");
	fprintf(fp, "DGWRITE,Disk Group Write KB/s %s", hostname);
	for (i = 0; i < DGROUPS; i++) {
		if (dgroup_name[i] != 0)
			fprintf(fp, ",%s", dgroup_name[i]);
	}
	fprintf(fp, "\n");
	fprintf(fp, "DGSIZE,Disk Group Block Size KB %s", hostname);
	for (i = 0; i < DGROUPS; i++) {
		if (dgroup_name[i] != 0)
			fprintf(fp, ",%s", dgroup_name[i]);
	}
	fprintf(fp, "\n");
	fprintf(fp, "DGXFER,Disk Group Transfers/s %s", hostname);
	for (i = 0; i < DGROUPS; i++) {
		if (dgroup_name[i] != 0)
			fprintf(fp, ",%s", dgroup_name[i]);
	}
	fprintf(fp, "\n");
}



void hint(void)
{
	printf("\tDeveloper: Brian Smith\n");
	printf("\tBased on nmon by Nigel Griffiths\n");
	printf("\tLicense:  GPL version 3\n\n");
	printf("\tThis program is distributed in the hope that it will be useful,\n");
	printf("\tbut WITHOUT ANY WARRANTY; without even the implied warranty of\n");
	printf("\tMERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");
	printf("\nHint: %s [-h] [-s <seconds>] [-c <count>] [-f -d <disks> -t -r <name>] [-x]\n\n", progname);
	printf("\t-h            FULL help information\n");
	printf("\tInteractive-Mode:\n");
	printf("\tread startup banner and type: \"h\" once it is running\n");
	printf("\tFor Data-Collect-Mode (-f)\n");
	printf("\t-f            spreadsheet output format [note: default -s300 -c288]\n");
	printf("\toptional\n");
	printf("\t-s <seconds>  between refreshing the screen [default 2]\n");
	printf("\t-c <number>   of refreshes [default millions]\n");
	printf("\t-d <disks>    to increase the number of disks [default 256]\n");
	printf("\t-t            spreadsheet includes top processes\n");
	printf("\t-x            capacity planning (15 min for 1 day = -fdt -s 900 -c 96)\n");
	printf("\n");
}

void help(void)
{
	hint();
	printf("Version - %s\n\n",SccsId);
	printf("For Interactive-Mode\n");
	printf("\t-s <seconds>  between refreshing the screen [default 2]\n");
	printf("\t-c <number>   of refreshes [default millions]\n");
	printf("\t-g <filename> User Defined Disk Groups [hit g to show them]\n");
	printf("\t              - file = on each line: group_name <disks list> sapce separated\n");
	printf("\t              - like: database sdb sdc sdd sde\n");
	printf("\t              - upto 32 disk groups, disks can appear more than once\n");
	printf("\t-b            black and white [default is colour]\n");
	printf("\texample: %s -s 1 -c 100\n",progname);
	printf("\n");
	printf("For Data-Collect-Mode = spreadsheet format (comma separated values)\n");
	printf("\tNote: use only one of f,F,z,x or X and make it the first argument\n");
	printf("\t-f            spreadsheet output format [note: default -s300 -c288]\n");
	printf("\t\t\t output file is <hostname>_YYYYMMDD_HHMM.nmon\n");
	printf("\t-F <filename> same as -f but user supplied filename\n");
	printf("\t-r <runname>  goes into spreadsheet file [default hostname]\n");
	printf("\t-t            include top processes in the output\n");
	printf("\t-T            as -t plus saves command line arguments in UARG section\n");
	printf("\t-s <seconds>  between snap shots\n");
	printf("\t-c <number>   of refreshes\n");
	printf("\t-d <disks>    to increase the number of disks [default 256]\n");
	printf("\t-l <dpl>      disks/line default 150 to avoid spreadsheet issues. EMC=64.\n");
	printf("\t-g <filename> User Defined Disk Groups (see above)\n");

	printf("\t-N            include NFS Network File System\n");
	printf("\t-I <percent>  Include precoess and disks busy threshold (default 0.1)\n");
	printf("\t              don't save or show proc/disk using less than this percent\n");
	printf("\t-m <directory> elmon changes to this directory before saving to file\n");
	printf("\texample: collect for 1 hour at 30 second intervals with top procs\n");
	printf("\t\t %s -f -t -r Test1 -s30 -c120\n",progname);
	printf("\n");
printf("\tTo load into a spreadsheet like Lotus 1-2-3:\n");
	printf("\tsort -A *nmon >stats.csv\n");
	printf("\ttransfer the stats.csv file to your PC\n");
	printf("\tStart 1-2-3 and then Open <char-separated-value ASCII file>\n");
	printf("\n");
	printf("Capacity planning mode - use cron to run each day\n");
	printf("\t-x            sensible spreadsheet output for CP =  one day\n");
	printf("\t              every 15 mins for 1 day ( i.e. -ft -s 900 -c 96)\n");
	printf("\t-X            sensible spreadsheet output for CP = busy hour\n");
	printf("\t              every 30 secs for 1 hour ( i.e. -ft -s 30 -c 120)\n");
	printf("\n");
	printf("Set-up and installation\n");
	printf("\t- this adds the disk %% busy numbers (otherwise they are zero)\n");
	printf("\tIf you have hundreds of disk this can take 1%% to 2%% CPU\n");
	printf("\n");

	printf("Interactive Mode Commands\n");
	printf("\tkey --- Toggles to control what is displayed ---\n");
	printf("\th   = Online help information\n");
	printf("\tr   = Machine type, machine name, cache details and OS version + LPAR\n");
	printf("\tc   = CPU by processor stats with bar graphs\n");
	printf("\tl   = long term CPU (over 75 snapshots) with bar graphs\n");
	printf("\tm   = Memory stats\n");
	printf("\tM   = Memory graphs\n");
	printf("\tV   = Virtual Memory and Swap stats\n");
	printf("\tk   = Kernel Internal stats\n"); 
	printf("\tn   = Network stats and errors\n");
	printf("\tN   = NFS Network File System\n");
	printf("\td   = Disk I/O Graphs\n");
	printf("\tD   = Disk I/O Stats\n");
	printf("\to   = Disk I/O Map (one character per disk showing how busy it is)\n");
#ifdef PARTITIONS
	printf("\tP   = Partitions Disk I/O Stats\n");
#endif 
#ifdef POWER
	printf("\tp   = Logical Partitions Stats\n");
#endif 
	printf("\tb   = black and white mode (or use -b option)\n");
	printf("\t.   = minimum mode i.e. only busy disks and processes\n");
	printf("\n");
	printf("\tkey --- Other Controls ---\n");
	printf("\t+   = double the screen refresh time\n");
	printf("\t-   = halves the screen refresh time\n");
	printf("\tq   = quit (also x, e or control-C)\n");
	printf("\t0   = reset peak counts to zero (peak = \">\")\n");
	printf("\tC   = Single-Column View\n");
	printf("\tspace = refresh screen now\n");
	printf("\n");
	printf("Startup Control\n");
	printf("\tIf you find you always type the same toggles every time you start\n");
	printf("\tthen place them in the ELMON shell variable. For example:\n");
	printf("\t export ELMON=cmdrvtan\n");

	printf("\n");
	printf("Others:\n");
	printf("\ta) To you want to stop elmon - kill -USR2 <elmon-pid>\n");
	printf("\tb) Use -p and elmon outputs the background process pid\n");
	printf("\tc) To limit the processes elmon lists (online and to a file)\n");
	printf("\t   Either set NMONCMD0 to NMONCMD63 to the program names\n");
	printf("\t   or use -C cmd:cmd:cmd etc. example: -C ksh:vi:syncd\n");
	printf("\td) If you want to pipe elmon output to other commands use a FIFO:\n");
	printf("\t   mkfifo /tmp/mypipe\n");
	printf("\t   elmon -F /tmp/mypipe &\n");
	printf("\t   grep /tmp/mypipe\n");
	printf("\te) If elmon fails please report it with:\n");
	printf("\t   1) elmon version like: %s\n",VERSION);
	printf("\t   2) the output of cat /proc/cpuinfo\n");
	printf("\t   3) some clue of what you were doing\n");
	printf("\t   4) I may ask you to run the debug version\n");
	printf("\n");
	exit(0);
}

#define JFSMAX 128
#define LOAD 1
#define UNLOAD 0
#define JFSNAMELEN 64
#define JFSTYPELEN 8

struct jfs {
	char name[JFSNAMELEN];
	char device[JFSNAMELEN];
	char type[JFSNAMELEN];
	int  fd;
	int  mounted;
	} jfs[JFSMAX];

int jfses =0;
void jfs_load(int load)
{
int i;
struct stat stat_buffer;
FILE * mfp; /* FILE pointer for mtab file*/
struct mntent *mp; /* mnt point stats */
static int jfs_loaded = 0;

	if(load==LOAD) { 
		if(jfs_loaded == 0) { 
			mfp = setmntent("/etc/mtab","r");
			for(i=0; i<JFSMAX && (mp = getmntent(mfp) ) != NULL; i++) {
				strncpy(jfs[i].device, mp->mnt_fsname,JFSNAMELEN);
				strncpy(jfs[i].name,mp->mnt_dir,JFSNAMELEN);
				strncpy(jfs[i].type, mp->mnt_type,JFSTYPELEN);
				mp->mnt_fsname[JFSNAMELEN-1]=0;
				mp->mnt_dir[JFSNAMELEN-1]=0;
				mp->mnt_type[JFSTYPELEN-1]=0;
			}
			endfsent();
			jfs_loaded = 1;
			jfses=i;
		}

		/* 1st or later time - just reopen the mount points */
		for(i=0;i<JFSMAX && jfs[i].name[0] !=0;i++) {
			if(stat(jfs[i].name, &stat_buffer) != -1 ) {

                                jfs[i].fd = open(jfs[i].name, O_RDONLY);
                                if(jfs[i].fd != -1 ) 
                                        jfs[i].mounted = 1;
                                else
                                        jfs[i].mounted = 0;
			}
			else jfs[i].mounted = 0;
		}
	} else { /* this is an unload request */
		if(jfs_loaded)
			for(i=0;i<JFSMAX && jfs[i].name[0] != 0;i++) {
			    if(jfs[i].mounted)
				close(jfs[i].fd);
			    jfs[i].fd=0;
			}
		else 
			/* do nothing */ ;
	}
}

/* We order this array rather than the actual process tables
 * the index is the position in the process table and
 * the time is the CPU used in the last period in seconds
 */
struct topper {
	int	index;
	int	other;
	double	size;
	double	io;
	int	time;
} *topper;
int	topper_size = 200;

/* Routine used by qsort to order the processes by CPU usage */
int	cpu_compare(const void *a, const void *b)
{
	return (int)(((struct topper *)b)->time - ((struct topper *)a)->time);
}

int	size_compare(const void *a, const void *b)
{
	return (int)((((struct topper *)b)->size - ((struct topper *)a)->size));
}

int	disk_compare(void *a, void *b)
{
	return (int)((((struct topper *)b)->io - ((struct topper *)a)->io));
}


/* checkinput is the subroutine to handle user input */
int checkinput(void)
{
	static int use_env = 1;
	char	buf[1024];
	int	chars;
	int	key;
	int	i = 0;
	char *p;
	int 	keys = 0;  //Keeps track if a key was pressed.  1 = yes, 2 = arrow key was pressed

	if (!cursed) /* not user input so stop with control-C */
		return 0;

	if (use_env){
		use_env = 0;
		p=getenv("ELMON");
		if(p!=0){
			strcpy(buf,p);
			chars = strlen(buf);
                        for (i = chars; i >= 0; i--){
				ungetch(buf[i]);
			}
                }
	}
	
	key = getch();
		while (key != ERR){
				keys = 1;
				welcome=0;
				switch (key) {
				case 'x':
				case 'q':
					nocbreak();
					endwin();
					exit(0);

				case '6':
				case '7':
				case '8':
				case '9':
					dotline = buf[i] - '0';
					break;
				case '+':
					seconds = seconds * 2;
					if (seconds > 10 && (seconds % 10 != 0)){
						seconds = (int)(seconds / 10) * 10;
					}
					break;
				case '-':
					seconds = seconds / 2;
					if (seconds < 1)
						seconds = 1;
					if (seconds > 10 && (seconds % 10 != 0)){
						seconds = (int)(seconds / 10) * 10;
					}
					break;
				case '.':
					if (show_all)
						show_all = 0;
					else {
						show_all = 1;
						show_disk_mode = SHOW_DISK_STATS;
						add_option(SHOW_DISK);
						add_option(SHOW_TOP);
						show_topmode =3;
					}
					clear();
					break;
				case '?':
				case 'h':
				case 'H':
                                        if (enabled_option(SHOW_HELP))
                                                remove_option(SHOW_HELP);
                                        else {
                                                add_option(SHOW_HELP);
                                                remove_option(SHOW_VERBOSE);
                                        }
					clear();
					break;
				case 'b':
				case 'B':
					FLIP(colour);
					clear();
					break;
				case 'Z':
					FLIP(show_raw);
					add_option(SHOW_SMP);
					clear();
					break;
				case 'l':
					flip (SHOW_LONGTERM);
					clear();
					break;
				case 'p':
					flip(SHOW_LPAR);
					clear();
					break;
				case 'V':
					flip(SHOW_VM);
					clear();
					break;
				case 'j':
				case 'J':
                                        flip(SHOW_JFS);
                                        if (enabled_option(SHOW_JFS)){
                                                jfs_load(1);
                                        }else{
                                                jfs_load(0);
                                        }
					clear();
					break;
#ifdef PARTITIONS
				case 'P':
					flip(SHOW_PARTITIONS);
					clear();
					break;
#endif /*PARTITIONS*/
				case 'k':
				case 'K':
					flip(SHOW_KERNEL);
					clear();
					break;
				case 'm':
					flip(SHOW_MEMORY);
					clear();
					break;
				case 'M':
					flip(SHOW_MEMORY_GRAPH);
					clear();
					break;
				case 'L':
					flip(SHOW_LARGE);
					clear();
					break;
				case 'D':
                                        switch (show_disk_mode) {
                                        case SHOW_DISK_NONE:
                                                show_disk_mode = SHOW_DISK_STATS;
                                                add_option(SHOW_DISK);
                                                break;
                                        case SHOW_DISK_STATS:
                                                show_disk_mode = SHOW_DISK_NONE;
                                                remove_option(SHOW_DISK);
                                                break;
                                        case SHOW_DISK_GRAPH:
                                                show_disk_mode = SHOW_DISK_STATS;
                                                add_option(SHOW_DISK);
                                                break;
                                        }
					clear();
					break;
				case 'd':
                                       switch (show_disk_mode) {
                                        case SHOW_DISK_NONE:
                                                show_disk_mode = SHOW_DISK_GRAPH;
                                                add_option(SHOW_DISK);
                                                break;
                                        case SHOW_DISK_STATS:
                                                show_disk_mode = SHOW_DISK_GRAPH;
                                                add_option(SHOW_DISK);
                                                break;
                                        case SHOW_DISK_GRAPH:
                                                show_disk_mode = 0;
                                                remove_option(SHOW_DISK);
                                                break;
                                        }
					clear();
					break;
				case 'o':
				case 'O':
					flip(SHOW_DISKMAP);
					clear();
					break;
				case 'n':
                                       if (enabled_option(SHOW_NET)) {
                                                remove_option(SHOW_NET);
                                                show_neterror = 0;
                                        } else {
                                                add_option(SHOW_NET);
                                                show_neterror = 3;
                                        }
					clear();
					break;
				case 'N':
					flip(SHOW_NFS);
					clear();
					break;
				case 'c':
					flip(SHOW_SMP);
					clear();
					break;
				case 'C':
					FLIP(show_columns);
					clear();
					break;
				case 'r':
				case 'R':
					flip(SHOW_CPU);
					clear();
					break;
				case 't':
					show_topmode = 3; /* Fall Through */
				case 'T':
					flip(SHOW_TOP);
					clear();
					break;
				case 'v':
					flip(SHOW_VERBOSE);
					clear();
					break;
				case 'u':
					if (show_args == ARGS_NONE) {
						args_load();
						show_args = ARGS_ONLY;
						add_option(SHOW_TOP);
						if( show_topmode != 3 &&
						 show_topmode != 4 &&
						 show_topmode != 5 )
						 show_topmode = 3;
					} else 
						show_args = ARGS_NONE;
					clear();
					break;
				case '1':
					show_topmode = 1;
					add_option(SHOW_TOP);
					clear();
					break;
/*
				case '2':
					show_topmode = 2;
					show_top = 1;
					clear();
					break;
*/
				case '3':
					show_topmode = 3;
					add_option(SHOW_TOP);
					clear();
					break;
				case '4':
					show_topmode = 4;
					add_option(SHOW_TOP);
					clear();
					break;
				case '5':
					show_topmode = 5;
					add_option(SHOW_TOP);
					clear();
					break;
				case '0':
					for(i=0;i<max_cpus+1;i++)
						cpu_peak[i]=0;
					for(i=0;i<networks;i++) {
						net_read_peak[i]=0.0;
						net_write_peak[i]=0.0;
					}
					for(i=0;i<disks;i++) {
						disk_busy_peak[i]=0.0;
						disk_rate_peak[i]=0.0;
					}
					snap_clear();
					aiocount_max = 0;
					aiotime_max = 0.0;
					aiorunning_max = 0;
					huge_peak = 0;
					mem_peak = 0;
					swap_peak = 0;
					break;
				case ' ':
					clear();
					break;
				case 'g':
					flip(SHOW_DGROUP);
                                        clear();
                                        break;
				case KEY_ENTER:
				case '\n':
					if (enabled_option(SHOW_HELP)){
						ungetch(keymap[menu_sX][menu_sY]);
					}
					break;
				case KEY_UP:
					if (enabled_option(SHOW_HELP)){
						keys = 2;
						menu_sY--;
						if (menu_sY == 0 && menu_sX == 2) {menu_sX = 1;}
						if (menu_sY == 6 && menu_sX == 2) { menu_sX = 3;}
						if (menu_sY == 0) { menu_sY = 8;}
						if (menu_sY > 6 && menu_sX > 2) {menu_sX = 2;}
					}
					break;
				case KEY_DOWN:
					if (enabled_option(SHOW_HELP)){
						keys = 2;
						menu_sY++;
						if (menu_sY == 9 && menu_sX == 2) { menu_sX = 3;}
						if (menu_sY == 9) { menu_sY = 1;}
						if (menu_sY == 7 && menu_sX == 2) {menu_sX = 1;}
						if (menu_sY == 7 && menu_sX >= 3) {menu_sX = 2;}
					}
					break;
				case KEY_LEFT:
					if (enabled_option(SHOW_HELP)){
						keys = 2;
						menu_sX--;
						if (menu_sX == 0) { menu_sX = 4;}
						if (menu_sY > 6 && menu_sX > 2) {menu_sX = 2;}
					}
					break;
				case KEY_RIGHT:
					if (enabled_option(SHOW_HELP)){
						keys = 2;
						menu_sX++;
						if (menu_sX == 5) { menu_sX = 1;}
						if (menu_sY > 6 && menu_sX > 2) {menu_sX = 2;}
					}
					break;
				#ifdef NCURSES_MOUSE_VERSION	
				case KEY_MOUSE:
					if (mouse){
						getmouse(&mouse_event);
						if (enabled_option(SHOW_HELP)){
							if (mouse_event.x < 80){
								int offset;
								if (enabled_option(SHOW_VERBOSE)){
									offset = 6;
								}else{
									offset = 0;
								}
								if (mouse_event.y == 2 + offset && mouse_event.x <= 15)
									ungetch('h');
								if (mouse_event.y == 2 + offset && mouse_event.x <= 36 && mouse_event.x > 15)
									ungetch('l');
								if (mouse_event.y == 2 + offset && mouse_event.x <= 58 && mouse_event.x > 36)
									ungetch('m');
								if (mouse_event.y == 2 + offset && mouse_event.x > 58)
									ungetch('M');
								if (mouse_event.y == 3 + offset && mouse_event.x <= 15)
									ungetch('V');
								if (mouse_event.y == 3 + offset && mouse_event.x <= 36 && mouse_event.x > 15)
									ungetch('n');
								if (mouse_event.y == 3 + offset && mouse_event.x <= 58 && mouse_event.x > 36)
									ungetch('j');
								if (mouse_event.y == 3 + offset && mouse_event.x > 58)
									ungetch('k');
								if (mouse_event.y == 4 + offset && mouse_event.x <= 15)
									ungetch('N');
								if (mouse_event.y == 4 + offset && mouse_event.x <= 36 && mouse_event.x > 15)
									ungetch('o');
								if (mouse_event.y == 4 + offset && mouse_event.x <= 58 && mouse_event.x > 36)
									ungetch('d');
								if (mouse_event.y == 4 + offset && mouse_event.x > 58)
									ungetch('D');
								if (mouse_event.y == 5 + offset && mouse_event.x <= 15)
									ungetch('c');
								if (mouse_event.y == 5 + offset && mouse_event.x <= 36 && mouse_event.x > 15)
									ungetch('g');
								if (mouse_event.y == 5 + offset && mouse_event.x <= 58 && mouse_event.x > 36)
									ungetch('r');
								if (mouse_event.y == 5 + offset && mouse_event.x > 58)
									ungetch('v');
								if (mouse_event.y == 7 + offset && mouse_event.x <= 15)
									ungetch('t');
								if (mouse_event.y == 7 + offset && mouse_event.x <= 36 && mouse_event.x > 15)
									ungetch('1');
								if (mouse_event.y == 7 + offset && mouse_event.x <= 58 && mouse_event.x > 36)
									ungetch('3');
								if (mouse_event.y == 7 + offset && mouse_event.x > 58)
									ungetch('u');
								if (mouse_event.y == 9 + offset && mouse_event.x <= 15)
									ungetch('q');
								if (mouse_event.y == 9 + offset && mouse_event.x <= 36 && mouse_event.x > 15)
									ungetch('b');
								if (mouse_event.y == 9 + offset && mouse_event.x <= 58 && mouse_event.x > 36)
									ungetch('C');
								if (mouse_event.y == 9 + offset && mouse_event.x > 58)
									ungetch(' ');
								if (mouse_event.y == 10 + offset && mouse_event.x <= 36)
									ungetch('.');
								if (mouse_event.y == 10 + offset && mouse_event.x > 36)
									ungetch('0');
								if (mouse_event.y == 11 + offset && mouse_event.x <= 36)
									ungetch('+');
								if (mouse_event.y == 11 + offset && mouse_event.x > 36)
									ungetch('-');
							}
						}
						else {
							add_option(SHOW_HELP);
							remove_option(SHOW_VERBOSE);
							clear();
						}
					}
					break;
				#endif
				default: return 0;
				}
			key = getch();
		}

	if (keys == 1) {        //Key was pressed
		keys = 0;
		return 1;
	}else if (keys == 2) {  //Arraw key was pressed
		keys = 0;
		return 2;
	}else {                 //No keys were pressed
		return 0;
	}
}

void go_background(int def_loops, int def_secs)
{
	cursed = 0;
	if (maxloops == -1)
		maxloops = def_loops;
	if (seconds  == -1)
		seconds = def_secs;
        add_option(SHOW_CPU);
        add_option(SHOW_SMP);
        add_option(SHOW_MEMORY);
        add_option(SHOW_LARGE);
        remove_option(SHOW_TOP);
        add_option(SHOW_PARTITIONS);
        add_option(SHOW_LPAR);
        add_option(SHOW_VM);
        add_option(SHOW_KERNEL);
        add_option(SHOW_NET);
        add_option(SHOW_JFS);
        add_option(SHOW_DISK);
        show_disk_mode    = SHOW_DISK_STATS;

        show_all     = 1;
        show_topmode = 3;
}

void proc_net()
{
static FILE *fp = (FILE *)-1;
char buf[1024];
int i=0;
int ret;
unsigned long junk;

	if( fp == (FILE *)-1) {
           if( (fp = fopen("/proc/net/dev","r")) == NULL) {
		error("failed to open - /proc/net/dev");
		networks=0;
		return;
	   }
	}
	if(fgets(buf,1024,fp) == NULL) goto end; /* throw away the header lines */
	if(fgets(buf,1024,fp) == NULL) goto end; /* throw away the header lines */
/*
Inter-|   Receive                                                |  Transmit
 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
    lo:    1956      30    0    0    0     0          0         0     1956      30    0    0    0     0       0          0
  eth0:       0       0    0    0    0     0          0         0   458718       0  781    0    0     0     781          0
  sit0:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0
  eth1:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0
*/
	for(i=0;i<NETMAX;i++) {
		if(fgets(buf,1024,fp) == NULL)
			break;
		strip_spaces(buf);
				     /* 1   2   3    4   5   6   7   8   9   10   11   12  13  14  15  16 */
		ret = sscanf(&buf[0], "%s %llu %llu %lu %lu %lu %lu %lu %lu %llu %llu %lu %lu %lu %lu %lu",
		(char *)&p->ifnets[i].if_name,
			&p->ifnets[i].if_ibytes,
			&p->ifnets[i].if_ipackets,
			&p->ifnets[i].if_ierrs,
			&p->ifnets[i].if_idrop,
			&p->ifnets[i].if_ififo,
			&p->ifnets[i].if_iframe,
			&junk,
			&junk,
			&p->ifnets[i].if_obytes,
			&p->ifnets[i].if_opackets,
			&p->ifnets[i].if_oerrs,
			&p->ifnets[i].if_odrop,
			&p->ifnets[i].if_ofifo,
			&p->ifnets[i].if_ocolls,
			&p->ifnets[i].if_ocarrier
			);
		if(ret != 16) 
			fprintf(stderr,"sscanf wanted 16 returned = %d line=%s\n", ret, (char *)buf);
	}
	end:
	if(reread) {
		fclose(fp);
		fp = (FILE *)-1;
	} else rewind(fp);
	networks = i;
}


int proc_procsinfo(int pid, int index)
{
FILE *fp;
char filename[64];
char buf[1024*4];
int size=0;
int ret=0;
int count=0;

	sprintf(filename,"/proc/%d/stat",pid);
	if( (fp = fopen(filename,"r")) == NULL) {
		sprintf(buf,"failed to open file %s",filename);
		error(buf);
		return 0;
	}
	size = fread(buf, 1, 1024-1, fp);
	if(size == -1) {
#ifdef DEBUG
		fprintf(stderr,"procsinfo read returned = %d assuming process stopped pid=%d\n", ret,pid);
#endif /*DEBUG*/
		return 0;
	}
	fclose(fp);
        ret = sscanf(buf, "%d (%s)",
                &p->procs[index].pi_pid,
                &p->procs[index].pi_comm[0]);
	if(ret != 2) {
		fprintf(stderr,"procsinfo sscanf returned = %d line=%s\n", ret,buf);
		return 0;
	}
	p->procs[index].pi_comm[strlen(p->procs[index].pi_comm)-1] = 0;

	for(count=0; count<size;count++)	/* now look for ") " as dumb Infiniban driver includes "()" */
		if(buf[count] == ')' && buf[count+1] == ' ' ) break;

	if(count == size) {
#ifdef DEBUG
		fprintf(stderr,"procsinfo failed to find end of command buf=%s\n", buf);
#endif /*DEBUG*/
		return 0;
	}
	count++; count++;

        ret = sscanf(&buf[count], 
"%c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %lu %lu %ld %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d",
                &p->procs[index].pi_state,
                &p->procs[index].pi_ppid,
                &p->procs[index].pi_pgrp,
                &p->procs[index].pi_session,
                &p->procs[index].pi_tty_nr,
                &p->procs[index].pi_tty_pgrp,
                &p->procs[index].pi_flags,
                &p->procs[index].pi_minflt,
                &p->procs[index].pi_cmin_flt,
                &p->procs[index].pi_majflt,
                &p->procs[index].pi_cmaj_flt,
                &p->procs[index].pi_utime,
                &p->procs[index].pi_stime,
                &p->procs[index].pi_cutime,
                &p->procs[index].pi_cstime,
                &p->procs[index].pi_pri,
                &p->procs[index].pi_nice,
                &p->procs[index].junk,
                &p->procs[index].pi_it_real_value,
                &p->procs[index].pi_start_time,
                &p->procs[index].pi_vsize,
                &p->procs[index].pi_rss,
                &p->procs[index].pi_rlim_cur,
                &p->procs[index].pi_start_code,
                &p->procs[index].pi_end_code,
                &p->procs[index].pi_start_stack,
                &p->procs[index].pi_esp,
                &p->procs[index].pi_eip,
                &p->procs[index].pi_pending_signal,
                &p->procs[index].pi_blocked_sig,
                &p->procs[index].pi_sigign,
                &p->procs[index].pi_sigcatch,
                &p->procs[index].pi_wchan,
                &p->procs[index].pi_nswap,
                &p->procs[index].pi_cnswap,
                &p->procs[index].pi_exit_signal,
		&p->procs[index].pi_cpu
		);
	if(ret != 37) {
		fprintf(stderr,"procsinfo2 sscanf wanted 37 returned = %d pid=%d line=%s\n", ret,pid,buf);
		return 0;
	}

	sprintf(filename,"/proc/%d/statm",pid);
	if( (fp = fopen(filename,"r")) == NULL) {
		sprintf(buf,"failed to open file %s",filename);
		error(buf);
		return 0;
	}
	size = fread(buf, 1, 1024*4-1, fp);
	if(size == -1) {
		sprintf(buf,"failed to read file %s",filename);
		error(buf);
		return 0;
	}
	fclose(fp);

        ret = sscanf(&buf[0], "%lu %lu %lu %lu %lu %lu %lu",
                &p->procs[index].statm_size,
                &p->procs[index].statm_resident,
                &p->procs[index].statm_share,
                &p->procs[index].statm_trs,
                &p->procs[index].statm_drs,
                &p->procs[index].statm_lrs,
                &p->procs[index].statm_dt
		);
	if(ret != 7) {
		fprintf(stderr,"sscanf wanted 7 returned = %d line=%s\n", ret,buf);
		return 0;
	}
	return 1;
}
#ifdef DEBUGPROC 
print_procs(int index)
{
printf("procs[%d].pid           =%d\n",index,procs[index].pi_pid);
printf("procs[%d].comm[0]       =%s\n",index,&procs[index].pi_comm[0]);
printf("procs[%d].state         =%c\n",index,procs[index].pi_state);
printf("procs[%d].ppid          =%d\n",index,procs[index].pi_ppid);
printf("procs[%d].pgrp          =%d\n",index,procs[index].pi_pgrp);
printf("procs[%d].session       =%d\n",index,procs[index].pi_session);
printf("procs[%d].tty_nr        =%d\n",index,procs[index].pi_tty_nr);
printf("procs[%d].tty_pgrp      =%d\n",index,procs[index].pi_tty_pgrp);
printf("procs[%d].flags         =%lu\n",index,procs[index].pi_flags);
printf("procs[%d].minflt       =%lu\n",index,procs[index].pi_minflt);
printf("procs[%d].cmin_flt     =%lu\n",index,procs[index].pi_cmin_flt);
printf("procs[%d].majflt       =%lu\n",index,procs[index].pi_majflt);
printf("procs[%d].cmaj_flt     =%lu\n",index,procs[index].pi_cmaj_flt);
printf("procs[%d].utime        =%lu\n",index,procs[index].pi_utime);
printf("procs[%d].stime        =%lu\n",index,procs[index].pi_stime);
printf("procs[%d].cutime       =%ld\n",index,procs[index].pi_cutime);
printf("procs[%d].cstime       =%ld\n",index,procs[index].pi_cstime);
printf("procs[%d].pri           =%d\n",index,procs[index].pi_pri);
printf("procs[%d].nice          =%d\n",index,procs[index].pi_nice);
printf("procs[%d].junk          =%d\n",index,procs[index].junk);
printf("procs[%d].it_real_value =%lu\n",index,procs[index].pi_it_real_value);
printf("procs[%d].start_time    =%lu\n",index,procs[index].pi_start_time);
printf("procs[%d].vsize         =%lu\n",index,procs[index].pi_vsize);
printf("procs[%d].rss           =%lu\n",index,procs[index].pi_rss);
printf("procs[%d].rlim_cur      =%lu\n",index,procs[index].pi_rlim_cur);
printf("procs[%d].start_code    =%lu\n",index,procs[index].pi_start_code);
printf("procs[%d].end_code      =%lu\n",index,procs[index].pi_end_code);
printf("procs[%d].start_stack   =%lu\n",index,procs[index].pi_start_stack);
printf("procs[%d].esp           =%lu\n",index,procs[index].pi_esp);
printf("procs[%d].eip           =%lu\n",index,procs[index].pi_eip);
printf("procs[%d].pending_signal=%lu\n",index,procs[index].pi_pending_signal);
printf("procs[%d].blocked_sig   =%lu\n",index,procs[index].pi_blocked_sig);
printf("procs[%d].sigign        =%lu\n",index,procs[index].pi_sigign);
printf("procs[%d].sigcatch      =%lu\n",index,procs[index].pi_sigcatch);
printf("procs[%d].wchan         =%lu\n",index,procs[index].pi_wchan);
printf("procs[%d].nswap         =%lu\n",index,procs[index].pi_nswap);
printf("procs[%d].cnswap        =%lu\n",index,procs[index].pi_cnswap);
printf("procs[%d].exit_signal   =%d\n",index,procs[index].pi_exit_signal);
printf("procs[%d].cpu           =%d\n",index,procs[index].pi_cpu);
printf("OK\n");
}
#endif /*DEBUG*/
/* --- */

int isnumbers(char *s)
{
	while(*s != 0) {
		if( *s < '0' || *s > '9')
			return 0;
		s++;
	}
	return 1;
}

int getprocs(int details)
{
struct dirent *dent;
DIR *procdir;
int count =0;

	if((char *)(procdir = opendir("/proc")) == NULL) {
		printf("opendir(/proc) failed");
		return 0;
	}
	while( (char *)(dent = readdir(procdir)) != NULL ) {
		if(dent->d_type == 4) { /* is this a directlory */
/* mainframes report 0 = unknown every time !!!!  */
/*
		    printf("inode=%d type=%d name=%s\n",
				dent->d_ino,	
				dent->d_type,	
				dent->d_name);
*/
		    if(isnumbers(dent->d_name)) {
/*			printf("%s pid\n",dent->d_name); */
			if(details) {
				count=count+proc_procsinfo(atoi(dent->d_name),count);
			} else {
				count++;
			}
		    }
/*
		    else
			printf("NOT numbers\n");
*/
		}
	}
	closedir(procdir);
	return count;
}
/* --- */

char graph_line[] = "+-------------------------------------------------+";
/* Start process as specified in cmd in a child process without waiting
 * for completion
 * not sure if want to prevent this funcitonality for root user
 * when: CHLD_START, CHLD_SNAP or CHLD_END
 * cmd:  pointer to command string - assumed to be cleansed ....
 * timestamp_type: 0 - T%04d, 1 - detailed time stamp
 * loop: loop id (0 for CHLD_START)
 * the_time: time to use for timestamp generation
 */
void child_start(int when,
                char *cmd,
                int timestamp_type,
                int loop,
                time_t the_time)
{
        int i;
        pid_t child_pid;
        char time_stamp_str[20]="";
        char *when_info="";
        struct tm *tim; /* used to work out the hour/min/second */

#ifdef DEBUG2
fprintf(fp,"child start when=%d cmd=%s time=%d loop=%d\n",when,cmd,timestamp_type,loop);
#endif 
        /* Validate parameter and initialize error text */
        switch( when ) {
                case CHLD_START:
                        when_info = "elmon fork exec failure CHLD_START";
                        break;
                case CHLD_END:
                        when_info = "elmon fork exec failure CHLD_END";
                        break;

                case CHLD_SNAP:
                        /* check if old child has finished - otherwise we do nothing */
                        if( nmon_children[CHLD_SNAP] != -1 ) {
                                if(!cursed)fprintf(fp,"ERROR,T%04d, Starting snap command \"%s\" failed as previous child still running - killing it now\n", loop, cmd);
                                kill( nmon_children[CHLD_SNAP],9);
                        }

                        when_info = "elmon fork exec failure CHLD_SNAP";
                        break;
        }


        /* now fork off a child process. */
        switch (child_pid = fork()) {
                case -1:        /* fork failed. */
                        perror(when_info);
                        return;

                case 0:         /* inside child process.  */
                        /* create requested timestamp */
                        if( timestamp_type == 1 ) {
                                tim = localtime(&the_time);
                                sprintf(time_stamp_str,"%02d:%02d:%02d,%02d,%02d,%04d",
                                       tim->tm_hour, tim->tm_min, tim->tm_sec,
                                       tim->tm_mday, tim->tm_mon + 1, tim->tm_year + 1900);
                        }
                        else {
                                sprintf(time_stamp_str,"T%04d", loop);
                        }

                        /* close all open file pointers except the defaults */
                        for( i=3; i<5; ++i )
                                close(i);

                        /* Now switch to the defined command */
                        execlp(cmd, cmd, time_stamp_str,(void *)0);

                        /* If we get here the specified command could not be started */
                        perror(when_info);
                        exit(1);                     /* We can't do anything more */
                        /* never reached */

                default:        /* inside parent process. */
                        /* In father - remember child pid for future */
                        nmon_children[when] = child_pid;
        }
}

int main(int argc, char **argv)
{
	int secs;
	char mapch;
	//int old_cpus; //Doesn't appear to be needed (Brian)
	int mapnum;
	int mapmax=0;
	int cpu_idle;
	int cpu_user;
	int cpu_sys;
	int cpu_wait;
	int	n=0;			/* reusable counters */
	int	i=0;
	int	j=0;
	int	k=0;
	int	ret=0;
	int	max_sorted;
	int	skipped;
	double	elapsed;		/* actual seconds between screen updates */
	double	cpu_sum;
	double	cpu_busy;
	double	ftmp;
	int	nfs_first_time =1;
	int	vm_first_time =1;
#ifdef POWER
	int	lpar_first_time =1;
#endif /* POWER */
	int	smp_first_time =1;
	int	proc_first_time =1;
	pid_t	firstproc = (pid_t)0;
	pid_t childpid = -1;
	int ralfmode = 0;
	long	xfers;
	char	pgrp[32];
	struct tm *tim; /* used to work out the hour/min/second */
	float	total_busy;	/* general totals */
	float	total_rbytes;	/* general totals */
	float	total_wbytes;
	float	total_xfers;
	float	total_size;
	struct utsname uts;		/* UNIX name, version, etc */
	double top_disk_busy;
	char *top_disk_name;
	double disk_total;
	double disk_busy;
	double disk_read;
	double disk_size;
	double disk_write;
	double disk_xfers;
	double readers;
	double writers;
	double mem_free;
	double mem_used;
	double mem_cache;
	double mem_buffers;
	double swap_used;
	double swap_free;

	/* for popen on oslevel */
	char str[512];
	char * str_p;
	int varperftmp = 0;
	char *formatstring;
	char user_filename[512];
	char user_filename_set = 0;
	struct statfs statfs_buffer;
	float fs_size;
	float fs_free;
	float fs_size_used;
	char cmdstr[256];
	int disk_stats_read = 0;
	int updays, uphours, upmins;
	float v2c_total;
	float v2s_total;
	float v3c_total;
	float v3s_total;
	//int room =1;
	int errors=0;
	WINDOW * padmem = NULL;
	WINDOW * padmemgraph = NULL;
	WINDOW * padlarge = NULL;
	WINDOW * padpage = NULL;
	WINDOW * padker = NULL;
	WINDOW * padres = NULL;
	WINDOW * padnet = NULL;
	WINDOW * padneterr = NULL;
	WINDOW * padnfs = NULL;
	WINDOW * padcpu = NULL;
	WINDOW * padsmp = NULL;
	WINDOW * padlong = NULL;
	WINDOW * paddisk = NULL;
	WINDOW * paddg = NULL;
	WINDOW * padmap = NULL;
	WINDOW * padtop = NULL;
	WINDOW * padjfs = NULL;
	WINDOW * padlpar = NULL;
	WINDOW * padverb = NULL;
	WINDOW * padhelp = NULL;

        char  *nmon_start = (char *)NULL;
        char  *nmon_end   = (char *)NULL;
        char  *nmon_snap  = (char *)NULL;
        char  *nmon_tmp   = (char *)NULL;
        int   nmon_one_in  = 1;
        /* Flag what kind of time stamp we give to started children
         * 0: "T%04d"
         * 1: "hh:mm:ss,dd,mm,yyyy"
         */
        int   time_stamp_type =0;

#define MAXROWS 256
#define MAXCOLS 150

#define BANNER(pad,string) {mvwhline(pad, 0, 0, ACS_HLINE,maxcols-2); \
                                        wmove(pad,0,0); \
                                        wattron(pad,A_STANDOUT); \
                                        wprintw(pad," "); \
                                        wprintw(pad,string); \
                                        wprintw(pad," "); \
                                        wattroff(pad,A_STANDOUT); }

	/* check the user supplied options */
	progname = argv[0];
	for (i=(int)strlen(progname)-1;i>0;i--)
		if(progname[i] == '/') {
			progname = &progname[i+1];
		}

	if(getenv("NMONDEBUG") != NULL) 
		debug=1;
	if(getenv("NMONERROR") != NULL) 
		error_on=1;
	if(getenv("NMONBUG1") != NULL) 
		reread=1;
        if (getenv("NMONDEBUG") != NULL)
                debug = 1;

        if ((nmon_start = getenv("NMON_START")) != NULL) {
                nmon_start = check_call_string(nmon_start, "NMON_START");
        }

        if ((nmon_end = getenv("NMON_END")) != NULL) {
                nmon_end = check_call_string(nmon_end, "NMON_END");
        }

        if ((nmon_tmp = getenv("NMON_ONE_IN")) != NULL) {
                nmon_one_in = atoi(nmon_tmp);
                if( errno != 0 ) {
                        fprintf(stderr,"ERROR elmon: invalid NMON_ONE_IN shell variable\n");
                        nmon_one_in = 1;
                }
        }

        if ((nmon_snap = getenv("NMON_SNAP")) != NULL) {
                nmon_snap = check_call_string(nmon_snap, "NMON_SNAP");
        }

        if ((nmon_tmp = getenv("NMON_TIMESTAMP")) != NULL) {
                time_stamp_type = atoi(nmon_tmp);
                if (time_stamp_type != 0 && time_stamp_type != 1 )
                        time_stamp_type = 1;
        }
#ifdef DEBUG2
printf("NMON_START=%s.\n",nmon_start);
printf("NMON_END=%s.\n",nmon_end);
printf("NMON_SNAP=%s.\n",nmon_snap);
printf("ONE_IN=%d.\n",nmon_one_in);
printf("TIMESTAMP=%d.\n",time_stamp_type);
#endif 

#ifdef REREAD
		reread=1;
#endif
	for(i=0; i<CMDMAX;i++) {
		sprintf(cmdstr,"NMONCMD%d",i);
		cmdlist[i] = getenv(cmdstr);
		if(cmdlist[i] != 0) 
			cmdfound = i+1;
	}
	/* Setup long and short Hostname */
	gethostname(hostname, sizeof(hostname));
	strcpy(fullhostname, hostname);
	for (i = 0; i < sizeof(hostname); i++)
		if (hostname[i] == '.')
			hostname[i] = 0;
	if(run_name_set == 0)
		strcpy(run_name,hostname);

	/* Check the version of OS */
	uname(&uts);

	proc_init();

	while ( -1 != (i = getopt(argc, argv, "?Rhs:bc:d:DfF:r:tTxXzeEl:qpC:Vg:Nm:I:Z" ))) {
		switch (i) {
		case '?':
			hint();
			exit(0);
		case 'h':
			help();
			break;
		case 's':
			seconds = atof(optarg) * 10;
			if (seconds == 0 )
				seconds = 2 * 10;  //Default to 2 seconds
			break;
		case 'p':
			ralfmode = 1;
			break;
		case 'b':
			colour = 0;
			break;
		case 'c':
			maxloops = atoi(optarg);
			break;
		case 'N':
			add_option(SHOW_NFS);
			break;
		case 'm':
			if(chdir(optarg) == -1) {
				perror("changing directory failed");
				printf("Directory attempted was:%s\n",optarg);
				exit(993);
			}
			break;
		case 'I':
			ignore_procdisk_threshold = atof(optarg);
			break;
		case 'd':
			diskmax = atoi(optarg);
			break;
		case 'R':
			show_rrd = 1;
			go_background(288, 300);
			show_aaa = 0;
			show_para = 0;
			show_headings = 0;
			break;
		case 'r': strcpy(run_name,optarg); 
			run_name_set++;
			break;
		case 'F': /* background mode with user supplied filename */
			strcpy(user_filename,optarg);
			user_filename_set++;
			go_background(288, 300);
			break;

		case 'f': /* background mode i.e. for spread sheet output */
			go_background(288, 300);
			break;
		case 'T':
			show_args = ARGS_ONLY; /* drop through */
		case 't':
			add_option(SHOW_TOP); /* put top process output in spreadsheet mode */
			show_topmode = 3;
			break;
		case 'z': /* background mode for 1 day output to /var/perf/tmp */
			varperftmp++;
			go_background(4*24, 15*60);
			break;

		case 'x': /* background mode for 1 day capacity planning */
			add_option(SHOW_TOP);
			show_topmode = 3;
			go_background(4*24, 15*60);
			break;
		case 'X': /* background mode for 1 hour capacity planning */
			add_option(SHOW_TOP);
			show_topmode = 3;
			go_background(120, 30);
			break;
		case 'Z':
			show_raw=1;
			break;
		case 'l':
			disks_per_line = atoi(optarg);
			if(disks_per_line < 3 || disks_per_line >250) disks_per_line = 100;
			break;
		case 'C': /* commandlist argument */
			cmdlist[0] = malloc(strlen(optarg)+1); /* create buffer */
			strcpy(cmdlist[0],optarg);
			if(cmdlist[0][0]!= 0)
				cmdfound=1;
			for(i=0,j=1;cmdlist[0][i] != 0;i++) {
				if(cmdlist[0][i] == ':') {
					cmdlist[0][i] = 0;
					cmdlist[j] = &cmdlist[0][i+1];
					j++;
					cmdfound=j;
					if(j >= CMDMAX) break;
				}
			}
			break;
		case 'V': /* elmon version */
			printf("elmon verion %s\n",VERSION);
			exit(0);
			break;
                case 'g': /* disk groups */
			add_option(SHOW_DGROUP);
                        dgroup_loaded = 1;
                        dgroup_filename = optarg;
                        break;
		}
	}
	/* Set parameters if not set by above */
	if (maxloops == -1)
		maxloops = 9999999;
	if (seconds  == -1)
		seconds = 2 * 10;
        if (cursed)
		remove_option(SHOW_DGROUP);

	/* To get the pointers setup */
	switcher();

	/* Initialise the time stamps for the first loop */
	p->time = doubletime();
	q->time = doubletime();

	find_release();

	proc_read(P_STAT);
        for(i=1;i<proc[P_STAT].lines;i++) {
                if(strncmp("cpu",proc[P_STAT].line[i],3) == 0)
                        max_cpus = cpus=i;
                else
                        break;
        }
	proc_read(P_STAT);
	proc_cpu();
	proc_read(P_UPTIME);
	proc_read(P_LOADAVG);
	proc_kernel();
	memcpy(&q->cpu_total, &p->cpu_total, sizeof(struct cpu_stat));

	p->dk = malloc(sizeof(struct dsk_stat) * diskmax+1);	
	q->dk = malloc(sizeof(struct dsk_stat) * diskmax+1);	
	disk_busy_peak = malloc(sizeof(double) * diskmax);
	disk_rate_peak = malloc(sizeof(double) * diskmax);
	for(i=0;i<diskmax;i++) {
		disk_busy_peak[i]=0.0;
		disk_rate_peak[i]=0.0;
	}

	cpu_peak = malloc(sizeof(double) * 128); /* MAGIC */
	for(i=0;i<(max_cpus+1);i++)
		cpu_peak[i]=0.0;

	n = getprocs(0);
	p->procs = malloc(sizeof(struct procsinfo ) * n  +8);
	q->procs = malloc(sizeof(struct procsinfo ) * n  +8);
	p->nprocs = q->nprocs = n;

	/* Initialise the top processes table */
	topper = malloc(sizeof(struct topper ) * topper_size); /* round up */

	/* Get Disk Stats. */
	proc_disk(0.0);
	memcpy(q->dk, p->dk, sizeof(struct dsk_stat) * disks);

        /* load dgroup - if required */
        if (dgroup_loaded == 1) {
                load_dgroup(p->dk);
        }

	/* Get Network Stats. */
	proc_net();
	memcpy(q->ifnets, p->ifnets, sizeof(struct net_stat) * networks);
	for(i=0;i<networks;i++) {
		net_read_peak[i]=0.0;
		net_write_peak[i]=0.0;
	}

	/* Set the pointer ready for the next round */
	switcher();

	/* Initialise signal handlers so we can tidy up curses on exit */
	signal(SIGUSR1, interrupt);
	signal(SIGUSR2, interrupt);
	signal(SIGINT, interrupt);
	signal(SIGWINCH, interrupt);
	signal(SIGCHLD, interrupt);

	/* Start Curses */
	if (cursed) {
		initscr();
		#ifdef NCURSES_MOUSE_VERSION	
		if (NCURSES_MOUSE_VERSION > 0){   //See if ncurses supports mouse
			mmask_t mouse_mask;
			mouse_mask = mousemask(ALL_MOUSE_EVENTS, NULL);
			if (mouse_mask != 0 ) {
				mouse = 1;        //Mouse support is available
			}
		}
		#endif
		nodelay(stdscr, TRUE);
		noecho();
		keypad(stdscr,TRUE);
		cbreak();
		move(0, 0);
		refresh();
		COLOUR colour = has_colors();
        	COLOUR start_color();
                COLOUR init_pair((short)0,(short)7,(short)0); /* White */
                COLOUR init_pair((short)1,(short)1,(short)0); /* Red */
                COLOUR init_pair((short)2,(short)2,(short)0); /* Green */
                COLOUR init_pair((short)3,(short)3,(short)0); /* Yellow */
                COLOUR init_pair((short)4,(short)4,(short)0); /* Blue */
                COLOUR init_pair((short)5,(short)5,(short)0); /* Magenta */
                COLOUR init_pair((short)6,(short)6,(short)0); /* Cyan */
                COLOUR init_pair((short)7,(short)7,(short)0); /* White */
		COLOUR init_pair((short)8,(short)1,(short)1); /* Red background, red text */
		COLOUR init_pair((short)9,(short)2,(short)2); /* Green background, green text */
		COLOUR init_pair((short)10,(short)4,(short)4); /* Blue background, blue text */
		COLOUR init_pair((short)11,(short)3,(short)3); /* Yellow background, yellow text */
		COLOUR init_pair((short)12,(short)6,(short)6); /* Cyan background, cyan text */
		COLOUR init_pair((short)13,(short)5,(short)5); /* Magenta background, Magenta text */

		clear();
		maxcols = COLS;
		padlpar = newpad(11,MAXCOLS);
		padmap = newpad(24,MAXCOLS);
		padhelp = newpad(11,MAXCOLS);
		padmem = newpad(20,MAXCOLS);
		padmemgraph = newpad(8,MAXCOLS);
		padlarge = newpad(20,MAXCOLS);
		padpage = newpad(20,MAXCOLS);
		padcpu = newpad(20,MAXCOLS);
		padsmp = newpad(MAXROWS,MAXCOLS);
		padlong = newpad(MAXROWS,MAX_SNAPS);
		padnet = newpad(MAXROWS,MAXCOLS);
		padneterr = newpad(MAXROWS,MAXCOLS);
		paddisk = newpad(MAXROWS,MAXCOLS);
		paddg = newpad(MAXROWS,MAXCOLS);
		padjfs = newpad(MAXROWS,MAXCOLS);
		padker = newpad(5,MAXCOLS);
		padverb = newpad(8,MAXCOLS);
		padres = newpad(23,MAXCOLS);
		padnfs = newpad(25,MAXCOLS);
		padtop = newpad(MAXROWS,MAXCOLS);


	} else {
		/* Output the header lines for the spread sheet */
		timer = time(0);
		tim = localtime(&timer);
		tim->tm_year += 1900 - 2000;  /* read localtime() manual page!! */
		tim->tm_mon  += 1; /* because it is 0 to 11 */
		if(varperftmp)
			sprintf( str, "/var/perf/tmp/%s_%02d.nmon", hostname, tim->tm_mday);
		else if(user_filename_set)
			strcpy( str, user_filename);
		else
			sprintf( str, "%s_%02d%02d%02d_%02d%02d.nmon",
			hostname,
			tim->tm_year,
			tim->tm_mon,
			tim->tm_mday, 
			tim->tm_hour, 
			tim->tm_min);
		if((fp = fopen(str,"w")) ==0 ) {
			perror("elmon: failed to open output file");
			printf("elmon: output filename=%s\n",str);
			exit(42);
		}
		/* disconnect from terminal */
		fflush(NULL);
		if (!debug && (childpid = fork()) != 0) {
			if(ralfmode)
				printf("%d\n",childpid);
			exit(0); /* parent returns OK */
		}
		if(!debug) {
			close(0);
			close(1);
			close(2);
			setpgrp(); /* become process group leader */
			signal(SIGHUP, SIG_IGN); /* ignore hangups */
		}
                /* Do the nmon_start activity early on */
                if (nmon_start) {
                        timer = time(0);
                        child_start(CHLD_START, nmon_start, time_stamp_type, 1, timer);
                }

		if(show_aaa) {
		fprintf(fp,"AAA,progname,%s\n", progname);
		fprintf(fp,"AAA,command,");
		for(i=0;i<argc;i++)
			fprintf(fp,"%s ",argv[i]);
		fprintf(fp,"\n");
		fprintf(fp,"AAA,version,%s\n", VERSION);
		fprintf(fp,"AAA,disks_per_line,%d\n", disks_per_line);
		fprintf(fp,"AAA,max_disks,%d,set by -d option\n", diskmax);
		fprintf(fp,"AAA,disks,%d,\n", disks);

		fprintf(fp,"AAA,host,%s\n", hostname);
		fprintf(fp,"AAA,user,%s\n", getenv("USER"));
		fprintf(fp,"AAA,OS,Linux,%s,%s,%s\n",uts.release,uts.version,uts.machine); 
		fprintf(fp,"AAA,runname,%s\n", run_name);
		fprintf(fp,"AAA,time,%02d:%02d.%02d\n", tim->tm_hour, tim->tm_min, tim->tm_sec);
		fprintf(fp,"AAA,date,%02d-%3s-%02d\n", tim->tm_mday, month[tim->tm_mon-1], tim->tm_year+2000);
		fprintf(fp,"AAA,interval,%-6.1f\n", (double)seconds / (double)10);
		fprintf(fp,"AAA,snapshots,%d\n", maxloops);
		fprintf(fp,"AAA,cpus,%d,%d\n", cpus,cpus);
		fprintf(fp,"AAA,proc_stat_variables,%d\n", stat8);

		fprintf(fp,"AAA,note0, Warning - use the UNIX sort command to order this file before loading into a spreadsheet\n");
		fprintf(fp,"AAA,note1, The First Column is simply to get the output sorted in the right order\n");
		fprintf(fp,"AAA,note2, The T0001-T9999 column is a snapshot number. To work out the actual time; see the ZZZ section at the end\n");
		}
		fflush(NULL);

		for (i = 1; i <= cpus; i++)
			fprintf(fp,"CPU%02d,CPU %d %s,User%%,Sys%%,Wait%%,Idle%%\n", i, i, run_name);
		fprintf(fp,"CPU_ALL,CPU Total %s,User%%,Sys%%,Wait%%,Idle%%,Busy,CPUs\n", run_name);
		fprintf(fp,"MEM,Memory MB %s,memtotal,hightotal,lowtotal,swaptotal,memfree,highfree,lowfree,swapfree,memshared,cached,active,bigfree,buffers,swapcached,inactive\n", run_name);
		fprintf(fp,"PROC,Processes %s,Runnable,Swap-in,pswitch,syscall,read,write,fork,exec,sem,msg\n", run_name);
/*
		fprintf(fp,"PAGE,Paging %s,faults,pgin,pgout,pgsin,pgsout,reclaims,scans,cycles\n", run_name);
		fprintf(fp,"FILE,File I/O %s,iget,namei,dirblk,readch,writech,ttyrawch,ttycanch,ttyoutch\n", run_name);
*/


		fprintf(fp,"NET,Network I/O %s,", run_name);
		for (i = 0; i < networks; i++)
			fprintf(fp,"%-2s-read-KB/s,", (char *)p->ifnets[i].if_name);
		for (i = 0; i < networks; i++)
			fprintf(fp,"%-2s-write-KB/s,", (char *)p->ifnets[i].if_name);
		fprintf(fp,"\n");
		fprintf(fp,"NETPACKET,Network Packets %s,", run_name);
		for (i = 0; i < networks; i++)
			fprintf(fp,"%-2s-read/s,", (char *)p->ifnets[i].if_name);
		for (i = 0; i < networks; i++)
			fprintf(fp,"%-2s-write/s,", (char *)p->ifnets[i].if_name);
		/* iremoved as it is not below in the BUSY line fprintf(fp,"\n"); */
#ifdef DEBUG
		if(debug)printf("disks=%d x%sx\n",(char *)disks,p->dk[0].dk_name);
#endif /*DEBUG*/
		for (i = 0; i < disks; i++)  {
			if(NEWDISKGROUP(i))
			    fprintf(fp,"\nDISKBUSY%s,Disk %%Busy %s", dskgrp(i) ,run_name);
			fprintf(fp,",%s", (char *)p->dk[i].dk_name);
		}
		for (i = 0; i < disks; i++) {
			if(NEWDISKGROUP(i))
			    fprintf(fp,"\nDISKREAD%s,Disk Read KB/s %s", dskgrp(i),run_name);
			fprintf(fp,",%s", (char *)p->dk[i].dk_name);
		}
		for (i = 0; i < disks; i++) {
			if(NEWDISKGROUP(i))
			    fprintf(fp,"\nDISKWRITE%s,Disk Write KB/s %s", (char *)dskgrp(i),run_name);
			fprintf(fp,",%s", (char *)p->dk[i].dk_name);
		}
		for (i = 0; i < disks; i++) {
			if(NEWDISKGROUP(i))
				fprintf(fp,"\nDISKXFER%s,Disk transfers per second %s", (char *)dskgrp(i),run_name);
			fprintf(fp,",%s", p->dk[i].dk_name);
		}
		for (i = 0; i < disks; i++) {
			if(NEWDISKGROUP(i))
				fprintf(fp,"\nDISKBSIZE%s,Disk Block Size %s", dskgrp(i),run_name);
			fprintf(fp,",%s", (char *)p->dk[i].dk_name);
		}
		fprintf(fp,"\n");
		jfs_load(LOAD);
		fprintf(fp,"JFSFILE,JFS Filespace %%Used %s", hostname);
		for (k = 0; k < jfses; k++) {
  		    if(jfs[k].mounted && strncmp(jfs[k].name,"/proc",5)
  		    			&& strncmp(jfs[k].name,"/sys",4)
  		    			&& strncmp(jfs[k].name,"/dev/pts",8)
			)  /* /proc gives invalid/insane values */
			fprintf(fp,",%s", jfs[k].name); 
		}
		fprintf(fp,"\n");
		jfs_load(UNLOAD);
#ifdef POWER
		fprintf(fp,"LPAR,Shared CPU LPAR Stats %s,PhysicalCPU,capped,shared_processor_mode,system_potential_processors,system_active_processors,pool_capacity,MinEntCap,partition_entitled_capacity,partition_max_entitled_capacity,MinProcs,partition_active_processors,partition_potential_processors,capacity_weight,unallocated_capacity_weight,BoundThrds,MinMem,unallocated_capacity,pool_idle_time\n",hostname);
#endif /*POWER*/
		if(enabled_option(SHOW_TOP)){
			fprintf(fp,"TOP,%%CPU Utilisation\n");
			fprintf(fp,"TOP,+PID,Time,%%CPU,%%Usr,%%Sys,Size,ResSet,ResText,ResData,ShdLib,MajorFault,MinorFault,Command\n");
		}
		linux_bbbp("/etc/release",    "/bin/cat /etc/*ease 2>/dev/null", WARNING);
		linux_bbbp("lsb_release",    "/usr/bin/lsb_release -a 2>/dev/null", WARNING);
		linux_bbbp("fdisk-l",          "/sbin/fdisk -l 2>/dev/null", WARNING);
		linux_bbbp("/proc/cpuinfo",    "/bin/cat /proc/cpuinfo 2>/dev/null", WARNING);
		linux_bbbp("/proc/meminfo",    "/bin/cat /proc/meminfo 2>/dev/null", WARNING);
		linux_bbbp("/proc/stat",       "/bin/cat /proc/stat 2>/dev/null", WARNING);
		linux_bbbp("/proc/version",    "/bin/cat /proc/version 2>/dev/null", WARNING);
		linux_bbbp("/proc/net/dev",    "/bin/cat /proc/net/dev 2>/dev/null", WARNING);
		linux_bbbp("/proc/diskinfo",   "/bin/cat /proc/diskinfo 2>/dev/null", WARNING);
		linux_bbbp("/proc/diskstat",   "/bin/cat /proc/diskstat 2>/dev/null", WARNING);
		linux_bbbp("/proc/partitions", "/bin/cat /proc/partitions 2>/dev/null", WARNING);
		linux_bbbp("/proc/1/stat",     "/bin/cat /proc/1/stat 2>/dev/null", WARNING);
		linux_bbbp("/proc/1/statm",    "/bin/cat /proc/1/statm 2>/dev/null", WARNING);
#ifdef POWER
		linux_bbbp("/proc/ppc64/lparcfg",    "/bin/cat /proc/ppc64/lparcfg 2>/dev/null", WARNING);
#endif
#ifdef MAINFRAME
		linux_bbbp("/proc/sysinfo",    "/bin/cat /proc/sysinfo 2>/dev/null", WARNING);
#endif
		linux_bbbp("/proc/net/rpc/nfs",        "/bin/cat /proc/net/rpc/nfs 2>/dev/null", WARNING);
		linux_bbbp("/proc/net/rpc/nfsd",        "/bin/cat /proc/net/rpc/nfsd 2>/dev/null", WARNING);
		linux_bbbp("ifconfig",        "/sbin/ifconfig 2>/dev/null", WARNING);
		sleep(1); /* to get the first stats to cover this one second */
	     }
	/* To get the pointers setup */
	switcher();
	checkinput();
	fflush(NULL);
#ifdef POWER 
lparcfg.timebase = -1; 
#endif

        int last_cols = COLS;
        int last_lines = LINES;

	/* Main loop of the code */
	for(loop=1; ; loop++) {
	        if (last_num_col == 1){
	                maxcols = COLS;
	        }else if (last_num_col == 2){
	                maxcols = COLS/2;
	        }else {
	                maxcols = COLS/3;
	        }

		if (maxcols < MAX_SNAPS) {
			current_snaps = maxcols - 8;
		} else{
			current_snaps = MAX_SNAPS - 8;
		}
		
		disk_stats_read = 0;

		if(loop == 3) /* This stops the nmon causing the cpu peak at startup */
			for(i=0;i<(max_cpus+1);i++)
				cpu_peak[i]=0.0;
			
		/* Reset the cursor position to top left */
		y_1 = x_1 = x_2 = x_3 = 0;

		/* Save the time and work out how long we were actually asleep */
		p->time = doubletime();
		elapsed = p->time - q->time;
		timer = time(0);
		tim = localtime(&timer);
		if (cursed) { /* Top line */
	                        if (last_cols != COLS || last_lines != LINES){
	                                clear();
	                        }
	                        last_cols = COLS;
	                        last_lines = LINES;

				box(stdscr,0,0);
				mvprintw(x_1, 1, "elmon"); 
				mvprintw(x_1, 7, "%s", VERSION); 
				if(flash_on && seconds > 9)  //If screen is updating more than once per second, don't flash the help message
					mvprintw(x_1,15,"[H for help]");
				else if (seconds < 10)
					mvprintw(x_1,15,"[H for help]");
				
				mvprintw(x_1, 30, "Hostname=%s", hostname);

				if (seconds % 10 == 0)    //seconds = whole number of seconds
					mvprintw(x_1, 52, "Refresh=%2.0fsecs ", (double)seconds / (double)10);
				else
					mvprintw(x_1, 52, "Refresh=%2.1fsecs ", (double)seconds / (double)10);

                                mvprintw(x_1, 70, "%02d:%02d.%02d",
				    tim->tm_hour, tim->tm_min, tim->tm_sec);
				wnoutrefresh(stdscr);
			x_1 = x_1 + 1;
			x_2 = x_2 + 1;
			x_3 = x_3 + 1;

			if(welcome && getenv("ELMON") == 0) {

					COLOUR attrset(COLOR_PAIR(2));
mvprintw(x_1+1, 2, "-------------------------------------");
mvprintw(x_1+2, 2, "######  #      #    #   ####   #    #");
mvprintw(x_1+3, 2, "#       #      ##  ##  #    #  ##   #");
mvprintw(x_1+4, 2, "#       #      # ## #  #    #  # #  #");
mvprintw(x_1+5, 2, "####    #      #    #  #    #  #  # #");
mvprintw(x_1+6, 2, "#       #      #    #  #    #  #   ##");
mvprintw(x_1+7, 2, "######  #####  #    #   ####   #    #");
mvprintw(x_1+8, 2, "-------------------------------------");
					COLOUR attrset(COLOR_PAIR(0));
mvprintw(x_1+1, 43, "For help run ...");
mvprintw(x_1+2, 43, " elmon -?  - hint");
mvprintw(x_1+3, 43, " elmon -h  - full");
mvprintw(x_1+5, 43, "To start the same way every time");
mvprintw(x_1+6, 43, " set the ELMON shell variable");

#ifdef NCURSES_MOUSE_VERSION
if (mouse){
	mvprintw(x_1+10, 3, "For interactive help menu type H or click on the screen");
	mvprintw(x_1+11, 3, " Within menu, use keys or mouse click to toggle stats on/off");
	mvprintw(x_1+12, 3, " You can also use the arrow keys to highlight a option and press enter");
} else{
	mvprintw(x_1+10, 3, "For interactive help menu type H");
	mvprintw(x_1+11, 3, " Within menu, use keys to toggle stats on/off");
	mvprintw(x_1+12, 3, " You can also use the arrow keys to highlight a option and press enter");
}
#else
	mvprintw(x_1+10, 3, "For interactive help menu type H");
	mvprintw(x_1+11, 3, " Within menu, use keys to toggle stats on/off");
	mvprintw(x_1+12, 3, " You can also use the arrow keys to highlight a option and press enter");
#endif

mvprintw(x_1+14, 3, "");
mvprintw(x_1+15, 3, "elmon is developed by Brian Smith and is licensed under the GPL v3");
mvprintw(x_1+16, 3, "elmon is based on the nmon application by Nigel Griffiths");
mvprintw(x_1+17, 3, "");
mvprintw(x_1+18, 3, "This program is distributed in the hope that it will be useful,");
mvprintw(x_1+19, 3, "but WITHOUT ANY WARRANTY; without even the implied warranty of");
mvprintw(x_1+20, 3, "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.");
				x_1 = x_1 + 20;
			}
		} else {
			if (!cursed && nmon_snap && (loop % nmon_one_in) == 0 ) {
				child_start(CHLD_SNAP, nmon_snap, time_stamp_type, loop, timer);
			}


			if(!show_rrd)
			    fprintf(fp,"ZZZZ,%s,%02d:%02d:%02d,%02d-%s-%4d\n", LOOP, 
					tim->tm_hour, tim->tm_min, tim->tm_sec,
					tim->tm_mday, month[tim->tm_mon], tim->tm_year+1900);
			fflush(NULL);
		}
		if (enabled_option(SHOW_VERBOSE) && cursed) {
			BANNER(padverb, "Verbose Mode");
			mvwprintw(padverb,1, 0, " Code    Resource            Stats   Now\tWarn\tDanger ");
		/*	display(padverb,7); */
			/* move(x,0); */
			x_1=x_1+6;
		}

		if (enabled_option(SHOW_HELP) && cursed) {
			displayhelpmenu(padhelp);
			display(padhelp,11);
		}
/*
		if(error_on && errorstr[0] != 0) {
			mvprintw(x, 0, "Error: %s  ",errorstr);
			x = x + 1;
		}
*/

                if (enabled_option(SHOW_DISK) || enabled_option(SHOW_VERBOSE) || enabled_option(SHOW_DISKMAP || enabled_option(SHOW_DGROUP))) {
                        proc_read(P_STAT);
                        proc_disk(elapsed);
                }


                for(loop_options = 0; loop_options < optionCount; loop_options++){

                        if (enabled_options[loop_options] == SHOW_CPU && cursed) {
				proc_read(P_CPUINFO);
				proc_read(P_VERSION);
				BANNER(padcpu,"Linux and Processor Details");
				mvwprintw(padcpu,1, 4, "Linux: %s", proc[P_VERSION].line[0]);
				mvwprintw(padcpu,2, 4, "Build: %s", proc[P_VERSION].line[1]);
				mvwprintw(padcpu,3, 4, "Release  : %s", uts.release );
				mvwprintw(padcpu,4, 4, "Version  : %s", uts.version);
	#ifdef POWER
				mvwprintw(padcpu,5, 4, "cpuinfo: %s", proc[P_CPUINFO].line[1]);
				mvwprintw(padcpu,6, 4, "cpuinfo: %s", proc[P_CPUINFO].line[2]);
				mvwprintw(padcpu,7, 4, "cpuinfo: %s", proc[P_CPUINFO].line[3]);
				mvwprintw(padcpu,8, 4, "cpuinfo: %s", proc[P_CPUINFO].line[proc[P_CPUINFO].lines-1]);
	#else 
	#ifdef MAINFRAME
				mvwprintw(padcpu,5, 4, "cpuinfo: %s", proc[P_CPUINFO].line[1]);
				mvwprintw(padcpu,6, 4, "cpuinfo: %s", proc[P_CPUINFO].line[2]);
				mvwprintw(padcpu,7, 4, "cpuinfo: %s", proc[P_CPUINFO].line[3]);
				mvwprintw(padcpu,8, 4, "cpuinfo: %s", proc[P_CPUINFO].line[4]);
	#else /* Intel is the default */
				mvwprintw(padcpu,5, 4, "cpuinfo: %s", proc[P_CPUINFO].line[4]);
				mvwprintw(padcpu,6, 4, "cpuinfo: %s", proc[P_CPUINFO].line[1]);
				mvwprintw(padcpu,7, 4, "cpuinfo: %s", proc[P_CPUINFO].line[6]);
				mvwprintw(padcpu,8, 4, "cpuinfo: %s", proc[P_CPUINFO].line[17]);
	#endif /*MAINFRAME*/
	#endif /*POWER*/
				mvwprintw(padcpu,9, 4, "# of CPUs: %d", cpus);
				mvwprintw(padcpu,10, 4,"Machine  : %s", uts.machine);
				mvwprintw(padcpu,11, 4,"Nodename : %s", uts.nodename);
				mvwprintw(padcpu,12, 4,"/etc/*ease[1]: %s", easy[0]);
				mvwprintw(padcpu,13, 4,"/etc/*ease[2]: %s", easy[1]);
				mvwprintw(padcpu,14, 4,"/etc/*ease[3]: %s", easy[2]);
				mvwprintw(padcpu,15, 4,"/etc/*ease[4]: %s", easy[3]);
				mvwprintw(padcpu,16, 4,"lsb_release: %s", lsb_release[0]);
				mvwprintw(padcpu,17, 4,"lsb_release: %s", lsb_release[1]);
				mvwprintw(padcpu,18, 4,"lsb_release: %s", lsb_release[2]);
				mvwprintw(padcpu,19, 4,"lsb_release: %s", lsb_release[3]);
				display(padcpu,20);
			}
                        if (enabled_options[loop_options] == SHOW_LONGTERM ) {
					proc_read(P_STAT);
					proc_cpu();
					cpu_user = p->cpu_total.user - q->cpu_total.user; 
					cpu_sys  = p->cpu_total.sys  - q->cpu_total.sys; 
					cpu_wait = p->cpu_total.wait - q->cpu_total.wait; 
					cpu_idle = p->cpu_total.idle - q->cpu_total.idle; 
					cpu_sum = cpu_idle + cpu_user + cpu_sys + cpu_wait;
	
					plot_save(
					    (double)cpu_user / (double)cpu_sum * 100.0,
					    (double)cpu_sys  / (double)cpu_sum * 100.0,
					    (double)cpu_wait / (double)cpu_sum * 100.0,
					    (double)cpu_idle / (double)cpu_sum * 100.0);
					plot_snap(padlong);
					display(padlong,MAX_SNAP_ROWS+2);
			}
                        if (enabled_options[loop_options] == SHOW_SMP) {
				//Commented out the following 4 lines - I don't understand how the if statement would ever evaluate to true (Brian)
				/*old_cpus = cpus;
				if(old_cpus != cpus) {
					CURSE wclrtobot(padtop);
				}*/
				if(cpus>max_cpus && !cursed) {
					for (i = max_cpus+1; i <= cpus; i++)
						fprintf(fp,"CPU%02d,CPU %d %s,User%%,Sys%%,Wait%%,Idle%%\n", i, i, run_name);
					max_cpus= cpus;
				}
				if(cursed) {
				BANNER(padsmp,"CPU Utilisation");
				mvwprintw(padsmp,1, 27, graph_line);
	/*
	mvwprintw(padsmp,2, 0, "CPU  User%%  Sys%% Wait%% Idle|0          |25         |50          |75       100|");
	*/
				mvwprintw(padsmp,1, 27, graph_line);
				mvwprintw(padsmp,2, 0, "CPU  ");
				COLOUR wattrset(padsmp, COLOR_PAIR(2));
				mvwprintw(padsmp,2, 5, "User%%");
				COLOUR wattrset(padsmp, COLOR_PAIR(1));
				mvwprintw(padsmp,2, 10, "  Sys%%");
				COLOUR wattrset(padsmp, COLOR_PAIR(4));
				mvwprintw(padsmp,2, 16, " Wait%%");
				COLOUR wattrset(padsmp, COLOR_PAIR(0));
				mvwprintw(padsmp,2, 22, " Idle|0          |25         |50          |75       100|");
	
				}	
					proc_read(P_STAT);
					proc_cpu();
				for (i = 0; i < cpus; i++) {
					cpu_user = p->cpuN[i].user - q->cpuN[i].user; 
					cpu_sys  = p->cpuN[i].sys  - q->cpuN[i].sys; 
					cpu_wait = p->cpuN[i].wait - q->cpuN[i].wait; 
					cpu_idle = p->cpuN[i].idle - q->cpuN[i].idle; 
					cpu_sum = cpu_idle + cpu_user + cpu_sys + cpu_wait;
	                                if(smp_first_time && cursed) {
					    mvwprintw(padsmp,3 + i, 27, "| Please wait gathering data");
	                                } else {
					    if(!show_raw)
						plot_smp(padsmp,i+1, 3 + i, 
					    (double)cpu_user / (double)cpu_sum * 100.0, 
					    (double)cpu_sys  / (double)cpu_sum * 100.0, 
					    (double)cpu_wait / (double)cpu_sum * 100.0, 
					    (double)cpu_idle / (double)cpu_sum * 100.0);
					    else
						save_smp(padsmp,i+1, 3+i,
						  RAW(user) - RAW(nice),
						  RAW(sys),
						  RAW(wait),
						  RAW(idle),
						  RAW(nice),
						  RAW(irq),
						  RAW(softirq),
						  RAW(steal));
					   RRD fprintf(fp,"rrdtool update cpu%02d.rrd %s:%.1f:%.1f:%.1f:%.1f\n",i,LOOP,
					    (double)cpu_user / (double)cpu_sum * 100.0,
					    (double)cpu_sys  / (double)cpu_sum * 100.0,
					    (double)cpu_wait / (double)cpu_sum * 100.0,
					    (double)cpu_idle / (double)cpu_sum * 100.0);
					}
				}
				CURSE mvwprintw(padsmp,i + 3, 27, graph_line);
				cpu_user = p->cpu_total.user - q->cpu_total.user; 
				cpu_sys  = p->cpu_total.sys  - q->cpu_total.sys; 
				cpu_wait = p->cpu_total.wait - q->cpu_total.wait; 
				cpu_idle = p->cpu_total.idle - q->cpu_total.idle; 
				cpu_sum = cpu_idle + cpu_user + cpu_sys + cpu_wait;
	
				RRD fprintf(fp,"rrdtool update cpu.rrd %s:%.1f:%.1f:%.1f:%.1f\n",LOOP,
					    (double)cpu_user / (double)cpu_sum * 100.0,
					    (double)cpu_sys  / (double)cpu_sum * 100.0,
					    (double)cpu_wait / (double)cpu_sum * 100.0,
					    (double)cpu_idle / (double)cpu_sum * 100.0);
				if (cpus > 1 || !cursed) {
					if(!smp_first_time || !cursed) {
					    if(!show_raw) {
						plot_smp(padsmp,0, 4 + i,
					    (double)cpu_user / (double)cpu_sum * 100.0,
					    (double)cpu_sys  / (double)cpu_sum * 100.0,
					    (double)cpu_wait / (double)cpu_sum * 100.0,
					    (double)cpu_idle / (double)cpu_sum * 100.0);
					    } else {
						save_smp(padsmp,0, 4+i,
						  RAWTOTAL(user) - RAWTOTAL(nice),
						  RAWTOTAL(sys),
						  RAWTOTAL(wait),
						  RAWTOTAL(idle),
						  RAWTOTAL(nice),
						  RAWTOTAL(irq),
						  RAWTOTAL(softirq),
						  RAWTOTAL(steal));
					    }
					}
	
					CURSE mvwprintw(padsmp, i + 5, 27, graph_line);
					i = i + 2;
				}
	                        smp_first_time=0;
				display(padsmp, i + 4);
			    }
                        if(enabled_options[loop_options] == SHOW_VERBOSE && cursed) {
                                	proc_read(P_STAT);
                                	proc_cpu();
					cpu_user = p->cpu_total.user - q->cpu_total.user; 
					cpu_sys  = p->cpu_total.sys  - q->cpu_total.sys; 
					cpu_wait = p->cpu_total.wait - q->cpu_total.wait; 
					cpu_idle = p->cpu_total.idle - q->cpu_total.idle; 
					cpu_sum = cpu_idle + cpu_user + cpu_sys + cpu_wait;
	
					cpu_busy= (double)(cpu_user + cpu_sys)/ (double)cpu_sum * 100.0; 
					mvwprintw(padverb,2, 0, "        -> CPU               %%busy %5.1f%%\t>80%%\t>90%%          ",cpu_busy);
					if(cpu_busy > 90.0){
						COLOUR wattrset(padverb,COLOR_PAIR(1));
						mvwprintw(padverb,2, 0, " DANGER");
					}
					else if(cpu_busy > 80.0) {
						COLOUR wattrset(padverb,COLOR_PAIR(4));
						mvwprintw(padverb,2, 0, "Warning");
					}
					else  {
						COLOUR wattrset(padverb,COLOR_PAIR(2));
						mvwprintw(padverb,2, 0, "     OK");
					}
					COLOUR wattrset(padverb,COLOR_PAIR(0));
			}
	#ifdef POWER
                        if (enabled_options[loop_options] == SHOW_LPAR) {
				if(lparcfg.timebase == -1) {
					lparcfg.timebase=0;
					proc_read(P_CPUINFO);
					for(i=0;i<proc[P_CPUINFO].lines-1;i++) {
						if(!strncmp("timebase",proc[P_CPUINFO].line[i],8)) {
							sscanf(proc[P_CPUINFO].line[i],"timebase : %lld",&lparcfg.timebase);
							break;
						}
					}
				}
				ret = proc_lparcfg();
				if(cursed) {
					BANNER(padlpar,"LPAR Stats");
					if(ret == 0) {
					mvwprintw(padlpar,2, 0, "Reading data from /proc/ppc64/lparcfg failed");
					mvwprintw(padlpar,3, 0, "Either run as the root user or ");
					mvwprintw(padlpar,4, 0, "as the root user run: chmod ugo+r /proc/ppc64/lparcfg");
					} else {
					mvwprintw(padlpar,1, 0, "LPAR=%d  SerialNumber=%s  Type=%s",
						lparcfg.partition_id, lparcfg.serial_number, lparcfg.system_type);
					mvwprintw(padlpar,2, 0, "Flags:      Shared-CPU=%-5s  Capped=%-5s",
						lparcfg.shared_processor_mode?"true":"false",
						lparcfg.capped?"true":"false");
					mvwprintw(padlpar,3, 0, "Systems CPU Pool=%8.2f          Active=%8.2f    Total=%8.2f",
						(float)lparcfg.pool_capacity,
						(float)lparcfg.system_active_processors,
						(float)lparcfg.system_potential_processors);
					mvwprintw(padlpar,4, 0, "LPARs CPU    Min=%8.2f     Entitlement=%8.2f      Max=%8.2f",
						lparcfg.MinEntCap/100.0,
						lparcfg.partition_entitled_capacity/100.0,
						lparcfg.partition_max_entitled_capacity/100.0);
					mvwprintw(padlpar,5, 0, "Virtual CPU  Min=%8.2f          VP Now=%8.2f      Max=%8.2f",
						(float)lparcfg.MinProcs,
						(float)lparcfg.partition_active_processors,
						(float)lparcfg.partition_potential_processors);
					mvwprintw(padlpar,6, 0, "Memory       Min= unknown             Now=%8.2f      Max=%8.2f",
						(float)lparcfg.MinMem,
						(float)lparcfg.DesMem);
					mvwprintw(padlpar,7, 0, "Other     Weight=%8.2f   UnallocWeight=%8.2f Capacity=%8.2f",
						(float)lparcfg.capacity_weight,
						(float)lparcfg.unallocated_capacity_weight,
						(float)lparcfg.CapInc/100.0);
	
					mvwprintw(padlpar,8, 0, "      BoundThrds=%8.2f UnallocCapacity=%8.2f  Increment",
						(float)lparcfg.BoundThrds,
						(float)lparcfg.unallocated_capacity);
					if(lparcfg.purr_diff == 0 || lparcfg.timebase <1) {
						mvwprintw(padlpar,9, 0, "lparcfg: purr field always zero, upgrade to SLES9+sp1 or RHEL4+u1");
					} else {
	                                        if(lpar_first_time) {
						    mvwprintw(padlpar,9, 0, "Please wait gathering data");
	
	                                            lpar_first_time=0;
	                                        } else {
						    mvwprintw(padlpar,9, 0, "Physical CPU use=%8.3f ",
								(double)lparcfg.purr_diff/(double)lparcfg.timebase/elapsed);
						    if( lparcfg.pool_idle_time != NUMBER_NOT_VALID && lparcfg.pool_idle_saved != 0)
							    mvwprintw(padlpar,9, 29, "PoolIdleTime=%8.2f",
								(double)lparcfg.pool_idle_diff/(double)lparcfg.timebase/elapsed);
						    mvwprintw(padlpar,9, 54, "[timebase=%lld]", lparcfg.timebase);
						}
	                                       }
					}
					display(padlpar,10);
				} else {
					if(ret != 0)
					    fprintf(fp,"LPAR,%s,%9.6f,%d,%d,%d,%d,%d,%.1f,%.1f,%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%lld\n", 
						LOOP,
						(double)lparcfg.purr_diff/(double)lparcfg.timebase/elapsed,
						lparcfg.capped,
						lparcfg.shared_processor_mode,
						lparcfg.system_potential_processors,
						lparcfg.system_active_processors,
						lparcfg.pool_capacity,
						lparcfg.MinEntCap/100.0,
						lparcfg.partition_entitled_capacity/100.0,
						lparcfg.partition_max_entitled_capacity/100.0,
						lparcfg.MinProcs,
						lparcfg.partition_active_processors,
						lparcfg.partition_potential_processors,
						lparcfg.capacity_weight,
						lparcfg.unallocated_capacity_weight,
						lparcfg.BoundThrds,
						lparcfg.MinMem,
						lparcfg.unallocated_capacity,
						lparcfg.pool_idle_time);
				}
			}
	#endif /*POWER*/
	
	
                        if (enabled_options[loop_options] == SHOW_MEMORY_GRAPH && cursed) {
				int peak_col_mem;
				int peak_col_swap;
				proc_read(P_MEMINFO);
				proc_mem();
	
				mem_used = ((double)(p->mem.memtotal - p->mem.memfree - p->mem.buffers - p->mem.cached) / (double)p->mem.memtotal * 100);
				mem_free =  (double)p->mem.memfree / (double)p->mem.memtotal * 100;
				mem_cache =  (double)p->mem.cached / (double)p->mem.memtotal * 100;
				mem_buffers = (double)p->mem.buffers / (double)p->mem.memtotal * 100;
				swap_used = ((double)(p->mem.swaptotal - p->mem.swapfree) / (double)p->mem.swaptotal * 100 );
				swap_free = (double)p->mem.swapfree / (double)p->mem.swaptotal * 100; 
	
				if(mem_peak < (mem_used + mem_cache + mem_buffers) ) {
					mem_peak = (double)((int)mem_used/2 + (int)mem_cache/2 + (int)mem_buffers/2)*2.0;
				}
	
				BANNER(padmemgraph,"Memory Graphs");
	
				mvwprintw(padmemgraph, 1,  0, "Memory (%.1f MB Total)", p->mem.memtotal/1024.0);
	                        mvwprintw(padmemgraph, 1, 27, graph_line);
	
	                        COLOUR wattrset(padmemgraph, COLOR_PAIR(5));
	                        mvwprintw(padmemgraph,2, 0, "Used%%");
	                        COLOUR wattrset(padmemgraph, COLOR_PAIR(3));
	                        mvwprintw(padmemgraph,2, 6, "Buffers%%");
	                        COLOUR wattrset(padmemgraph, COLOR_PAIR(6));
	                        mvwprintw(padmemgraph,2, 15, "Cache%%");
	                        COLOUR wattrset(padmemgraph, COLOR_PAIR(0));
	                        mvwprintw(padmemgraph,2, 22, "Free%%");
	                        mvwprintw(padmemgraph,2, 27, "|0          |25         |50          |75       100|");
	
				mvwprintw(padmemgraph, 3,  0, "%3.1lf", mem_used);
				mvwprintw(padmemgraph, 3,  6, "%3.1lf", mem_buffers);
				mvwprintw(padmemgraph, 3, 15, "%3.1lf", mem_cache);
				mvwprintw(padmemgraph, 3, 22, "%3.1lf", mem_free);
				mvwprintw(padmemgraph, 3, 27, "|");
	
				wmove(padmemgraph,3, 28);
				for (i = 0; i < (int)((mem_used / 2)); i++){
					COLOUR wattrset(padmemgraph,COLOR_PAIR(13));
					wprintw(padmemgraph,"U");
					COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
				}
				for (i = 0; i < (int)((mem_buffers) / 2); i++){
					COLOUR wattrset(padmemgraph,COLOR_PAIR(11));
					wprintw(padmemgraph,"B");
					COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
				}
				for (i = 0; i < (int)((mem_cache) / 2); i++){
					COLOUR wattrset(padmemgraph,COLOR_PAIR(12));
					wprintw(padmemgraph,"C");
					COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
				}
				for (i = 0; i < (int)((mem_free) / 2); i++){
					COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
					wprintw(padmemgraph," ");
					COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
				}
	
				mvwprintw(padmemgraph, 3, 77, "|");
			
				peak_col_mem = 28 +(int)(mem_peak/2);
				if(peak_col_mem > 77)
					peak_col_mem=77;
				mvwprintw(padmemgraph, 3, peak_col_mem, ">");
	
				mvwprintw(padmemgraph, 4,  0, "Swap (%.1f MB Total)", p->mem.swaptotal/1024.0);
	                        mvwprintw(padmemgraph,4, 27, graph_line);
	
	                        COLOUR wattrset(padmemgraph, COLOR_PAIR(5));
	                        mvwprintw(padmemgraph,5, 0, "Used%%");
	                        COLOUR wattrset(padmemgraph, COLOR_PAIR(0));
	                        mvwprintw(padmemgraph,5, 22, "Free%%");
	                        mvwprintw(padmemgraph,5, 27, "|0          |25         |50          |75       100|");
				if(swap_peak < swap_used ) {
					swap_peak = (double)((int)swap_used/2)*2.0;
				}
	
	                        mvwprintw(padmemgraph, 6,  0, "%3.1lf", swap_used);
	                        mvwprintw(padmemgraph, 6, 22, "%3.1lf", swap_free);
	                        mvwprintw(padmemgraph, 6, 27, "|");
	
	                        wmove(padmemgraph,6, 28);
	                        for (i = 0; i < (int)((swap_used / 2)); i++){
	                                COLOUR wattrset(padmemgraph,COLOR_PAIR(13));
	                                wprintw(padmemgraph,"U");
	                                COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
	                        }
	                        for (i = 0; i < (int)((swap_free) / 2); i++){
	                                COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
	                                wprintw(padmemgraph," ");
	                                COLOUR wattrset(padmemgraph,COLOR_PAIR(0));
	                        }
	
	                        mvwprintw(padmemgraph, 6, 77, "|");
	
	                        peak_col_swap = 28 +(int)(swap_peak/2);
	                        if(peak_col_swap > 77)
	                                peak_col_swap=77;
	                        mvwprintw(padmemgraph, 6, peak_col_swap, ">");
	                        mvwprintw(padmemgraph,7, 27, graph_line);
	
				display(padmemgraph,8);
			}
	
                        if (enabled_options[loop_options] == SHOW_MEMORY) {
				proc_read(P_MEMINFO);
				proc_mem();
				if(cursed) {
					BANNER(padmem,"Memory Stats");
					mvwprintw(padmem,1, 1, "               RAM     High      Low     Swap");
					mvwprintw(padmem,2, 1, "Total MB    %8.1f %8.1f %8.1f %8.1f ",
						p->mem.memtotal/1024.0,
						p->mem.hightotal/1024.0,
						p->mem.lowtotal/1024.0,
						p->mem.swaptotal/1024.0);
					mvwprintw(padmem,3, 1, "Free  MB    %8.1f %8.1f %8.1f %8.1f ",
						p->mem.memfree/1024.0,
						p->mem.highfree/1024.0,
						p->mem.lowfree/1024.0,
						p->mem.swapfree/1024.0);
					mvwprintw(padmem,4, 1, "Free Percent %7.1f%% %7.1f%% %7.1f%% %7.1f%% ",
						p->mem.memfree  == 0 ? 0.0 : 100.0*(float)p->mem.memfree/(float)p->mem.memtotal,
						p->mem.highfree == 0 ? 0.0 : 100.0*(float)p->mem.highfree/(float)p->mem.hightotal,
						p->mem.lowfree  == 0 ? 0.0 : 100.0*(float)p->mem.lowfree/(float)p->mem.lowtotal,
						p->mem.swapfree == 0 ? 0.0 : 100.0*(float)p->mem.swapfree/(float)p->mem.swaptotal);
	
	
					mvwprintw(padmem,5, 1, "            MB                  MB                  MB");
	#ifdef LARGEMEM
					mvwprintw(padmem,6, 1, "                     Cached=%8.1f     Active=%8.1f",
						p->mem.cached/1024.0,
						p->mem.active/1024.0);
	#else
					mvwprintw(padmem,6, 1, " Shared=%8.1f     Cached=%8.1f     Active=%8.1f",
						p->mem.memshared/1024.0,
						p->mem.cached/1024.0,
						p->mem.active/1024.0);
					mvwprintw(padmem,5, 68, "MB");
					mvwprintw(padmem,6, 55, "bigfree=%8.1f",
						p->mem.bigfree/1024);
	#endif /*LARGEMEM*/
					mvwprintw(padmem,7, 1, "Buffers=%8.1f Swapcached=%8.1f  Inactive =%8.1f",
						p->mem.buffers/1024.0,
						p->mem.swapcached/1024.0,
						p->mem.inactive/1024.0);
	
					mvwprintw(padmem,8, 1, "Dirty  =%8.1f Writeback =%8.1f  Mapped   =%8.1f",
						p->mem.dirty/1024.0,
						p->mem.writeback/1024.0,
						p->mem.mapped/1024.0);
					mvwprintw(padmem,9, 1, "Slab   =%8.1f Commit_AS =%8.1f PageTables=%8.1f",
						p->mem.slab/1024.0,
						p->mem.committed_as/1024.0,
						p->mem.pagetables/1024.0);
					display(padmem,10);
				} else {
	
					if(show_rrd) 
					   str_p = "rrdtool update mem.rrd %s:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f\n";
					else
					   str_p = "MEM,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n";
					   fprintf(fp,str_p,
						LOOP,
						p->mem.memtotal/1024.0,
						p->mem.hightotal/1024.0,
						p->mem.lowtotal/1024.0,
						p->mem.swaptotal/1024.0,
						p->mem.memfree/1024.0,
						p->mem.highfree/1024.0,
						p->mem.lowfree/1024.0,
						p->mem.swapfree/1024.0,
						p->mem.memshared/1024.0,
						p->mem.cached/1024.0,
						p->mem.active/1024.0,
	#ifdef LARGEMEM
						-1.0,
	#else
						p->mem.bigfree/1024.0,
	#endif /*LARGEMEM*/
						p->mem.buffers/1024.0,
						p->mem.swapcached/1024.0,
						p->mem.inactive/1024.0);
				}
	/* for testing large page		p->mem.hugefree = 250;
			p->mem.hugetotal = 1000;
			p->mem.hugesize = 16*1024;
	*/
			}
                        if (enabled_options[loop_options] == SHOW_LARGE) {
				proc_read(P_MEMINFO);
				proc_mem();
				if(cursed) {
					BANNER(padlarge,"Large (Huge) Page Stats");
				    if(p->mem.hugetotal > 0) {
					if(p->mem.hugetotal - p->mem.hugefree > huge_peak)
						huge_peak = p->mem.hugetotal - p->mem.hugefree; 
					mvwprintw(padlarge,1, 1, "Total Pages=%7ld   100.0%%   Huge Page Size =%ld KB",    p->mem.hugetotal, p->mem.hugesize);
					mvwprintw(padlarge,2, 1, "Used  Pages=%7ld   %5.1f%%   Used Pages Peak=%-8ld",    
					(long)(p->mem.hugetotal - p->mem.hugefree), 
					(p->mem.hugetotal - p->mem.hugefree)/(float)p->mem.hugetotal*100.0, 
					huge_peak);
					mvwprintw(padlarge,3, 1, "Free  Pages=%7ld   %5.1f%%",    p->mem.hugefree, p->mem.hugefree/(float)p->mem.hugetotal*100.0);
				    } else {
					mvwprintw(padlarge,1, 1, " There are no Huge Pages");
					mvwprintw(padlarge,2, 1, " - see /proc/meminfo");
				    }
					display(padlarge,4);
				} else {
					if(p->mem.hugetotal > 0) {
						if(first_huge == 1){
							first_huge=0;
							fprintf(fp,"HUGEPAGES,Huge Page Use %s,HugeTotal,HugeFree,HugeSizeMB\n", run_name);
						}
						fprintf(fp,"HUGEPAGES,%s,%ld,%ld,%.1f\n", 
							LOOP,
							p->mem.hugetotal,
							p->mem.hugefree,
							p->mem.hugesize/1024.0);
					}
				}
			}
                        if (enabled_options[loop_options] == SHOW_VM) {
	#define VMDELTA(variable) (p->vm.variable - q->vm.variable)
	#define VMCOUNT(variable) (p->vm.variable                 )
				ret = read_vmstat();
				if(cursed) {
					BANNER(padpage,"Virtual-Memory");
					if(ret < 0 ) {
					    mvwprintw(padpage,2, 2, "Virtual Memory stats not supported with this kernel");
					    mvwprintw(padpage,3, 2, "/proc/vmstat only seems to appear in 2.6 onwards");
	
					} else {
					  if(vm_first_time) {
					    mvwprintw(padpage,2, 2, "Please wait - collecting data");
					    vm_first_time=0;
					  } else {
					    mvwprintw(padpage,1, 0, "nr_dirty    =%9lld pgpgin      =%8lld",
						VMCOUNT(nr_dirty),
						VMDELTA(pgpgin));
					    mvwprintw(padpage,2, 0, "nr_writeback=%9lld pgpgout     =%8lld",
						VMCOUNT(nr_writeback),
						VMDELTA(pgpgout));
					    mvwprintw(padpage,3, 0, "nr_unstable =%9lld pgpswpin    =%8lld",
						VMCOUNT(nr_unstable),
						VMDELTA(pswpin));
					    mvwprintw(padpage,4, 0, "nr_table_pgs=%9lld pgpswpout   =%8lld",
						VMCOUNT(nr_page_table_pages),
						VMDELTA(pswpout));
					    mvwprintw(padpage,5, 0, "nr_mapped   =%9lld pgfree      =%8lld",
						VMCOUNT(nr_mapped),
						VMDELTA(pgfree));
					    mvwprintw(padpage,6, 0, "nr_slab     =%9lld pgactivate  =%8lld",
						VMCOUNT(nr_slab),
						VMDELTA(pgactivate));
					    mvwprintw(padpage,7, 0, "                       pgdeactivate=%8lld",
						VMDELTA(pgdeactivate));
					    mvwprintw(padpage,8, 0, "allocstall  =%9lld pgfault     =%8lld  kswapd_steal     =%7lld",
						VMDELTA(allocstall),
						VMDELTA(pgfault),
						VMDELTA(kswapd_steal));
					    mvwprintw(padpage,9, 0, "pageoutrun  =%9lld pgmajfault  =%8lld  kswapd_inodesteal=%7lld",
						VMDELTA(pageoutrun),
						VMDELTA(pgmajfault),
						VMDELTA(kswapd_inodesteal));
					    mvwprintw(padpage,10, 0,"slabs_scanned=%8lld pgrotated   =%8lld  pginodesteal     =%7lld",
						VMDELTA(slabs_scanned),
						VMDELTA(pgrotated),
						VMDELTA(pginodesteal));
	
	
	
					    mvwprintw(padpage,1, 46, "              High Normal    DMA");
					    mvwprintw(padpage,2, 46, "alloc      %7lld%7lld%7lld",
						VMDELTA(pgalloc_high),
						VMDELTA(pgalloc_normal),
						VMDELTA(pgalloc_dma));
					    mvwprintw(padpage,3, 46, "refill     %7lld%7lld%7lld",
						VMDELTA(pgrefill_high),
						VMDELTA(pgrefill_normal),
						VMDELTA(pgrefill_dma));
					    mvwprintw(padpage,4, 46, "steal      %7lld%7lld%7lld",
						VMDELTA(pgsteal_high),
						VMDELTA(pgsteal_normal),
						VMDELTA(pgsteal_dma));
					    mvwprintw(padpage,5, 46, "scan_kswapd%7lld%7lld%7lld",
						VMDELTA(pgscan_kswapd_high),
						VMDELTA(pgscan_kswapd_normal),
						VMDELTA(pgscan_kswapd_dma));
					    mvwprintw(padpage,6, 46, "scan_direct%7lld%7lld%7lld",
						VMDELTA(pgscan_direct_high),
						VMDELTA(pgscan_direct_normal),
						VMDELTA(pgscan_direct_dma));
					  }
					}
					display(padpage,11);
				} else {
					if( ret < 0) {
						remove_option(SHOW_VM);
					} else if(vm_first_time) {
						vm_first_time=0;
	fprintf(fp,"VM,Paging and Virtual Memory,nr_dirty,nr_writeback,nr_unstable,nr_page_table_pages,nr_mapped,nr_slab,pgpgin,pgpgout,pswpin,pswpout,pgfree,pgactivate,pgdeactivate,pgfault,pgmajfault,pginodesteal,slabs_scanned,kswapd_steal,kswapd_inodesteal,pageoutrun,allocstall,pgrotated,pgalloc_high,pgalloc_normal,pgalloc_dma,pgrefill_high,pgrefill_normal,pgrefill_dma,pgsteal_high,pgsteal_normal,pgsteal_dma,pgscan_kswapd_high,pgscan_kswapd_normal,pgscan_kswapd_dma,pgscan_direct_high,pgscan_direct_normal,pgscan_direct_dma\n");
					} 
					if(show_rrd)
						str_p = "rrdtool update vm.rrd %s" 
						":%lld:%lld:%lld:%lld:%lld"
						":%lld:%lld:%lld:%lld:%lld"
						":%lld:%lld:%lld:%lld:%lld"
						":%lld:%lld:%lld:%lld:%lld"
						":%lld:%lld:%lld:%lld:%lld"
						":%lld:%lld:%lld:%lld:%lld"
						":%lld:%lld:%lld:%lld:%lld"
						":%lld:%lld\n";
					else
						str_p = "VM,%s" 
						",%lld,%lld,%lld,%lld,%lld"
						",%lld,%lld,%lld,%lld,%lld"
						",%lld,%lld,%lld,%lld,%lld"
						",%lld,%lld,%lld,%lld,%lld"
						",%lld,%lld,%lld,%lld,%lld"
						",%lld,%lld,%lld,%lld,%lld"
						",%lld,%lld,%lld,%lld,%lld"
						",%lld,%lld\n";
	
					fprintf(fp, str_p,
						LOOP,
						VMCOUNT(nr_dirty),
						VMCOUNT(nr_writeback),
						VMCOUNT(nr_unstable),
						VMCOUNT(nr_page_table_pages),
						VMCOUNT(nr_mapped),
						VMCOUNT(nr_slab),
						VMDELTA(pgpgin),
						VMDELTA(pgpgout),
						VMDELTA(pswpin),
						VMDELTA(pswpout),
						VMDELTA(pgfree),
						VMDELTA(pgactivate),
						VMDELTA(pgdeactivate),
						VMDELTA(pgfault),
						VMDELTA(pgmajfault),
						VMDELTA(pginodesteal),
						VMDELTA(slabs_scanned),
						VMDELTA(kswapd_steal),
						VMDELTA(kswapd_inodesteal),
						VMDELTA(pageoutrun),
						VMDELTA(allocstall),
						VMDELTA(pgrotated),
						VMDELTA(pgalloc_high),
						VMDELTA(pgalloc_normal),
						VMDELTA(pgalloc_dma),
						VMDELTA(pgrefill_high),
						VMDELTA(pgrefill_normal),
						VMDELTA(pgrefill_dma),
						VMDELTA(pgsteal_high),
						VMDELTA(pgsteal_normal),
						VMDELTA(pgsteal_dma),
						VMDELTA(pgscan_kswapd_high),
						VMDELTA(pgscan_kswapd_normal),
						VMDELTA(pgscan_kswapd_dma),
						VMDELTA(pgscan_direct_high),
						VMDELTA(pgscan_direct_normal),
						VMDELTA(pgscan_direct_dma));
				}
			}
                        if (enabled_options[loop_options] == SHOW_KERNEL) {
				proc_read(P_STAT);
				proc_cpu();
				proc_read(P_UPTIME);
				proc_read(P_LOADAVG);
				proc_kernel();
				if(cursed) {
					BANNER(padker,"Kernel Stats");
					mvwprintw(padker,1, 1, "RunQueue       %8lld   Load Average    CPU use since boot time",
						p->cpu_total.running);
						updays=p->cpu_total.uptime/60/60/24;
						uphours=(p->cpu_total.uptime-updays*60*60*24)/60/60;
						upmins=(p->cpu_total.uptime-updays*60*60*24-uphours*60*60)/60;
					mvwprintw(padker,2, 1, "ContextSwitch  %8.1f    1 mins %5.2f    Uptime Days=%3d Hours=%2d Mins=%2d",
						(float)(p->cpu_total.ctxt - q->cpu_total.ctxt)/elapsed,
						(float)p->cpu_total.mins1,
						updays, uphours, upmins);
						updays=p->cpu_total.idletime/60/60/24;
						uphours=(p->cpu_total.idletime-updays*60*60*24)/60/60;
						upmins=(p->cpu_total.idletime-updays*60*60*24-uphours*60*60)/60;
					mvwprintw(padker,3, 1, "Forks          %8.1f    5 mins %5.2f    Idle   Days=%3d Hours=%2d Mins=%2d",
						(float)(p->cpu_total.procs - q->cpu_total.procs)/elapsed,
						(float)p->cpu_total.mins5,
						updays, uphours, upmins);
	
					mvwprintw(padker,4, 1, "Interrupts     %8.1f   15 mins %5.2f    Average CPU use=%6.2f%%",
						(float)(p->cpu_total.intr - q->cpu_total.intr)/elapsed,
						(float)p->cpu_total.mins15,
						(float)(
						(p->cpu_total.uptime -
						p->cpu_total.idletime)/
						p->cpu_total.uptime *100.0));
					display(padker,5);
				} else {
					if(proc_first_time) {
						q->cpu_total.ctxt = p->cpu_total.ctxt;
						q->cpu_total.procs= p->cpu_total.procs;
						proc_first_time=0;
					}
	/*fprintf(fp,"PROC,Processes %s,Runnable,Swap-in,pswitch,syscall,read,write,fork,exec,sem,msg\n", run_name);*/
					if(show_rrd)
						str_p = "rrdtool update proc.rrd %s:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f\n";
					else
						str_p = "PROC,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n";
	
					fprintf(fp,str_p,
						LOOP,
						(float)p->cpu_total.running,/*runqueue*/
						-1.0,			/*swapin*/
									/*pswitch*/
						(float)(p->cpu_total.ctxt - q->cpu_total.ctxt)/elapsed,	
						-1.0,		/*syscall*/
						-1.0,		/*read*/
						-1.0,		/*write*/
								/*fork*/
						(float)(p->cpu_total.procs - q->cpu_total.procs)/elapsed,
						-1.0,		/*exec*/
						-1.0,		/*sem*/
						-1.0);		/*msg*/
				}
			}
	
                        if (enabled_options[loop_options] == SHOW_NFS) {
				proc_read(P_NFS);
				proc_read(P_NFSD);
				proc_nfs();
	
				if(cursed) {
					BANNER(padnfs,"Network Filesystem (NFS) I/O");
					mvwprintw(padnfs,1, 0, " Version 2     Client    Server");
					mvwprintw(padnfs,1, 41, "Version 3     Client    Server");
	#define NFS_TOTAL(member) (float)(p->member)
	#define NFS_DELTA(member) (((float)(p->member - q->member)/elapsed))
						v2c_total =0;
						v2s_total =0;
						v3c_total =0;
						v3s_total =0;
					for(i=0;i<18;i++) {
						mvwprintw(padnfs,2+i,  1, "%12s %8.1f %8.1f",
						nfs_v2_names[i],
						NFS_DELTA(nfs.v2c[i]),
						NFS_DELTA(nfs.v2s[i]));
						v2c_total +=NFS_DELTA(nfs.v2c[i]);
						v2s_total +=NFS_DELTA(nfs.v2s[i]);
					}
					for(i=0;i<22;i++) {
						mvwprintw(padnfs,2+i, 41, "%12s %8.1f %8.1f",
						nfs_v3_names[i],
						NFS_DELTA(nfs.v3c[i]),
						NFS_DELTA(nfs.v3s[i]));
						v3c_total +=NFS_DELTA(nfs.v3c[i]);
						v3s_total +=NFS_DELTA(nfs.v3s[i]);
					}
					mvwprintw(padnfs,2+19,  1, "%12s %8.1f %8.1f",
						"V2 Totals", v2c_total,v2s_total);
					mvwprintw(padnfs,2+20,  1, "%12s %8.1f %8.1f",
						"V3 Totals", v3c_total,v3s_total);
						
					display(padnfs,24);
				} else {
					if(nfs_first_time && ! show_rrd) {
						fprintf(fp,"NFSCLIV2,NFS Client v2");
						for(i=0;i<18;i++) 
							fprintf(fp,",%s",nfs_v2_names[i]);
						fprintf(fp,"\n");
						fprintf(fp,"NFSSVRV2,NFS Server v2");
						for(i=0;i<18;i++) 
							fprintf(fp,",%s",nfs_v2_names[i]);
						fprintf(fp,"\n");
	
						fprintf(fp,"NFSCLIV3,NFS Client v3");
						for(i=0;i<22;i++) 
							fprintf(fp,",%s",nfs_v3_names[i]);
						fprintf(fp,"\n");
						fprintf(fp,"NFSSVRV3,NFS Server v3");
						for(i=0;i<22;i++) 
							fprintf(fp,",%s",nfs_v3_names[i]);
						fprintf(fp,"\n");
						memcpy(&q->nfs,&p->nfs,sizeof(struct nfs_stat));
						nfs_first_time=0;
					}
					fprintf(fp,show_rrd ? "rrdtool update nfscliv2.rrd %s" : "NFSCLIV2,%s", LOOP);
					for(i=0;i<18;i++) {
						fprintf(fp,show_rrd ? ":%d" : ",%d", 
						(int)NFS_DELTA(nfs.v2c[i]));
					}
					fprintf(fp,"\n");
					fprintf(fp,show_rrd ? "rrdtool update nfsvriv2.rrd %s" : "NFSSVRV2,%s,", LOOP);
					for(i=0;i<18;i++) {
						fprintf(fp,show_rrd ? ":%d" : ",%d", 
						(int)NFS_DELTA(nfs.v2s[i]));
					}
					fprintf(fp,"\n");
					fprintf(fp,show_rrd ? "rrdtool update nfscliv3.rrd %s" : "NFSCLIV3,%s,", LOOP);
					for(i=0;i<22;i++) {
						fprintf(fp,show_rrd ? ":%d" : ",%d", 
						(int)NFS_DELTA(nfs.v3c[i]));
					}
					fprintf(fp,"\n");
					fprintf(fp,show_rrd ? "rrdtool update nfsvriv3.rrd %s" : "NFSSVRV3,%s,", LOOP);
					for(i=0;i<22;i++) {
						fprintf(fp,show_rrd ? ":%d" : ",%d", 
						(int)NFS_DELTA(nfs.v3s[i]));
					}
					fprintf(fp,"\n");
				}
			}
                        if (enabled_options[loop_options] == SHOW_NET) {
				if(cursed) {
				BANNER(padnet,"Network I/O");
				mvwprintw(padnet,1, 0, "I/F Name Recv=KB/s Trans=KB/s packin packout insize outsize Peak->Recv Trans");
				}
				proc_net();
				for (i = 0; i < networks; i++) {
	
	#define IFDELTA(member) ((float)( (q->ifnets[i].member > p->ifnets[i].member) ? 0 : (p->ifnets[i].member - q->ifnets[i].member)/elapsed) )
	#define IFDELTA_ZERO(member1,member2) ((IFDELTA(member1) == 0) || (IFDELTA(member2)== 0)? 0.0 : IFDELTA(member1)/IFDELTA(member2) )
	
					if(net_read_peak[i] < IFDELTA(if_ibytes) / 1024.0)
						net_read_peak[i] = IFDELTA(if_ibytes) / 1024.0;
					if(net_write_peak[i] < IFDELTA(if_obytes) / 1024.0)
						net_write_peak[i] = IFDELTA(if_obytes) / 1024.0;
	
					CURSE mvwprintw(padnet,2 + i, 0, "%6.6s %7.1f %7.1f    %6.1f   %6.1f  %6.1f %6.1f    %7.1f %7.1f   ",
					    &p->ifnets[i].if_name[0],
					    IFDELTA(if_ibytes) / 1024.0,   
					    IFDELTA(if_obytes) / 1024.0, 
					    IFDELTA(if_ipackets), 
					    IFDELTA(if_opackets),
					    IFDELTA_ZERO(if_ibytes, if_ipackets),
					    IFDELTA_ZERO(if_obytes, if_opackets),
					    net_read_peak[i],
					    net_write_peak[i]
						);
				}
				display(padnet,networks + 2);
				if (!cursed) {
					fprintf(fp,show_rrd ? "rrdtool update net.rrd %s" : "NET,%s,", LOOP);
					for (i = 0; i < networks; i++) {
						fprintf(fp,show_rrd ? ":%.1f" : "%.1f,", IFDELTA(if_ibytes) / 1024.0);
					}
					for (i = 0; i < networks; i++) {
						fprintf(fp,show_rrd ? ":%.1f" : "%.1f,", IFDELTA(if_obytes) / 1024.0);
					}
					fprintf(fp,"\n");
					fprintf(fp,show_rrd ? "rrdtool update netpacket.rrd %s" : "NETPACKET,%s,", LOOP);
					for (i = 0; i < networks; i++) {
						fprintf(fp,show_rrd ? ":%.1f" : "%.1f,", IFDELTA(if_ipackets) );
					}
					for (i = 0; i < networks; i++) {
						fprintf(fp,show_rrd ? ":%.1f" : "%.1f,", IFDELTA(if_opackets) );
					}
					fprintf(fp,"\n");
				}
				errors=0;
				for (i = 0; i < networks; i++) {
					errors += p->ifnets[i].if_ierrs - q->ifnets[i].if_ierrs
						+ p->ifnets[i].if_oerrs - q->ifnets[i].if_oerrs
						+ p->ifnets[i].if_ocolls - q->ifnets[i].if_ocolls;
				}
				if(errors) show_neterror=3;
				if(show_neterror) {
					if(cursed) {
					BANNER(padneterr,"Network Error Counters");
					mvwprintw(padneterr,1, 0, "I/F Name iErrors iDrop iOverrun iFrame oErrors   oDrop oOverrun oCarrier oColls ");
					}
					for (i = 0; i < networks; i++) {
						CURSE mvwprintw(padneterr,2 + i, 0, "%6.6s %7lu %7lu %7lu %7lu %7lu %7lu %7lu %7lu %7lu",
						    &p->ifnets[i].if_name[0],
						    p->ifnets[i].if_ierrs,   
						    p->ifnets[i].if_idrop,   
						    p->ifnets[i].if_ififo,   
						    p->ifnets[i].if_iframe,   
						    p->ifnets[i].if_oerrs,   
						    p->ifnets[i].if_odrop,   
						    p->ifnets[i].if_ofifo,   
						    p->ifnets[i].if_ocarrier,   
						    p->ifnets[i].if_ocolls);   
	
					}
					display(padneterr,networks + 2);
					if(show_neterror > 0) show_neterror--;
				}
			}
	#ifdef JFS
                        if (enabled_options[loop_options] == SHOW_JFS) {
			    if(cursed) {
				BANNER(padjfs,"Filesystems");
				mvwprintw(padjfs,1, 0, "Filesystem            SizeMB  FreeMB %%Used Type     MountPoint");
	
				for (k = 0; k < jfses; k++) {
					fs_size=0;
					fs_free=0;
					fs_size_used=100.0;
				    if(jfs[k].mounted) {
					if(!strncmp(jfs[k].name,"/proc/",6)       /* sub directorys have to be fake too */
					       || !strncmp(jfs[k].name,"/sys/",5)
					       || !strncmp(jfs[k].name,"/dev/",5)
					       || !strncmp(jfs[k].name,"/proc",6) /* one more than the string to ensure the NULL */
					       || !strncmp(jfs[k].name,"/sys",5)
					       || !strncmp(jfs[k].name,"/dev",5)
						) { /* /proc gives invalid/insane values */
						mvwprintw(padjfs,2+k, 0, "%-14s", jfs[k].name);
						mvwprintw(padjfs,2+k, 43, "%-8s not a real filesystem",jfs[k].type);
					} else {
						statfs_buffer.f_blocks=0;
					    if((ret=fstatfs( jfs[k].fd, &statfs_buffer)) != -1) {
						if(statfs_buffer.f_blocks != 0) {
						fs_size = (float)statfs_buffer.f_blocks*4.0/1024.0;
						fs_free = (float)statfs_buffer.f_bfree*4.0/1024.0;
						fs_size_used = ((float)statfs_buffer.f_blocks - (float)statfs_buffer.f_bfree)/(float)statfs_buffer.f_blocks*100.0;
	
						if( (i=strlen(jfs[k].device)) <20)
							str_p=&jfs[k].device[0];
						else {
							str_p=&jfs[k].device[i-20];
						}
					    mvwprintw(padjfs,2+k, 0, "%-20s %7.1f %7.1f %5.1f %-8s %s",
						str_p,
						fs_size,
						fs_free,
						fs_size_used,
						jfs[k].type,
						jfs[k].name
						);
	
						} else {
						mvwprintw(padjfs,2+k, 0, "%s", jfs[k].name);
						mvwprintw(padjfs,2+k, 43, "%-8s fstatfs returned zero blocks!!", jfs[k].type);
						}
					    }
					    else {
						mvwprintw(padjfs,2+k, 0, "%s", jfs[k].name);
						mvwprintw(padjfs,2+k, 43, "%-8s statfs failed", jfs[k].type);
					    }
					}
				    } else {
						mvwprintw(padjfs,2+k, 0, "%-14s", jfs[k].name);
						mvwprintw(padjfs,2+k, 43, "%-8s not mounted",jfs[k].type);
				    }
				}
				display(padjfs,2 + jfses);
			    } else {
				jfs_load(LOAD);
				fprintf(fp,show_rrd ? "rrdtool update jfsfile.rrd %s" : "JFSFILE,%s", LOOP);
				for (k = 0; k < jfses; k++) {
				    if(jfs[k].mounted && strncmp(jfs[k].name,"/proc",5)
							&& strncmp(jfs[k].name,"/sys",4)
							&& strncmp(jfs[k].name,"/dev/pts",8)
					)   { /* /proc gives invalid/insane values */
						    if(fstatfs( jfs[k].fd, &statfs_buffer) != -1) {
						fprintf(fp, show_rrd ? ":%.1f" : ",%.1f",
						((float)statfs_buffer.f_blocks - (float)statfs_buffer.f_bfree)/(float)statfs_buffer.f_blocks*100.0);
					    }
					    else
						fprintf(fp, show_rrd? ":U" : ",0.0");
					}
				}
				fprintf(fp, "\n");
				jfs_load(UNLOAD);
			    }
			}
	
	#endif /* JFS */
	
                        if (enabled_options[loop_options] == SHOW_DISKMAP) {
				BANNER(padmap,"Disk %%Busy Map");
				mvwprintw(padmap,0, 20,"Key: #=80%% X=60%% O=40%% o=30%% +=20%% -=10%% .=5%% _=0%%");
				mvwprintw(padmap,1, 0,"             hdisk(s)  1         2         3         4         5         6   ");
				mvwprintw(padmap,2, 0,"             0123456789012345678901234567890123456789012345678901234567890123");
				mvwprintw(padmap,3, 0,"hdisk0 to 63 ");
				for (i = 0; i < disks; i++) {
					disk_busy = DKDELTA(dk_time) / elapsed;
					disk_read = DKDELTA(dk_rkb) / elapsed;
					disk_write = DKDELTA(dk_wkb) / elapsed;
					if(disk_busy >80) mapch = '#';
					else if(disk_busy>60) mapch = 'X';
					else if(disk_busy>40) mapch = 'O';
					else if(disk_busy>30) mapch = 'o';
					else if(disk_busy>20) mapch = '+';
					else if(disk_busy>10) mapch = '-';
					else if(disk_busy> 5) mapch = '.';
					else mapch = '_';
	/*
					if( strlen( p->dk[i].dk_name) < 5 || !isdigit(p->dk[i].dk_name[5]) )
						continue;
	*/
					mapnum = atoi(&p->dk[i].dk_name[5]);
					if(mapnum >mapmax) mapmax = mapnum;
	#define MAPWRAP 64
					mvwprintw(padmap,3 + (int)(mapnum/MAPWRAP), 13+ (mapnum%MAPWRAP), "%c",mapch);
				}
				display(padmap,4 + mapmax/MAPWRAP);
			}


                        if (enabled_options[loop_options] == SHOW_DISK || enabled_options[loop_options] == SHOW_VERBOSE) {
				if (cursed) {
				    if(enabled_option(SHOW_DISK)) {
					BANNER(paddisk,"Disk I/O");
					switch(disk_mode) {
					case DISK_MODE_PARTITIONS: mvwprintw(paddisk, 0, 15, "(/proc/partitions)");break;
					case DISK_MODE_DISKSTATS:  mvwprintw(paddisk, 0, 15, "(/proc/diskstats)");break;
					case DISK_MODE_IO: mvwprintw(paddisk, 0, 15, "(/proc/stat+disk_io)");break;
					}
					mvwprintw(paddisk,0, 40, "all data is Kbytes per second");
					switch (show_disk_mode) {
					case SHOW_DISK_STATS: 
						mvwprintw(paddisk,1, 0, "DiskName Busy    Read    Write       Xfers   Size  Peak%%  Peak-RW    InFlight ");
						break;
					case SHOW_DISK_GRAPH: 
						if(colour){
							mvwprintw(paddisk,1, 0, "DiskName Busy  ");
							COLOUR wattrset(paddisk,COLOR_PAIR(6));
							mvwprintw(paddisk,1, 15, "Read ");
							COLOUR wattrset(paddisk,COLOR_PAIR(3));
							mvwprintw(paddisk,1, 20, "Write");
							COLOUR wattrset(paddisk,COLOR_PAIR(0));
							mvwprintw(paddisk,1, 25, "KB|0          |25         |50          |75       100|");
						}else{
							mvwprintw(paddisk,1, 0, "DiskName Busy  Read WriteKB|0          |25         |50          |75       100|");
						}
						break;
					}
				   }
					top_disk_busy = 0.0;
					top_disk_name = "";
					for (i = 0,k=0; i < disks; i++) {
	/*
						if(p->dk[i].dk_name[0] == 'h')
							continue;
	*/
						disk_busy = DKDELTA(dk_time) / elapsed;
						disk_read = DKDELTA(dk_rkb) / elapsed;
						disk_write = DKDELTA(dk_wkb) / elapsed;
						if( disk_busy > top_disk_busy) {
							top_disk_busy = disk_busy;
							top_disk_name = p->dk[i].dk_name;
						}
						xfers = DKDELTA(dk_xfers);
						if(disk_busy_peak[i] < disk_busy)
							disk_busy_peak[i] = disk_busy;
						if(disk_rate_peak[i] < (disk_read+disk_write))
							disk_rate_peak[i] = disk_read+disk_write;
						if(!show_all && disk_busy < 1)
							continue;
	
						if(strlen(p->dk[i].dk_name) > 8)
							str_p = &p->dk[i].dk_name[strlen(p->dk[i].dk_name) -8];
						else
							str_p = &p->dk[i].dk_name[0];
	
						if(show_disk_mode == SHOW_DISK_STATS) {
							/* output disks stats */
							mvwprintw(paddisk,2 + k, 0, "%-8s %3.0f%% %8.1f %8.1fKB/s %6.1f %5.1fKB  %3.0f%% %9.1fKB/s %3d",
							    str_p, 
							    disk_busy,
							    disk_read,
							    disk_write,
							    (double)xfers / elapsed,
							    xfers == 0 ? 0.0 : 
							    (DKDELTA(dk_rkb) + DKDELTA(dk_wkb) ) / xfers,
							    disk_busy_peak[i],
							    disk_rate_peak[i],
							    p->dk[i].dk_inflight);
							    k++;
						}
						if(show_disk_mode == SHOW_DISK_GRAPH) {
								int peak_col = 0;
								/* output disk bar graphs */
								mvwprintw(paddisk,2 + k, 0, "%-8s %3.0f%% %6.1f %6.1f",
								    str_p, 
									disk_busy,
									disk_read,
									disk_write);
								mvwprintw(paddisk,2 + k, 27, "|                                                  ");
								wmove(paddisk,2 + k, 28);
								if(disk_busy >100) disk_busy=100;
								if( disk_busy > 0.0 && (disk_write+disk_read) > 0.1) {
								readers = disk_busy*disk_read/(disk_write+disk_read);
								writers = disk_busy*disk_write/(disk_write+disk_read);
								if(readers + writers > 100) {
									readers=0;
									writers=0;
								}
								for (j = 0; j < writers/2; j++) {
									COLOUR wattrset(paddisk,COLOR_PAIR(11));
									wprintw(paddisk,"W");
									COLOUR wattrset(paddisk,COLOR_PAIR(0));
								}
								for (j = 0; j < readers/2; j++) {
									COLOUR wattrset(paddisk,COLOR_PAIR(12));
									wprintw(paddisk,"R");
									COLOUR wattrset(paddisk,COLOR_PAIR(0));
								}
								for (j = disk_busy; j < 50; j++)
									wprintw(paddisk," ");
								} else {
									for (j = 0; j < 50; j++)
										wprintw(paddisk," ");
									if(p->dk[i].dk_time == 0.0) 
										mvwprintw(paddisk,2 + k, 27, "| disk busy not available");
								     }
								if(disk_busy_peak[i] >100)
								   disk_busy_peak[i]=100;
	
								mvwprintw(paddisk,2 + i, 77, "| ");
	
								peak_col = 28 + (int)(disk_busy_peak[i]/2);
								if (peak_col > 77){
									peak_col = 77;	
								}
								mvwprintw(paddisk,2 + i, peak_col, ">");
								k++;
						}
					}
                                        if (enabled_options[loop_options] == SHOW_DISK){
						display(paddisk,2 + k);
					}
                                        if(enabled_options[loop_options] == SHOW_VERBOSE) {
						if(top_disk_busy > 80.0) {
							COLOUR wattrset(padverb,COLOR_PAIR(1));
							mvwprintw(padverb,3, 0, " DANGER");
						}
						else if(top_disk_busy > 60.0) {
							COLOUR wattrset(padverb,COLOR_PAIR(4));
							mvwprintw(padverb,3, 0, "Warning");
						}
						else  {
							COLOUR wattrset(padverb,COLOR_PAIR(2));
							mvwprintw(padverb,3, 0, "     OK");
						}
						COLOUR wattrset(padverb,COLOR_PAIR(0));
						mvwprintw(padverb,3, 8, "-> Top Disk %8s %%busy %5.1f%%\t>40%%\t>60%%          ",top_disk_name,top_disk_busy);
						//move(x,0);
					}
				} else {
					for (i = 0; i < disks; i++) {
						if(NEWDISKGROUP(i))
							fprintf(fp,show_rrd ? "%srrdtool update diskbusy%s.rrd %s" : "%sDISKBUSY%s,%s",i == 0 ? "": "\n", dskgrp(i), LOOP);
						/* check percentage is correct */
						ftmp = DKDELTA(dk_time) / elapsed;
						if(ftmp > 100.0 || ftmp < 0.0)
							fprintf(fp,show_rrd ? ":U" : ",101.00");
						else
							fprintf(fp,show_rrd ? ":%.1f" : ",%.1f",
								DKDELTA(dk_time) / elapsed);
					}
					for (i = 0; i < disks; i++) {
						if(NEWDISKGROUP(i))
							fprintf(fp,show_rrd ? "\nrrdtool update diskread%s.rrd %s" : "\nDISKREAD%s,%s", dskgrp(i),LOOP);
						fprintf(fp,show_rrd ? ":%.1f" : ",%.1f",
						    DKDELTA(dk_rkb) / elapsed);
					}
					for (i = 0; i < disks; i++) {
						if(NEWDISKGROUP(i))
							fprintf(fp,show_rrd ? "\nrrdtool update diskwrite%s.rrd %s" : "\nDISKWRITE%s,%s", dskgrp(i),LOOP);
						fprintf(fp,show_rrd ? ":%.1f" : ",%.1f",
						    DKDELTA(dk_wkb) / elapsed);
					}
					for (i = 0; i < disks; i++) {
						if(NEWDISKGROUP(i))
							fprintf(fp,show_rrd ? "\nrrdtool update diskxfer%s.rrd %s" : "\nDISKXFER%s,%s", dskgrp(i),LOOP);
						xfers = DKDELTA(dk_xfers);
						fprintf(fp,show_rrd ? ":%.1f" : ",%.1f",
							    (double)xfers / elapsed);
					}
					for (i = 0; i < disks; i++) {
						if(NEWDISKGROUP(i))
							fprintf(fp,show_rrd ? "\nrrdtool update diskbsize%s.rrd %s" : "\nDISKBSIZE%s,%s", dskgrp(i),LOOP);
						xfers = DKDELTA(dk_xfers);
						fprintf(fp,show_rrd ? ":%.1f" : ",%.1f",
							    xfers == 0 ? 0.0 : 
							    (DKDELTA(dk_rkb) + DKDELTA(dk_wkb) ) / xfers);
					}
					fprintf(fp,"\n");
				}
			}
                        if ((enabled_options[loop_options] == SHOW_DGROUP || (!cursed && dgroup_loaded))) {
				if (cursed) {
					BANNER(paddg,"Disk-Group-I/O");
					if (dgroup_loaded != 2 || dgroup_total_disks == 0) {
						mvwprintw(paddg, 1, 1, "No Disk Groups found use -g groupfile when starting elmon");
						n = 0;
					} else {
						mvwprintw(paddg, 1, 1, "Name          Disks AvgBusy Read|Write-KB/s  TotalMB/s   xfers/s BlockSizeKB");
						total_busy   = 0.0;
						total_rbytes = 0.0;
						total_wbytes = 0.0;
						total_xfers  = 0.0;
						for(k = n = 0; k < dgroup_total_groups; k++) {
							if (dgroup_name[k] == 0 )
								continue;
							disk_busy   = 0.0;
							disk_read = 0.0;
							disk_write = 0.0;
							disk_xfers  = 0.0;
							for (j = 0; j < dgroup_disks[k]; j++) {
								i = dgroup_data[k*DGROUPS+j];
								if (i != -1) {
									disk_busy   += DKDELTA(dk_time) / elapsed;
									disk_read += DKDELTA(dk_reads) * p->dk[i].dk_bsize / 1024.0 /elapsed;
									disk_write += DKDELTA(dk_writes) * p->dk[i].dk_bsize / 1024.0 /elapsed;
									disk_xfers  += DKDELTA(dk_xfers) /elapsed;
								}
							}
							if (dgroup_disks[k] == 0)
								disk_busy = 0.0;
							else
								disk_busy = disk_busy / dgroup_disks[k];
							total_busy += disk_busy;
							total_rbytes += disk_read;
							total_wbytes += disk_write;
							total_xfers  += disk_xfers;
							//if (!show_all && (disk_read < 1.0 && disk_write < 1.0))
						//		continue;
							if ((disk_read + disk_write) == 0 || disk_xfers == 0)
								total_size = 0.0;
							else
								total_size = ((float)disk_read + (float)disk_write) / (float)disk_xfers;
							mvwprintw(paddg, n + 2, 1, "%-14s   %3d %5.1f%% %9.1f|%-9.1f %6.1f %9.1f %6.1f ",
								 dgroup_name[k], 
								 dgroup_disks[k],
								 disk_busy,
								 disk_read,
								 disk_write,
								 (disk_read + disk_write) / 1024, /* in MB */
								 disk_xfers,
								 total_size
								 );
							n++;
						}
						mvwprintw(paddg, n + 2, 1, "Groups=%2d TOTALS %3d %5.1f%% %9.1f|%-9.1f %6.1f %9.1f",
							 n,
							 dgroup_total_disks,
							 total_busy / dgroup_total_disks,
							 total_rbytes,
							 total_wbytes,
							 (((double)total_rbytes + (double)total_wbytes)) / 1024, /* in MB */
							 total_xfers
							 );
					}
					display(paddg, 3 + dgroup_total_groups);
				} else {
					if (dgroup_loaded == 2) {
						fprintf(fp, show_rrd ? "rrdtool update dgbusy.rdd %s" : "DGBUSY,%s", LOOP);
						for (k = 0; k < dgroup_total_groups; k++) {
							if (dgroup_name[k] != 0) {
								disk_total = 0.0;
								for (j = 0; j < dgroup_disks[k]; j++) {
									i = dgroup_data[k*DGROUPS+j];
									if (i != -1) {
										disk_total += DKDELTA(dk_time) / elapsed;
									}
								}
								fprintf(fp, show_rrd ? ":%.1f" : ",%.1f", (float)(disk_total / dgroup_disks[k]));
							}
						}
						fprintf(fp, "\n");
						fprintf(fp, show_rrd ? "rrdtool update dgread.rdd %s" : "DGREAD,%s", LOOP);
						for (k = 0; k < dgroup_total_groups; k++) {
							if (dgroup_name[k] != 0) {
								disk_total = 0.0;
								for (j = 0; j < dgroup_disks[k]; j++) {
									i = dgroup_data[k*DGROUPS+j];
									if (i != -1) {
										disk_total += DKDELTA(dk_reads) * p->dk[i].dk_bsize / 1024.0;
									}
								}
								fprintf(fp, show_rrd ? ":%.1f" : ",%.1f", disk_total / elapsed);
							}
						}
						fprintf(fp, "\n");
						fprintf(fp, show_rrd ? "rrdtool update dgwrite.rdd %s" : "DGWRITE,%s", LOOP);
						for (k = 0; k < dgroup_total_groups; k++) {
							if (dgroup_name[k] != 0) {
								disk_total = 0.0;
								for (j = 0; j < dgroup_disks[k]; j++) {
									i = dgroup_data[k*DGROUPS+j];
									if (i != -1) {
										disk_total += DKDELTA(dk_writes) * p->dk[i].dk_bsize / 1024.0;
									}
								}
								fprintf(fp, show_rrd ? ":%.1f" : ",%.1f", disk_total / elapsed);
							}
						}
						fprintf(fp, "\n");
						fprintf(fp, show_rrd ? "rrdtool update dgbsize.rdd %s" : "DGSIZE,%s", LOOP);
						for (k = 0; k < dgroup_total_groups; k++) {
							if (dgroup_name[k] != 0) {
								disk_write = 0.0;
								disk_xfers  = 0.0;
								for (j = 0; j < dgroup_disks[k]; j++) {
									i = dgroup_data[k*DGROUPS+j];
									if (i != -1) {
										disk_write += (DKDELTA(dk_reads) + DKDELTA(dk_writes) ) * p->dk[i].dk_bsize / 1024.0;
										disk_xfers  += DKDELTA(dk_xfers);
									}
								}
								if ( disk_write == 0.0 || disk_xfers == 0.0)
									disk_size = 0.0;
								else
									disk_size = disk_write / disk_xfers;
								fprintf(fp, show_rrd ? ":%.1f" : ",%.1f", disk_size);
							}
						}
						fprintf(fp, "\n");
						fprintf(fp, show_rrd ? "rrdtool update dgxfer.rdd %s" : "DGXFER,%s", LOOP);
						for (k = 0; k < dgroup_total_groups; k++) {
							if (dgroup_name[k] != 0) {
								disk_total = 0.0;
								for (j = 0; j < dgroup_disks[k]; j++) {
									i = dgroup_data[k*DGROUPS+j];
									if (i != -1) {
										disk_total  += DKDELTA(dk_xfers);
									}
								}
								fprintf(fp, show_rrd ? ":%.1f" : ",%.1f", disk_total / elapsed);
							}
						}
						fprintf(fp, "\n");
					}
				}
			}
		} //End for loop to loop through enabled options - top mode is after this so that it will always be at the end
	
		if (enabled_option(SHOW_TOP)) {
			/* Get the details of the running processes */
			firstproc = 0;
			skipped = 0;
			n = getprocs(0);
			if (n > p->nprocs) {
				n = n +128; /* allow for growth in the number of processes in the mean time */
				p->procs = realloc(p->procs, sizeof(struct procsinfo ) * (n+1) ); /* add one to avoid overrun */
				p->nprocs = n;
			}

			firstproc = 0;
                        n = getprocs(1);

			if (topper_size < n) {
				topper = realloc(topper, sizeof(struct topper ) * (n+1) ); /* add one to avoid overrun */
				topper_size = n;
			}
			/* Sort the processes by CPU utilisation */
			for ( i = 0, max_sorted = 0; i < n; i++) {
				/* move forward in the previous array to find a match*/
				for(j=0;j < q->nprocs;j++) {
				    if (p->procs[i].pi_pid == q->procs[j].pi_pid) { /* found a match */
					topper[max_sorted].index = i;
					topper[max_sorted].other = j;
					topper[max_sorted].time =  TIMEDELTA(pi_utime,i,j) + 
								   TIMEDELTA(pi_stime,i,j);
					topper[max_sorted].size =  p->procs[i].statm_resident;

					max_sorted++;
					break;
				    }
				}
			}
			switch(show_topmode) {
			default:
			case 3: qsort((void *) & topper[0], max_sorted, sizeof(struct topper ), &cpu_compare );
				break;
			case 4: qsort((void *) & topper[0], max_sorted, sizeof(struct topper ), &size_compare );
				break;
#ifdef DISK
			case 5: qsort((void *) & topper[0], max_sorted, sizeof(struct topper ), &disk_compare );
				break;
#endif /* DISK */
			}
			CURSE BANNER(padtop,"Top Processes");
			CURSE mvwprintw(padtop,0, 15, "Procs=%d mode=%d (1=Basic, 3=Perf 4=Size 5=I/O)", n, show_topmode);
			if(cursed && first_time) {
				first_time = 0;
				mvwprintw(padtop,1, 1, "please wait - information being collected");
			}
			else {
			switch (show_topmode) {
			case 1:
				CURSE mvwprintw(padtop,1, 1, "  PID      PPID  Pgrp Nice Prior Status    proc-Flag Command");
				for (j = 0; j < max_sorted; j++) {
					i = topper[j].index;
					if (p->procs[i].pi_pgrp == p->procs[i].pi_pid)
						strcpy(pgrp, "none");
					else
						sprintf(&pgrp[0], "%d", p->procs[i].pi_pgrp);
					/* skip over processes with 0 CPU */
					if(!show_all && (topper[j].time/elapsed < ignore_procdisk_threshold) && !cmdfound) 
						break;
					    //if( x + j + 2 - skipped > LINES+2) /* +2 to for safety :-) */
						//break;
					CURSE mvwprintw(padtop,j + 2 - skipped, 1, "%7d %7d %6s %4d %4d %9s 0x%08x %1s %-32s",
					    p->procs[i].pi_pid,
					    p->procs[i].pi_ppid,
					    pgrp,
					    p->procs[i].pi_nice,
					    p->procs[i].pi_pri,

					    (topper[j].time * 100 / elapsed) ? "Running "
					     : get_state(p->procs[i].pi_state),
					    p->procs[i].pi_flags,
					    (p->procs[i].pi_tty_nr ? "F" : " "),
					    p->procs[i].pi_comm);
				}
				break;
			case 3:
			case 4:
			case 5:

				if(show_args == ARGS_ONLY) 
					formatstring = "  PID    %%CPU ResSize    Command                                            ";

				else if(COLS > 119)
					formatstring = "  PID       %%CPU    Size     Res    Res     Res     Res    Shared    Faults  Command";
				else
					formatstring = "  PID    %%CPU  Size   Res   Res   Res   Res Shared   Faults Command";
				CURSE mvwprintw(padtop,1, y_1, formatstring);

				if(show_args == ARGS_ONLY)
					formatstring = "         Used      KB                                                        ";
				else if(COLS > 119)
					formatstring = "            Used      KB     Set    Text    Data     Lib    KB     Min   Maj";
				else
					formatstring = "         Used    KB   Set  Text  Data   Lib    KB  Min  Maj ";
				CURSE mvwprintw(padtop,2, 1, formatstring);
				for (j = 0; j < max_sorted; j++) {
					i = topper[j].index;
					if(!show_all) { 
							/* skip processes with zero CPU/io */
						if(show_topmode == 3 && (topper[j].time/elapsed) < ignore_procdisk_threshold && !cmdfound)
							break;
						if(show_topmode == 5 && (topper[j].io < ignore_io_threshold && !cmdfound))
							break;
					}
					if(cursed) {
					    //if( x + j + 3 - skipped > LINES+2) /* +2 to for safety :-) */
						//break;
					    if(cmdfound && !cmdcheck(p->procs[i].pi_comm)) {
						skipped++;
					    	continue;
					    }
					  if(show_args == ARGS_ONLY){
					    mvwprintw(padtop,j + 3 - skipped, 1, 
					    "%7d %5.1f %7lu %-120s",
					    p->procs[i].pi_pid,
					    topper[j].time / elapsed,
					    p->procs[i].statm_resident*4,
					    args_lookup(p->procs[i].pi_pid,
							p->procs[i].pi_comm));
					  }
					  else {
					if(COLS > 119)
					    formatstring = "%8d %7.1f %7lu %7lu %7lu %7lu %7lu %5lu %6d %6d %-32s";
					else
					    formatstring = "%7d %5.1f %5lu %5lu %5lu %5lu %5lu %5lu %4d %4d %-32s";
					    mvwprintw(padtop,j + 3 - skipped, 1, formatstring,
					    p->procs[i].pi_pid,
					    topper[j].time/elapsed,
	/* topper[j].time /1000.0 / elapsed,*/
					    p->procs[i].statm_size*4 ,
					    p->procs[i].statm_resident*4,
					    p->procs[i].statm_trs*4,
					    p->procs[i].statm_drs*4,
					    p->procs[i].statm_lrs*4,
					    p->procs[i].statm_share*4,
					    (int)(COUNTDELTA(pi_minflt) / elapsed),
					    (int)(COUNTDELTA(pi_majflt) / elapsed),
					    p->procs[i].pi_comm);
					  }
					}
					else {
					    if((cmdfound && cmdcheck(p->procs[i].pi_comm)) || 
						(!cmdfound && ((topper[j].time / elapsed) > ignore_procdisk_threshold)) )
						 {
					    fprintf(fp,"TOP,%07d,%s,%.1f,%.1f,%.1f,%lu,%lu,%lu,%lu,%lu,%d,%d,%s\n",
					    /* 1 */ p->procs[i].pi_pid,
					    /* 2 */ LOOP,
					    /* 3 */ topper[j].time / elapsed,
                                            /* 4 */ TIMEDELTA(pi_utime,i,topper[j].other) / elapsed,
                                            /* 5 */ TIMEDELTA(pi_stime,i,topper[j].other) / elapsed,
					    /* 6 */ p->procs[i].statm_size*4,
					    /* 7 */ p->procs[i].statm_resident*4,
					    /* 8 */ p->procs[i].statm_trs*4,
					    /* 9 */ p->procs[i].statm_drs*4,
					    /* 10*/ p->procs[i].statm_share*4,
					    /* 11*/ (int)(COUNTDELTA(pi_minflt) / elapsed),
					    /* 12*/ (int)(COUNTDELTA(pi_majflt) / elapsed),
					    /* 13*/ p->procs[i].pi_comm);

					    if(show_args)
						args_output(p->procs[i].pi_pid,loop, p->procs[i].pi_comm);
					    }
					}
				}
				break;
			    }
			}
			CURSE display(padtop,j + 3 - skipped);
		}

		if(cursed) {
                        if(enabled_option(SHOW_VERBOSE)) {
				num_col_temp = num_col;
				y_1=x_1;
				x_1=1;
				display(padverb,4);
				x_1=y_1;
				num_col = num_col_temp;
			}
			wmove(stdscr,0, 0);
			wrefresh(stdscr);
			doupdate();
		
			column_check=0;  
			for (i = 0; i < seconds; i++) {
				if (column_check == 0) {     //Only check this stuff 1 time per screen refresh
	        			if (num_col != last_num_col){
	        				if (x_3 > 1 ){
	        				        last_num_col = 3;
	        				} else if (x_2 > 1){
	        				        last_num_col = 2;
	        				} else{
	        				        last_num_col = 1;
	        				}
	        			        clear();
						break;
	        			}
	        			if (x_3 > 1 ){
	        			        last_num_col = 3;
	        			} else if (x_2 > 1){
	        			        last_num_col = 2;
	        			} else{
	        			        last_num_col = 1;
	        			}
					column_check = 1;
				}
				usleep(100000);   // 1/10 of a second
				int result = checkinput();
				if (result == 2){   //An arrow key was pressed so we only want to update the help menu, not the entire screen
					num_col_temp = num_col;
					y_1=x_1;
					if (enabled_option(SHOW_VERBOSE)) {  //help menu is below the verbose window
						x_1 = 7;
					} else{
						x_1=1;
					}
					displayhelpmenu(padhelp);
					display(padhelp,11);
					wmove(stdscr,0, 0);
					wrefresh(stdscr);
					doupdate();
					x_1=y_1;
					num_col = num_col_temp;
				}
				if (result == 1)   //A key was pressed so break out.  
					break;
				
			}
		}
		else {
			fflush(NULL);
			secs = seconds; 
redo:
			errno = 0;
			ret = sleep(secs / 10); 
			if( ret != 0 || errno != 0) {
				fprintf(fp,"sleep got interrupted\n");
				fprintf(fp,"ret was %d\n",ret);
				fprintf(fp,"errno was %d\n",errno);
				secs=ret;
				goto redo;
			}
		}

		switcher();

		if (loop >= maxloops) {
			CURSE endwin();
                        if (nmon_end) {
                                child_start(CHLD_END, nmon_end, time_stamp_type, loop, timer);
                                /* Give the end - processing some time - 5s for now */
                                sleep(5);
                        }

			fflush(NULL);
			exit(0);
		}
	}
}
