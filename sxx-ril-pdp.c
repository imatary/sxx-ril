#include <telephony/ril.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/route.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <cutils/properties.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <linux/wait.h>

#include "sxx-ril.h"

void onDataCallListChanged(void *param);
extern int getZmdsValue();
extern void reportSignalStrength(void *param);

#define SIGNAL_POLL_DELAY 60
#define MAX_EVENTS 1024
#define PPP_TTY_PATH "ppp0"
#define PPP_INTERFACE  "ppp0"
#define SYS_NET_PATH   "/sys/class/net"

// pppd manager struct
typedef struct
{
	int epfd;
	int rfd;
	int wfd;
	int pppid;
	int stm;
}pm_t;

typedef enum
{
	PPPD_STM_IDLE = 0,
	PPPD_STM_STARTING,
	PPPD_STM_RUNNING,
	PPPD_STM_EXCEPTION,
	PPPD_STM_STOPPING,
    PPPD_STM_HALTING,
}PPPD_STM_STATE;

typedef enum
{
	PPPD_SIG_START,
	PPPD_SIG_STOP,
	PPPD_SIG_TIMER,
	PPPD_SIG_EXIT,
	PPPD_SIG_WAKEUP,
    PPPD_SIG_HALT,
    PPPD_SIG_RESTART,
}PPPD_SIG_TYPE;

typedef struct
{
	char apn[128];
	char user[128];
	char password[128];
	RIL_Token t;
}dc_data_t;


static pm_t g_pm;
static char g_dns1[32], g_dns2[32], g_dnses[64], g_local_ip[64], g_remote_ip[64];
static char g_user[128] = {0};
static char g_password[128] = {0};
static char g_device[128] = {0};
static char g_apn[128] = {0};
static struct itimerval g_oldtv;
static int signal_poll_delay = SIGNAL_POLL_DELAY;
static int s_lastPdpFailCause = PDP_FAIL_ERROR_UNSPECIFIED;// PDP_FAIL_USER_AUTHENTICATION ;

static int get_if_flags( const char *interface, unsigned *flags)
{  
	DIR  *dir_path = NULL;
	struct ifreq ifr;
	struct dirent *de;
	int query_sock = -1;
	int ret = -1;

	query_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(query_sock < 0){
		LOGD("failed to create query interface socket");
		goto result;
	}
	dir_path = opendir(SYS_NET_PATH);
	if(dir_path == 0){
		LOGD("failed to opendir %s",SYS_NET_PATH);
		goto result;
	}

	while((de = readdir(dir_path))){
		if(strcmp(de->d_name, interface) == 0){
			memset(&ifr, 0x00, sizeof(struct ifreq));
			strncpy((char *)&(ifr.ifr_name), interface, IFNAMSIZ);
			ifr.ifr_name[IFNAMSIZ -1] = 0x00;
			if (flags != NULL) {
				if(ioctl(query_sock, SIOCGIFFLAGS, &ifr) < 0) {
					*flags = 0;
				} else {
					*flags = ifr.ifr_flags;
				}
			}
			ret = 0;
			goto result;
		}
	}
result:
	if(query_sock != -1){
		close(query_sock);
		query_sock = -1;
	}
	if(dir_path){
		closedir(dir_path);
		dir_path = NULL;
	}

	return ret;
}
static int pppd_connect_status()
{
	int flags = 0;
	if (g_pm.pppid == 0) 
		return 0;

	get_if_flags(PPP_INTERFACE, &flags);
	return flags & 0x01;
}

static int add_event(int epfd,int fd,int ev_set)
{
	struct epoll_event ev;
	memset(&ev,0,sizeof(ev));
	ev.data.fd  = fd;
	ev.events   = ev_set;
	if(epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&ev) == -1)
		return -1;
	return 0;
}

static int wait_event(int epfd,struct epoll_event *e)
{
	return epoll_wait(epfd,e,MAX_EVENTS,-1);
}

