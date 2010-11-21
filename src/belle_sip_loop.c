/*
	belle-sip - SIP (RFC3261) library.
    Copyright (C) 2010  Belledonne Communications SARL

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "belle-sip/belle-sip.h"
#include "belle_sip_internal.h"

#include <unistd.h>
#include <poll.h>


void belle_sip_source_destroy(belle_sip_source_t *obj){
	if (obj->node.next || obj->node.prev){
		belle_sip_fatal("Destroying source currently used in main loop !");
	}
	belle_sip_free(obj);
}

void belle_sip_fd_source_init(belle_sip_source_t *s, belle_sip_source_func_t func, void *data, int fd, unsigned int events, unsigned int timeout_value_ms){
	static unsigned long global_id=1;
	s->id=global_id++;
	s->fd=fd;
	s->events=events;
	s->timeout=timeout_value_ms;
	s->data=data;
	s->notify=func;
}

static belle_sip_source_t * belle_sip_fd_source_new(belle_sip_source_func_t func, void *data, int fd, unsigned int events, unsigned int timeout_value_ms){
	belle_sip_source_t *s=belle_sip_new0(belle_sip_source_t);
	belle_sip_fd_source_init(s,func,data,fd,events,timeout_value_ms);
	return s;
}

belle_sip_source_t * belle_sip_timeout_source_new(belle_sip_source_func_t func, void *data, unsigned int timeout_value_ms){
	return belle_sip_fd_source_new(func,data,-1,0,timeout_value_ms);
}

struct belle_sip_main_loop{
	belle_sip_source_t *sources;
	belle_sip_source_t *control;
	int nsources;
	int run;
	int control_fds[2];
};

static int main_loop_done(void *data, unsigned int events){
	belle_sip_debug("Got data on control fd...");
	return TRUE;
}

belle_sip_main_loop_t *belle_sip_main_loop_new(void){
	belle_sip_main_loop_t*m=belle_sip_new0(belle_sip_main_loop_t);
	if (pipe(m->control_fds)==-1){
		belle_sip_fatal("Could not create control pipe.");
	}
	m->control=belle_sip_fd_source_new (main_loop_done,NULL,m->control_fds[0],BELLE_SIP_EVENT_READ,-1);
	belle_sip_main_loop_add_source(m,m->control);
	return m;
}

void belle_sip_main_loop_add_source(belle_sip_main_loop_t *ml, belle_sip_source_t *source){
	if (source->node.next || source->node.prev){
		belle_sip_fatal("Source is already linked somewhere else.");
		return;
	}
	if (source->timeout>0){
		source->expire_ms=belle_sip_time_ms()+source->timeout;
	}
		
	ml->sources=(belle_sip_source_t*)belle_sip_list_append_link((belle_sip_list_t*)ml->sources,(belle_sip_list_t*)source);
	ml->nsources++;
}

void belle_sip_main_loop_remove_source(belle_sip_main_loop_t *ml, belle_sip_source_t *source){
	ml->sources=(belle_sip_source_t*)belle_sip_list_remove_link((belle_sip_list_t*)ml->sources,(belle_sip_list_t*)source);
	ml->nsources--;
	if (source->on_remove)
		source->on_remove(source);
}

unsigned long belle_sip_main_loop_add_timeout(belle_sip_main_loop_t *ml, belle_sip_source_func_t func, void *data, unsigned int timeout_value_ms){
	belle_sip_source_t * s=belle_sip_timeout_source_new(func,data,timeout_value_ms);
	s->on_remove=belle_sip_source_destroy;
	belle_sip_main_loop_add_source(ml,s);
	return s->id;
}

/*
 Poll() based implementation of event loop.
 */

static int belle_sip_event_to_poll(unsigned int events){
	int ret=0;
	if (events & BELLE_SIP_EVENT_READ)
		ret|=POLLIN;
	if (events & BELLE_SIP_EVENT_WRITE)
		ret|=POLLOUT;
	if (events & BELLE_SIP_EVENT_ERROR)
		ret|=POLLERR;
	return ret;
}

static unsigned int belle_sip_poll_to_event(short events){
	unsigned int ret=0;
	if (events & POLLIN)
		ret|=BELLE_SIP_EVENT_READ;
	if (events & POLLOUT)
		ret|=BELLE_SIP_EVENT_WRITE;
	if (events & POLLERR)
		ret|=BELLE_SIP_EVENT_ERROR;
	return ret;
}

void belle_sip_main_loop_iterate(belle_sip_main_loop_t *ml){
	struct pollfd *pfd=(struct pollfd*)alloca(ml->nsources*sizeof(struct pollfd));
	int i=0;
	belle_sip_source_t *s,*next;
	uint64_t min_time_ms=(uint64_t)-1;
	int duration=-1;
	int ret;
	uint64_t cur;
	
	/*prepare the pollfd table */
	for(s=ml->sources;s!=NULL;s=(belle_sip_source_t*)s->node.next){
		if (s->fd!=-1){
			pfd[i].fd=s->fd;
			pfd[i].events=belle_sip_event_to_poll (s->events);
			pfd[i].revents=0;
			s->index=i;
			++i;
		}
		if (s->timeout>0){
			if (min_time_ms>s->expire_ms){
				min_time_ms=s->expire_ms;
			}
		}
	}
	
	if (min_time_ms!=(uint64_t)-1 ){
		/* compute the amount of time to wait for shortest timeout*/
		cur=belle_sip_time_ms();
		int64_t diff=min_time_ms-cur;
		if (diff>0)
			duration=(int)diff;
		else 
			duration=0;
	}
	/* do the poll */
	ret=poll(pfd,i,duration);
	if (ret==-1 && errno!=EINTR){
		belle_sip_error("poll() error: %s",strerror(errno));
		return;
	}
	cur=belle_sip_time_ms();
	/* examine poll results*/
	for(s=ml->sources;s!=NULL;s=next){
		unsigned revents=0;
		next=(belle_sip_source_t*)s->node.next;
		
		if (s->fd!=-1){
			if (pfd[s->index].revents!=0){
				revents=belle_sip_poll_to_event(pfd[s->index].revents);		
			}
		}
		if (revents!=0 || (s->timeout>0 && cur>=s->expire_ms)){
			ret=s->notify(s->data,revents);
			if (ret==0){
				/*this source needs to be removed*/
				belle_sip_main_loop_remove_source(ml,s);
			}else if (revents==0){
				/*timeout needs to be started again */
				s->expire_ms+=s->timeout;
			}
		}
	}
}

void belle_sip_main_loop_run(belle_sip_main_loop_t *ml){
	ml->run=1;
	while(ml->run){
		belle_sip_main_loop_iterate(ml);
	}
}

void belle_sip_main_loop_quit(belle_sip_main_loop_t *ml){
	ml->run=0;
	write(ml->control_fds[1],"a",1);
}

void belle_sip_main_loop_sleep(belle_sip_main_loop_t *ml, int milliseconds){
	belle_sip_main_loop_add_timeout(ml,(belle_sip_source_func_t)belle_sip_main_loop_quit,ml,milliseconds);
	belle_sip_main_loop_run(ml);
}

void belle_sip_main_loop_destroy(belle_sip_main_loop_t *ml){
	belle_sip_main_loop_remove_source (ml,ml->control);
	belle_sip_source_destroy(ml->control);
	close(ml->control_fds[0]);
	close(ml->control_fds[1]);
	belle_sip_free(ml);
}