static int del_event(int epfd,int fd,int ev_set)
{
	close(fd);
	if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, 0) == -1){
		MYLOG("Delete event failed!!!!");
		return -1;
	}
	return 0;
}

// just send a start signal to pppd manager so that 
// pppd launchering process does not block ril stack 

static int start_pppd(char* user, char* password)
{
	int sig = PPPD_SIG_START;

	if (user) { 
		strncpy(g_user, user, 128);
	} else {
		memset(g_user, 0, 128);
	}

	if (password) {
		strncpy(g_password, password, 128);
	} else {
		memset(g_password, 0, 128);
	}

	return write(g_pm.wfd, &sig, 4);
}

int stop_pppd()
{
	int sig = PPPD_SIG_STOP;
	return write(g_pm.wfd, &sig, 4);
}

void wait_for_pppd_down()
{
	while(g_pm.pppid != 0) {
		LOGD("wait for pppd down");
		sleep(1);
	}
}
//
static void signal_handler(int s)
{
	int i;
	int sig;
	if (s == SIGCHLD) {
		int pid = wait(NULL); 
		if(pid == g_pm.pppid) {
			waitpid(g_pm.pppid, &i, 0);
			sig = PPPD_SIG_EXIT;
			g_pm.pppid = 0;
		}
	} else if (s == SIGALRM) {
		sig = PPPD_SIG_TIMER;
	}

	write(g_pm.wfd, &sig, 4);
}

static int _start_pppd()
{
	int pid;
	char *ppp_params[] = {
		"pppd", //0
		get_data_port(), //1
		"115200", //2
		"crtscts", //3
		"connect",  //4
		get_chat_option(), //5
		"debug", //6 
		"nodetach", //7
		"ipcp-accept-local", //8
		"ipcp-accept-remote", //9
		"local", //10
		"usepeerdns", //11
		"defaultroute", //12
		"novj",//13
		"novjccomp", //14
		"noipdefault", //15
		"user", //16
		g_user, //17
		"password", //18
		g_password,// 19
		"lcp-max-configure",
		"2",
        "ipcp-max-configure",
        "15",
		NULL,
		NULL,
	};

	if ((pid = fork()) < 0) {
		LOGD("failed to fork process\n");
		return -1;

	} else if (pid == 0) {
		if(execve("/system/bin/pppd", ppp_params, environ) < 0 ){
			LOGE("Failed to execute the pppd process %d\n",errno);
			return -1;

		} else {
			LOGE("Success to execute the pppd process \n");
		}	

	} else {
		g_pm.pppid = pid;	
	}

	return 0;
}


static int _stop_pppd()
{
	int i;
	if (g_pm.pppid != 0) {
		LOGD("############ stop %d", g_pm.pppid);
		at_send_command("AT+CGACT=0", NULL);
		kill(g_pm.pppid, SIGHUP);
		waitpid(g_pm.pppid, &i, WUNTRACED | WCONTINUED);
		g_pm.pppid = 0;
	}
	return 0;
}

void halt_pppd()
{
    int sig = PPPD_SIG_HALT;
    write(g_pm.wfd, &sig, 4);
    wait_for_pppd_down();
}
void restart_pppd()
{
    int sig = PPPD_SIG_RESTART;
    write(g_pm.wfd, &sig, 4);
}

static int start_timer()
{
	struct itimerval itv;

	//start timer immediately
	itv.it_value.tv_sec = 1;
	itv.it_value.tv_usec = 0;

	//call signal handler every 500ms
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 500000;


	setitimer(ITIMER_REAL, &itv, &g_oldtv);
	return 0;
}

static char* stm_to_string(int stm)
{
	switch(stm){
		case PPPD_STM_IDLE: return "PPPD_STM_IDLE";
		case PPPD_STM_STARTING: return "PPPD_STM_STARTING";
		case PPPD_STM_RUNNING: return "PPPD_STM_RUNNING";
		case PPPD_STM_EXCEPTION: return "PPPD_STM_EXCEPTION";
		case PPPD_STM_STOPPING: return "PPPD_STM_STOPPING";
        case PPPD_STM_HALTING: return "PPPD_STM_HALTING";
	}
	return "UNKOWN_STATE";
}
static char* sig_to_string(int sig)
{
	switch(sig){
		case PPPD_SIG_START: return "PPPD_SIG_START";
		case PPPD_SIG_STOP: return "PPPD_SIG_STOP";
		case PPPD_SIG_EXIT: return "PPPD_SIG_EXIT";
		case PPPD_SIG_TIMER: return "PPPD_SIG_TIMER";
        case PPPD_SIG_HALT: return "PPPD_STM_HALT";
        case PPPD_SIG_RESTART: return "PPPD_STM_RESTART";
	}
	return "UNKOWN_SIG";	
}

static int check_connect(char *dst)  
{  
	int i = 0;  
	FILE *stream;  
	char recvBuf[16] = {0};  
	char cmdBuf[256] = {0};  

	if (NULL == dst)  
		return -1;  

	sprintf(cmdBuf, "ping -w 5 -c 1 -i 0.2 %s | busybox grep time= | busybox wc -l", dst);
	stream = popen(cmdBuf, "r"); 
	if(stream == NULL){
		LOGD("cannot open stream : %s", strerror(errno));
		return -1;
	}

	fread(recvBuf, sizeof(char), sizeof(recvBuf)-1, stream);  
	pclose(stream);
	if (atoi(recvBuf) > 0)  
		return 0;  

	return -1;  
}
static int get_connection_status()
{
	ATResponse *p_response = NULL;
	int conn = 0;
	int err = at_send_command_singleline("AT+CPAS", "+CPAS:", &p_response);
	if (err != 0 || p_response->success == 0 || p_response->p_intermediates == NULL) {
		goto error;
	} else {
		char *line = p_response->p_intermediates->line;
		err = at_tok_start(&line);
		if (err < 0)
			goto error;

		at_tok_nextint(&line, &conn);

	}
error:
	at_response_free(p_response);
	return !!conn;
}


static void* pm_thread(void* ptr)
{
	struct epoll_event ev;
	add_event(g_pm.epfd, g_pm.rfd, EPOLLIN|EPOLLET);

	while(1){
		//trigger source
		//SIG_TIMER : 500ms per second
		//SIG_START,SIG_STOP : by request

		wait_event(g_pm.epfd, &ev);
		if (ev.data.fd != g_pm.rfd)
			continue;

		int sig = -1;
		int n;
		int old_stm = g_pm.stm;

        //ignore some timer signal
		do {
            n = read(g_pm.rfd, &sig, 4);
        }while(n > 0 && sig == PPPD_SIG_TIMER);

		if (sig == PPPD_SIG_TIMER) {
			signal_poll_delay--;
			if (signal_poll_delay == 0) {
				signal_poll_delay = SIGNAL_POLL_DELAY;
                hide_info(1);
				reportSignalStrength(NULL);
                hide_info(0);
			}
            modem_periodic();
		}
		switch(g_pm.stm){
			case PPPD_STM_IDLE:
				if (sig == PPPD_SIG_START) {
					g_pm.stm = PPPD_STM_STARTING;
					_start_pppd();
				}
				break;
			case PPPD_STM_STARTING:
				if (sig == PPPD_SIG_TIMER &&
						pppd_connect_status()) {
					g_pm.stm = PPPD_STM_RUNNING;
				}

				if (sig == PPPD_SIG_EXIT) {
					g_pm.stm = PPPD_STM_EXCEPTION;
				}

				if (sig == PPPD_SIG_STOP) {
					g_pm.stm = PPPD_STM_STOPPING;
				}
				break;
			case PPPD_STM_RUNNING:
				if ((sig == PPPD_SIG_TIMER &&
							pppd_connect_status() == 0)) {
					g_pm.stm = PPPD_STM_EXCEPTION;
				}

				if (sig == PPPD_SIG_EXIT) {
					g_pm.stm = PPPD_STM_STARTING;
					_start_pppd();
				}

				if (sig == PPPD_SIG_STOP) {
					g_pm.stm = PPPD_STM_STOPPING;
				}

				if (sig == PPPD_SIG_WAKEUP) {
					if( get_connection_status() == 0){
						LOGD("Lost gprs connection");
						g_pm.stm = PPPD_STM_EXCEPTION;
					}
				}
            
                if (sig == PPPD_SIG_HALT) {
                    g_pm.stm = PPPD_STM_HALTING;
                    _stop_pppd();
                }

				break;
			case PPPD_STM_EXCEPTION:
				//restart when exception accurred
				if (sig == PPPD_SIG_TIMER) {
					_stop_pppd();

					if (g_pm.pppid == 0) {
						//until pppd exit, restart it
						_start_pppd();
						//do not restart the pppd by self, let the system kown that network 
						//has downed and do reconnect the network
						//RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
						g_pm.stm = PPPD_STM_STARTING;
						//g_pm.stm = PPPD_STM_IDLE;
					}
				}
				break;
			case PPPD_STM_STOPPING:
				if (sig == PPPD_SIG_TIMER) {
					_stop_pppd();
					if (g_pm.pppid == 0) {
						//until pppd exit
						g_pm.stm = PPPD_STM_IDLE;
					}
				}
				break;
            case PPPD_STM_HALTING:
                if (sig == PPPD_SIG_RESTART) {
                        _start_pppd();
                        g_pm.stm = PPPD_STM_STARTING;  
                }
                break;
		}

		if (old_stm != g_pm.stm) {
			LOGD("########## state machine change:%s->%s sig: %s ###########", stm_to_string(old_stm), 
					stm_to_string(g_pm.stm), sig_to_string(sig));	
		}	
	}
	return 0;
}

int init_pppd_manager()
{
	int pfd[2];
	g_pm.pppid = 0;
	g_pm.stm = PPPD_STM_IDLE;

	g_pm.epfd = epoll_create(MAX_EVENTS);
	if (g_pm.epfd < 0) {
		LOGE("cannot create epoll event");
		exit(1);		
	}
	pipe(pfd);
	if (pfd[0] < 0 || pfd[1] < 0) {
		LOGE("cannot create pipe fd");
		exit(1);	
	}
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	//fcntl(pfd[1], F_SETFL, O_NONBLOCK);

	g_pm.rfd = pfd[0];
	g_pm.wfd = pfd[1];

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init (&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&tid, NULL, pm_thread, NULL);

	//signal(SIGCHLD,SIG_IGN);

	signal(SIGCHLD,signal_handler);
	signal(SIGALRM,signal_handler);

	start_timer();
	return 0;
}

// report a fake contextList with suggestedRetryTime bigger than -1 
// so that framework will request data call setup again when pppd 
// connection not establish
//
void reportFakeDataCallList(RIL_Token *t)
{
	int n = 1;

	RIL_Data_Call_Response_v6 *responses =
		alloca(n * sizeof(RIL_Data_Call_Response_v6));

	int i;
	for (i = 0; i < n; i++) {
		responses[i].status = -1;
		responses[i].suggestedRetryTime = 2000; // retry in 2 seconds
		responses[i].cid = -1;
		responses[i].active = -1;
		responses[i].type = "";
		responses[i].ifname = "";
		responses[i].addresses = "";
		responses[i].dnses = "";
		responses[i].gateways = "";
	}
	RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
			n * sizeof(RIL_Data_Call_Response_v6));	
}

void reportDataCallList(RIL_Token *t)
{
	ATResponse *p_response;
	ATLine *p_cur;
	int err;
	int n = 0;
	char *out;

	err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
	if (err != 0 || p_response->success == 0) {
		if (t != NULL)
			RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
		else
			RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
					NULL, 0);
		return;
	}

	for (p_cur = p_response->p_intermediates; p_cur != NULL;
			p_cur = p_cur->p_next)
		n++;

    if (!get_connection_status())
        n = 0;

	RIL_Data_Call_Response_v6 *responses =
		alloca(n * sizeof(RIL_Data_Call_Response_v6));

	int i;
	for (i = 0; i < n; i++) {
		responses[i].status = -1;
		responses[i].suggestedRetryTime = -1; // retry in 2 seconds
		responses[i].cid = -1;
		responses[i].active = -1;
		responses[i].type = "";
		responses[i].ifname = "";
		responses[i].addresses = "";
		responses[i].dnses = "";
		responses[i].gateways = "";
	}

	RIL_Data_Call_Response_v6 *response = responses;
	for (p_cur = p_response->p_intermediates; p_cur != NULL;
			p_cur = p_cur->p_next) {
		char *line = p_cur->line;

		err = at_tok_start(&line);
		if (err < 0)
			goto error;

		err = at_tok_nextint(&line, &response->cid);
		if (err < 0)
			goto error;

		err = at_tok_nextint(&line, &response->active);
		if (err < 0)
			goto error;

		response++;
	}

	at_response_free(p_response);

	err = at_send_command_multiline ("AT+CGDCONT?", "+CGDCONT:", &p_response);


	if (err != 0 || p_response->success == 0) {
		if (t != NULL)
			RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
		else
			RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
					NULL, 0);
		return;
	}

	// when pppd connection established, report the network status
	for (p_cur = p_response->p_intermediates; p_cur != NULL;
			p_cur = p_cur->p_next) {
		char *line = p_cur->line;
		int cid;

		err = at_tok_start(&line);
		if (err < 0)
			goto error;

		err = at_tok_nextint(&line, &cid);
		if (err < 0)
			goto error;

		for (i = 0; i < n; i++) {
			if (responses[i].cid == cid)
				break;
		}

		if (i >= n) {
			/* details for a context we didn't hear about in the last request */
			continue;
		}

		//assume no error
		responses[i].status = 0;

		// type
		err = at_tok_nextstr(&line, &out);
		if (err < 0)
			goto error;
		responses[i].type = alloca(strlen(out) + 1);
		strcpy(responses[i].type, out);

		// APN ignored for v5
		err = at_tok_nextstr(&line, &out);
		if (err < 0)
			goto error;

		responses[i].ifname = alloca(strlen(PPP_TTY_PATH) + 1);
		strcpy(responses[i].ifname, PPP_TTY_PATH);

		err = at_tok_nextstr(&line, &out);
		if (err < 0)
			goto error;

		//responses[i].addresses = alloca(strlen(out) + 1);
		//strcpy(responses[i].addresses, out);


		property_get("net.ppp0.dns1", g_dns1, "8.8.8.8");
		property_get("net.ppp0.dns2", g_dns2, "8.8.4.4");
		property_get("net.ppp0.local-ip", g_local_ip, "0.0.0.0");
		property_get("net.ppp0.remote-ip", g_remote_ip, "10.64.64.64");

		responses[i].addresses = alloca(strlen(g_local_ip) + 1);
		strcpy(responses[i].addresses, g_local_ip);	

		if(!strcmp(g_dns1,"10.11.12.13") || !strcmp(g_dns1,"10.11.12.14")){
			responses[i].dnses = "8.8.8.8 8.8.4.4";
		}else{
			sprintf(g_dnses, "%s", g_dns1);
			responses[i].dnses = g_dnses;
		}

		responses[i].dnses = "8.8.8.8 8.8.4.4";
		responses[i].gateways = g_local_ip;
	}

	at_response_free(p_response);

	if (t != NULL)
		RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
				n * sizeof(RIL_Data_Call_Response_v6));
	else
		RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
				responses,
				n * sizeof(RIL_Data_Call_Response_v6));

	return;

error:
	if (t != NULL)
		RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
				NULL, 0);

	at_response_free(p_response);
}


void onDataCallListChanged(void *param)
{
	reportDataCallList(NULL);
}


void requestDataCallList(void *data, size_t datalen, RIL_Token t)
{
	reportDataCallList(&t);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_DATA_CALL_LIST, requestDataCallList)

int need_network_fix = 0;
// data call thread
static void* dc_thread(void* ptr)
{
	dc_data_t* dcd = (dc_data_t*)ptr;
	RIL_Token t = dcd->t;

	const char *apn = dcd->apn;
	char *cmd;
	int err = 0;

	asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\",,0,0", apn);

	err = at_send_command(cmd, NULL);
	free(cmd);
	
	at_send_command("AT+CGEREP=0,0", NULL);
	if (apn && strcmp(g_apn, apn) && pppd_connect_status()) {
		stop_pppd();
		while(pppd_connect_status()){
			LOGD("wait for connection down");
			sleep(1);	
		}
	}
	strncpy(g_apn, apn, 127);
	start_pppd(dcd->user, dcd->password);

	int timeout = 30;
	while(!pppd_connect_status()) {
		LOGD("wait for connection up");
		sleep(1);
		if(timeout-- < 0)
			break;
	}

	if (timeout < 0) {
        stop_pppd();
		reportFakeDataCallList(&t);
		goto out;
	}

    if (need_network_fix == 1) {
	    int zv = getZmdsValue();
	    if (zv == 13) {
		    select_tech(0);
	    }
    }
	reportDataCallList(&t);

	at_send_command("AT+CGEREP=1,0", NULL);

out:
	free(ptr);
	return NULL;	
}

void launch_data_call(void *data, size_t datalen, RIL_Token t)
{
	dc_data_t* dcd = (dc_data_t*)malloc(sizeof(dc_data_t));
	
	memset(dcd, 0, sizeof(dc_data_t));
	char* apn = ((const char **)data)[2];
	char* user = ((const char **)data)[3];
	char* password = ((const char **)data)[4];
	
	if (apn)
		strncpy(dcd->apn, apn, 127);
	if (user)
		strncpy(dcd->user, user, 127);
	if (password)
		strncpy(dcd->password, password, 127);

	dcd->t = t;

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init (&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&tid, NULL, dc_thread, dcd);
}

void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
	launch_data_call(data, datalen, t);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_SETUP_DATA_CALL, requestSetupDataCall)

void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
	stop_pppd();
    wait_for_pppd_down();
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    RIL_requestTimedCallback (onDataCallListChanged, NULL, &TIMEVAL_3);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_DEACTIVATE_DATA_CALL, requestDeactivateDataCall)


void requestLastDataCallFailCause(void *data, size_t datalen, RIL_Token t)
{
	RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_lastPdpFailCause,sizeof(int));
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE, requestLastDataCallFailCause)


static int onDataCallStageChanged(char* s, char* sms_pdu)
{
	RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
	return UNSOLICITED_SUCCESSED;
}
REGISTER_DEFAULT_UNSOLICITED(CGEV, "+CGEV:", onDataCallStageChanged)

void clearSmsCache()
{
	ATResponse *p_response;
	ATLine *p_cur;
	at_send_command("AT+CMGL",NULL);
}
void requestScreenState(void *data, size_t datalen, RIL_Token t)
{
	int on =  ((int *)data)[0];
	LOGD("*** SCREEN_STATE = %s ***",on ?"Screen On":"Screen Off");
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	if(on){
		RIL_onUnsolicitedResponse (
				RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
				NULL, 0);
		clearSmsCache();

		int sig = PPPD_SIG_WAKEUP;
		write(g_pm.wfd, &sig, 4);
	}
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_SCREEN_STATE, requestScreenState)




